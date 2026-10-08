#include "hle/psp_psmf.h"

// Ported from PPSSPP (GPL-2.0-or-later): Core/HLE/scePsmf.cpp (Psmf::Psmf,
// PsmfStream::readMPEGVideoStreamParams / readPrivateAudioStreamParams),
// Core/HLE/sceMpeg.cpp (AnalyzeMpeg, getMpegVersion) and
// Core/HW/MediaEngine.h (getMpegTimeStamp). Every read is bounds-checked
// against the caller's buffer; PPSSPP reads fixed offsets unchecked.

#include <algorithm>

namespace {

/// Bounds-checked big/little-endian reads: bytes past `size` read as zero.
struct Reader {
    const uint8_t* d;
    size_t n;

    uint8_t u8(size_t off) const { return off < n ? d[off] : 0; }
    uint32_t le32(size_t off) const {
        return static_cast<uint32_t>(u8(off)) |
               static_cast<uint32_t>(u8(off + 1)) << 8 |
               static_cast<uint32_t>(u8(off + 2)) << 16 |
               static_cast<uint32_t>(u8(off + 3)) << 24;
    }
    uint32_t be32(size_t off) const {
        return static_cast<uint32_t>(u8(off)) << 24 |
               static_cast<uint32_t>(u8(off + 1)) << 16 |
               static_cast<uint32_t>(u8(off + 2)) << 8 |
               static_cast<uint32_t>(u8(off + 3));
    }
    uint32_t be16(size_t off) const {
        return static_cast<uint32_t>(u8(off)) << 8 | u8(off + 1);
    }
    int64_t timestamp(size_t off) const {
        uint8_t t[6];
        for (size_t i = 0; i < 6; i++) {
            t[i] = u8(off + i);
        }
        return psmf_read_timestamp(t);
    }
};

}  // namespace

int64_t psmf_read_timestamp(const uint8_t* p6) {
    return static_cast<int64_t>(p6[5]) | (static_cast<int64_t>(p6[4]) << 8) |
           (static_cast<int64_t>(p6[3]) << 16) | (static_cast<int64_t>(p6[2]) << 24) |
           (static_cast<int64_t>(p6[1]) << 32) | (static_cast<int64_t>(p6[0]) << 36);
}

int psmf_mpeg_version_index(uint32_t rawVersion) {
    switch (rawVersion) {
        case PSMF_VERSION_0012: return 0;
        case PSMF_VERSION_0013: return 1;
        case PSMF_VERSION_0014: return 2;
        case PSMF_VERSION_0015: return 3;
        default: return -1;
    }
}

PsmfStatus psmf_parse(const uint8_t* data, size_t size, PsmfHeader* out) {
    *out = PsmfHeader();
    if (!data || size < 0x10) {
        return PsmfStatus::TooShort;
    }
    const Reader r{data, size};

    out->magic = r.le32(0);
    if (out->magic != PSMF_MAGIC) {
        return PsmfStatus::BadMagic;
    }
    out->version = r.le32(PSMF_STREAM_VERSION_OFFSET);
    out->streamOffset = r.be32(PSMF_STREAM_OFFSET_OFFSET);
    out->streamSize = r.be32(PSMF_STREAM_SIZE_OFFSET);
    out->streamDataTotalSize = r.be32(0x50);
    out->firstTimestamp = r.timestamp(PSMF_FIRST_TIMESTAMP_OFFSET);
    out->lastTimestamp = r.timestamp(PSMF_LAST_TIMESTAMP_OFFSET);
    out->streamDataNextBlockSize = r.be32(0x6A);
    out->streamDataNextInnerBlockSize = r.be32(0x7C);
    out->numStreams = r.be16(PSMF_STREAM_COUNT_OFFSET);

    if (out->version == 0) {
        return PsmfStatus::BadVersion;
    }
    if (out->streamOffset == 0) {
        return PsmfStatus::BadStreamOffset;
    }

    for (uint32_t i = 0; i < out->numStreams; i++) {
        const size_t entry = PSMF_STREAM_TABLE_OFFSET + i * PSMF_STREAM_ENTRY_SIZE;
        if (entry + PSMF_STREAM_ENTRY_SIZE > size) {
            break;  // only complete entries are used
        }
        PsmfStreamInfo s;
        s.streamId = r.u8(entry);
        s.privateId = r.u8(entry + 1);
        if ((s.streamId & PSMF_VIDEO_STREAM_ID) == PSMF_VIDEO_STREAM_ID) {
            s.type = PSMF_AVC_STREAM;
            s.channel = s.streamId & 0x0F;
            // Entry layout: +4 EP map offset, +8 EP map entries, +12/+13
            // width/height in macroblocks.
            out->epMapOffset = r.be32(entry + 4);
            out->epMapEntries = r.be32(entry + 8);
            s.videoWidth = r.u8(entry + 12) * 16;
            s.videoHeight = r.u8(entry + 13) * 16;
            if (out->videoWidth == 0 && out->videoHeight == 0) {
                out->videoWidth = s.videoWidth;
                out->videoHeight = s.videoHeight;
            }
        } else if ((s.streamId & PSMF_AUDIO_STREAM_ID) == PSMF_AUDIO_STREAM_ID) {
            // A private stream id with a high nibble is not ATRAC; PPSSPP
            // assumes PCM.
            s.type = (s.privateId & 0xF0) != 0 ? PSMF_PCM_STREAM : PSMF_ATRAC_STREAM;
            s.channel = s.privateId & 0x0F;
            s.audioChannels = r.u8(entry + 14);
            s.audioFrequency = r.u8(entry + 15);
        } else {
            continue;  // neither audio nor video: PPSSPP numbers only A/V streams
        }
        out->streams.push_back(s);
    }

    // EP map (seek table): clipped to the buffer and to a fixed cap.
    const size_t ep_off = out->epMapOffset;
    if (out->epMapEntries != 0 && ep_off < size) {
        const size_t fit = (size - ep_off) / PSMF_EP_ENTRY_SIZE;
        const size_t count = std::min<size_t>(
            std::min<size_t>(out->epMapEntries, fit), PSMF_EP_MAP_MAX_ENTRIES);
        out->epMap.reserve(count);
        for (size_t i = 0; i < count; i++) {
            const size_t e = ep_off + i * PSMF_EP_ENTRY_SIZE;
            PsmfEpEntry ep;
            ep.index = r.u8(e);
            ep.picOffset = r.u8(e + 1);
            ep.pts = r.be32(e + 2);
            ep.offset = r.be32(e + 6);
            out->epMap.push_back(ep);
        }
    }
    return PsmfStatus::Ok;
}
