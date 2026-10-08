#pragma once
// PSMF (PlayStation Media Framework) header parser.
//
// Pure data-in/data-out: no guest memory, no FFmpeg. The sceMpeg/scePsmf HLE
// (psp_hle_mpeg.cpp) copies the header out of guest RAM and hands it here;
// the media engine reads the presentation timestamps from the result.
//
// Ported from PPSSPP (GPL-2.0-or-later): Core/HLE/scePsmf.cpp (Psmf,
// PsmfStream), Core/HLE/sceMpeg.cpp (AnalyzeMpeg, getMpegVersion) and
// Core/HW/MediaEngine.h (getMpegTimeStamp).

#include <cstddef>
#include <cstdint>
#include <vector>

constexpr uint32_t PSMF_MAGIC = 0x464D5350U;  // "PSMF" read as a LE u32
constexpr size_t PSMF_STREAM_VERSION_OFFSET = 0x4;
constexpr size_t PSMF_STREAM_OFFSET_OFFSET = 0x8;
constexpr size_t PSMF_STREAM_SIZE_OFFSET = 0xC;
constexpr size_t PSMF_FIRST_TIMESTAMP_OFFSET = 0x54;
constexpr size_t PSMF_LAST_TIMESTAMP_OFFSET = 0x5A;
constexpr size_t PSMF_STREAM_COUNT_OFFSET = 0x80;
constexpr size_t PSMF_STREAM_TABLE_OFFSET = 0x82;
constexpr size_t PSMF_STREAM_ENTRY_SIZE = 16;
constexpr size_t PSMF_EP_ENTRY_SIZE = 10;
/// Upper bound on EP-map entries kept per header (the table is only used for
/// seeking, which no movie path needs; the bound keeps hostile headers cheap).
constexpr size_t PSMF_EP_MAP_MAX_ENTRIES = 0x10000;

constexpr int PSMF_VIDEO_STREAM_ID = 0xE0;
constexpr int PSMF_AUDIO_STREAM_ID = 0xBD;

// Stream types as exposed by scePsmf / sceMpeg.
constexpr int PSMF_AVC_STREAM = 0;
constexpr int PSMF_ATRAC_STREAM = 1;
constexpr int PSMF_PCM_STREAM = 2;
constexpr int PSMF_DATA_STREAM = 3;
constexpr int PSMF_AUDIO_STREAM = 15;  // query-only: matches ATRAC and PCM

// Raw version words ("0012".."0015" as a LE u32).
constexpr uint32_t PSMF_VERSION_0012 = 0x32313030U;
constexpr uint32_t PSMF_VERSION_0013 = 0x33313030U;
constexpr uint32_t PSMF_VERSION_0014 = 0x34313030U;
constexpr uint32_t PSMF_VERSION_0015 = 0x35313030U;

enum class PsmfStatus {
    Ok = 0,
    TooShort,         // not even the fixed header fields are present
    BadMagic,         // does not start with "PSMF"
    BadVersion,       // version word is zero
    BadStreamOffset,  // stream offset is zero
};

struct PsmfStreamInfo {
    int type = PSMF_AVC_STREAM;
    int channel = 0;      // ES channel: id & 0xF (video) / private id & 0xF
    int streamId = 0;     // PES stream id from the table (0xE0.. / 0xBD)
    int privateId = 0;    // second byte of the table entry
    int videoWidth = -1;  // -1 when this is not a video stream
    int videoHeight = -1;
    int audioChannels = -1;   // -1 when this is not an audio stream
    int audioFrequency = -1;

    /// scePsmfGetNumberOfSpecificStreams semantics: PSMF_AUDIO_STREAM matches
    /// both ATRAC and PCM streams, every other type matches exactly.
    bool matchesType(int ty) const {
        if (ty == PSMF_AUDIO_STREAM) {
            return type == PSMF_ATRAC_STREAM || type == PSMF_PCM_STREAM;
        }
        return type == ty;
    }
};

struct PsmfEpEntry {
    int index = 0;
    int picOffset = 0;
    uint32_t pts = 0;
    uint32_t offset = 0;
};

struct PsmfHeader {
    uint32_t magic = 0;
    uint32_t version = 0;       // raw LE word, e.g. PSMF_VERSION_0015
    uint32_t streamOffset = 0;  // bytes from the header start to the MPEG-PS
    uint32_t streamSize = 0;    // MPEG-PS length in bytes
    uint32_t streamDataTotalSize = 0;
    int64_t firstTimestamp = 0;  // 90 kHz
    int64_t lastTimestamp = 0;
    uint32_t streamDataNextBlockSize = 0;
    uint32_t streamDataNextInnerBlockSize = 0;
    uint32_t numStreams = 0;  // stream count as stored (may exceed streams.size())
    uint32_t epMapOffset = 0;
    uint32_t epMapEntries = 0;
    int videoWidth = 0;  // first video stream, 0 when there is none
    int videoHeight = 0;
    std::vector<PsmfStreamInfo> streams;  // audio/video streams, table order
    std::vector<PsmfEpEntry> epMap;

    int countStreams(int type) const {
        int n = 0;
        for (const PsmfStreamInfo& s : streams) {
            if (s.matchesType(type)) {
                n++;
            }
        }
        return n;
    }
};

/// Parses the PSMF header in `data[0..size)`. Never reads outside the buffer:
/// fields beyond `size` read as zero, the stream table and EP map are clipped
/// to what is present. `*out` is always reset first; on a non-Ok status it
/// holds whatever fixed fields could be read. Reads from a 2048-byte header
/// block are sufficient for every real movie.
PsmfStatus psmf_parse(const uint8_t* data, size_t size, PsmfHeader* out);

/// 40-bit big-endian MPEG timestamp (the 6-byte fields at 0x54 / 0x5A).
int64_t psmf_read_timestamp(const uint8_t* p6);

/// sceMpeg version index: 0..3 for "0012".."0015", -1 for anything else.
int psmf_mpeg_version_index(uint32_t rawVersion);
