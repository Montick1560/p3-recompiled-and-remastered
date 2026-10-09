#include "hle/psp_hle_mpeg.h"

#include "hle/psp_hle.h"
#include "hle/psp_media_engine.h"
#include "hle/psp_mpeg_demux.h"
#include "hle/psp_psmf.h"
#include "psp_memory.h"
#include "psp_scheduler.h"
#include "recomp.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

// ================================================================
// sceMpeg HLE: movie playback through the game's ringbuffer. Ported from
// PPSSPP (GPL-2.0-or-later), Core/HLE/sceMpeg.cpp: struct layouts, argument
// order, output-pointer writes and return codes follow it; hleDelayResult
// timing is not reproduced (calls complete immediately).
//
// Data path: sceMpegRingbufferPut calls the game's read callback (a guest
// function) to fill ring packets, then copies them into a PspMpegDemux. The
// decoders (psp_media_engine.h) pull H.264 access units and ATRAC3plus frames
// from it. The ring's packetsAvail tracks the bytes the video path has not
// consumed yet, so the game's feeder refills as frames are decoded.
//
// Arguments: a0..a3 in ctx->r[4..7], 5th/6th in t0/t1 (ctx->r[8], r[9]).
// ================================================================

namespace {

constexpr int32_t ERR_NO_DATA = (int32_t)0x80618001U;
constexpr int32_t ERR_BAD_VERSION = (int32_t)0x80610002U;
constexpr int32_t ERR_NO_MEMORY = (int32_t)0x80610022U;
constexpr int32_t ERR_INVALID_VALUE = (int32_t)0x806101FEU;
constexpr int32_t ERR_NOT_YET_INIT = (int32_t)0x80618009U;
constexpr int32_t ERR_AVC_DECODE_FATAL = (int32_t)0x80628002U;
constexpr int32_t ERR_KERNEL_ILLEGAL_ADDRESS = (int32_t)0x800200D3U;

constexpr int MPEG_AVC_STREAM = 0;
constexpr int MPEG_ATRAC_STREAM = 1;
constexpr int MPEG_AUDIO_STREAM = 15;

constexpr uint32_t MPEG_AVC_ES_SIZE = 2048;
constexpr uint32_t MPEG_ATRAC_ES_SIZE = 2112;
constexpr uint32_t MPEG_ATRAC_ES_OUTPUT_SIZE = 8192;
constexpr uint32_t MPEG_MEMSIZE = 0x10000;  // libmpeg 0105
constexpr int MPEG_DATA_ES_BUFFERS = 2;
constexpr int PACKET_SIZE = 2048;
constexpr int64_t VIDEO_TS_STEP = 3003;
constexpr int64_t AUDIO_TS_STEP = 4180;
constexpr uint32_t PSMF_MAGIC_LE = 0x464D5350U;  // "PSMF"

constexpr uint32_t kAddrMask = 0x07FFFFFFU;
constexpr uint32_t kNullPage = 0x00010000U;

// ---- guest memory ----

uint8_t* gptr(uint8_t* rdram, uint32_t addr, uint32_t len) {
    if (addr < kNullPage) {
        return nullptr;
    }
    const uint64_t a = addr & kAddrMask;
    if (a + len > PSP_MEM_SIZE) {
        return nullptr;
    }
    return rdram + a;
}

bool rd32(uint8_t* rdram, uint32_t addr, uint32_t* v) {
    uint8_t* p = gptr(rdram, addr, 4);
    if (!p) {
        return false;
    }
    std::memcpy(v, p, 4);
    return true;
}

void wr32(uint8_t* rdram, uint32_t addr, uint32_t v) {
    if (uint8_t* p = gptr(rdram, addr, 4)) {
        std::memcpy(p, &v, 4);
    }
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// SceMpegRingBuffer as laid out in guest memory.
struct Ring {
    int32_t packets;
    int32_t packetsRead;
    int32_t packetsWritePos;
    int32_t packetsAvail;
    int32_t packetSize;
    uint32_t data;
    uint32_t callbackAddr;
    int32_t callbackArgs;
    int32_t dataUpperBound;
    int32_t semaID;
    uint32_t mpeg;
    uint32_t gp;
};
static_assert(sizeof(Ring) == 48, "SceMpegRingBuffer is 48 bytes");

bool read_ring(uint8_t* rdram, uint32_t addr, Ring* r) {
    uint8_t* p = gptr(rdram, addr, sizeof(Ring));
    if (!p) {
        return false;
    }
    std::memcpy(r, p, sizeof(Ring));
    return true;
}

void write_ring(uint8_t* rdram, uint32_t addr, const Ring& r) {
    if (uint8_t* p = gptr(rdram, addr, sizeof(Ring))) {
        std::memcpy(p, &r, sizeof(Ring));
    }
}

// SceMpegAu: pts/dts are stored high word first.
struct Au {
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t esBuffer = 0;
    uint32_t esSize = 0;
};

int64_t swap_halves(uint64_t v) { return (int64_t)((v & 0xFFFFFFFFULL) << 32 | v >> 32); }

Au read_au(uint8_t* rdram, uint32_t addr) {
    Au au;
    uint8_t* p = gptr(rdram, addr, 24);
    if (p) {
        uint64_t pts, dts;
        std::memcpy(&pts, p, 8);
        std::memcpy(&dts, p + 8, 8);
        au.pts = swap_halves(pts);
        au.dts = swap_halves(dts);
        std::memcpy(&au.esBuffer, p + 16, 4);
        std::memcpy(&au.esSize, p + 20, 4);
    }
    return au;
}

void write_au(uint8_t* rdram, uint32_t addr, const Au& au) {
    uint8_t* p = gptr(rdram, addr, 24);
    if (p) {
        uint64_t pts = (uint64_t)swap_halves((uint64_t)au.pts);
        uint64_t dts = (uint64_t)swap_halves((uint64_t)au.dts);
        std::memcpy(p, &pts, 8);
        std::memcpy(p + 8, &dts, 8);
        std::memcpy(p + 16, &au.esBuffer, 4);
        std::memcpy(p + 20, &au.esSize, 4);
    }
}

// ---- contexts ----

struct StreamInfo {
    int type = -1;
    int num = 0;
    bool needsReset = true;
};

struct MpegCtx {
    uint32_t ringAddr = 0;
    int defaultFrameWidth = 0;
    int videoPixelMode = PSP_VIDEO_ABGR8888;
    bool avcRegistered = false;
    bool atracRegistered = false;
    bool esBuffers[MPEG_DATA_ES_BUFFERS] = {false, false};
    std::map<int, StreamInfo> streams;

    // From the PSMF header (sceMpegQueryStreamOffset).
    bool analyzed = false;
    uint32_t mpegOffset = 0;
    uint32_t streamSize = 0;
    int64_t firstTimestamp = 90000;
    int frameWidth = 0;   // avcDetailFrameWidth
    int frameHeight = 0;

    // Media engine state.
    std::unique_ptr<PspMpegDemux> demux;
    PspVideoDecoder video;
    PspMpegAudioDecoder audio;
    uint64_t fedBytes = 0;
    bool inputEnded = false;
    bool videoEnd = false;
    bool audioEnded = false;  // input ended and the audio ES ran dry
    int starvedCalls = 0;  // decodes in a row with no AU and no new data
    int zeroAsks = 0;      // RingbufferPut calls in a row asking for 0 packets
    int64_t videoPts = 0;  // relative to firstTimestamp, after the last picture
    int64_t audioPts = 0;  // relative, after the last audio frame
    int videoFrameCount = 0;
    int audioFrameCount = 0;
    int avcFrameStatus = 0;
    int videoStreamNum = 0;
    int audioStreamNum = 0;
};

std::mutex g_mtx;
std::map<uint32_t, std::unique_ptr<MpegCtx>> g_ctxs;  // keyed by handle
int g_streamIdGen = 1;

// Overridable for tests (psp_hle_mpeg_set_test_hooks).
PspMpegGuestCall g_guest_call = nullptr;

MpegCtx* get_ctx(uint8_t* rdram, uint32_t mpegAddr) {
    uint32_t handle = 0;
    if (!rd32(rdram, mpegAddr, &handle)) {
        return nullptr;
    }
    auto it = g_ctxs.find(handle);
    return it == g_ctxs.end() ? nullptr : it->second.get();
}

void reset_engine(MpegCtx* c, int ringPackets) {
    size_t cap = (size_t)std::max(ringPackets, 1) * PACKET_SIZE + PACKET_SIZE;
    c->demux = std::make_unique<PspMpegDemux>(cap);
    c->demux->setVideoStreamId(0xE0 | (c->videoStreamNum & 0xF));
    c->demux->setAudioChannel(c->audioStreamNum);
    c->video.reset();
    c->audio.reset();
    c->fedBytes = 0;
    c->inputEnded = false;
    c->videoEnd = false;
    c->audioEnded = false;
    c->starvedCalls = 0;
    c->zeroAsks = 0;
    c->videoPts = 0;
    c->audioPts = 0;
    c->videoFrameCount = 0;
    c->audioFrameCount = 0;
    c->avcFrameStatus = 0;
}

// Packets the video path still holds (PPSSPP: packets - getRemainSize()/2048).
// While the input has ended but pictures may still come out, at least one
// packet is reported so the game keeps asking for AUs until the true end --
// but only while the audio still runs: Patapon 3's player paces pictures on
// the audio clock and ends a movie once sceMpegRingbufferAvailableSize
// reports the whole ring free, so with the audio dry and every byte consumed
// the forced packet deadlocked the story movie (PPSSPP has no forced packet).
int occupied_packets(const MpegCtx* c, int ringPackets) {
    if (!c->demux) {
        return 0;
    }
    size_t pending = c->demux->pendingVideoBytes();
    int n = (int)((pending + PACKET_SIZE - 1) / PACKET_SIZE);
    if (c->inputEnded && !c->videoEnd && !c->audioEnded && n == 0) {
        n = 1;
    }
    return std::min(n, ringPackets);
}

void sync_avail(uint8_t* rdram, MpegCtx* c) {
    Ring r;
    if (c->analyzed && read_ring(rdram, c->ringAddr, &r)) {
        r.packetsAvail = occupied_packets(c, r.packets);
        write_ring(rdram, c->ringAddr, r);
    }
}

// Reads the PSMF header fields sceMpeg cares about. Returns 0 or the error
// sceMpegQueryStreamOffset reports.
struct Header {
    uint32_t magic = 0;
    int version = -1;
    uint32_t offset = 0;
    uint32_t size = 0;
    int64_t firstTs = 0;
    int width = 0;
    int height = 0;
};

Header parse_header(uint8_t* rdram, uint32_t addr) {
    Header h;
    uint8_t* p = gptr(rdram, addr, 0x90);
    if (!p) {
        return h;
    }
    std::memcpy(&h.magic, p, 4);
    uint32_t rawVersion;
    std::memcpy(&rawVersion, p + 4, 4);
    h.version = psmf_mpeg_version_index(rawVersion);
    h.offset = be32(p + 8);
    h.size = be32(p + 12);
    h.firstTs = psmf_read_timestamp(p + 0x54);
    h.width = p[142] * 0x10;
    h.height = p[143] * 0x10;
    return h;
}

// Feeds ring packets into the demuxer, skipping a PSMF header if the game
// streams the whole file rather than just the MPEG-PS part.
void feed(MpegCtx* c, const uint8_t* data, size_t n) {
    if (!c->demux) {
        return;
    }
    if (c->fedBytes == 0 && n >= 4 && std::memcmp(data, "PSMF", 4) == 0 && c->mpegOffset > 0 &&
        c->mpegOffset <= n) {
        data += c->mpegOffset;
        n -= c->mpegOffset;
    }
    if (!c->demux->addStreamData(data, n)) {
        std::fprintf(stderr, "[HLE] sceMpegRingbufferPut: demux full, %zu bytes dropped\n", n);
    }
    c->fedBytes += n;
    c->starvedCalls = 0;
    c->zeroAsks = 0;
    if (c->streamSize > 0 && c->fedBytes >= c->streamSize) {
        c->inputEnded = true;
    }
    c->demux->demux();
}

// One decoded picture (PPSSPP MediaEngine::stepVideo). Without FFmpeg each AU
// counts as a frame so playback timing still advances to the end.
bool step_video(MpegCtx* c) {
    if (!c->demux) {
        return false;
    }
    for (;;) {
        c->demux->demux();
        PspAvcAccessUnit au;
        if (c->demux->nextVideoAu(&au, c->inputEnded)) {
            if (!PspVideoDecoder::available() || c->video.decode(au.data.data(), au.data.size())) {
                c->videoPts += VIDEO_TS_STEP;
                return true;
            }
            continue;  // decoder delay: feed the next AU
        }
        // No complete AU. If the game has stopped feeding (several decodes
        // in a row without new packets after the stream started), the file
        // is exhausted: flush the trailing AU and drain the decoder
        // (PPSSPP ends the video when the demuxer runs dry).
        if (!c->inputEnded && c->fedBytes > 0 && ++c->starvedCalls >= 8) {
            c->inputEnded = true;
            continue;
        }
        if (c->inputEnded) {
            if (PspVideoDecoder::available() && c->video.drain()) {
                c->videoPts += VIDEO_TS_STEP;
                return true;
            }
            c->videoEnd = true;
            std::fprintf(stderr, "[HLE] sceMpeg: video end after %d pictures\n",
                         c->videoFrameCount);
        }
        return false;
    }
}

int32_t call_guest(uint8_t* rdram, recomp_context* ctx, uint32_t fn, uint32_t a0, uint32_t a1,
                   uint32_t a2) {
    if (g_guest_call) {
        return g_guest_call(rdram, ctx, fn, a0, a1, a2);
    }
    FuncPtr f = RECOMP_LOOKUP(fn);
    if (!f) {
        std::fprintf(stderr, "[HLE] sceMpegRingbufferPut: LOOKUP_MISS callback=0x%08X\n", fn);
        return -1;
    }
    // Same discipline as psp_intr_dispatch_vblank: run on this thread's
    // stack below the caller's frame, restore the whole register file.
    const recomp_context saved = *ctx;
    ctx->r[4] = (int32_t)a0;
    ctx->r[5] = (int32_t)a1;
    ctx->r[6] = (int32_t)a2;
    ctx->r[29] = (int32_t)(((uint32_t)saved.r[29] - 0x40u) & ~0xFu);
    ctx->r[31] = 0;
    f(rdram, ctx);
    int32_t v0 = ctx->r[2];
    *ctx = saved;
    return v0;
}

void ret(recomp_context* ctx, int32_t v) { ctx->r[2] = v; }
uint32_t arg(recomp_context* ctx, int n) { return (uint32_t)ctx->r[4 + n]; }

int log_budget = 64;
void logf_once(const char* fmt, uint32_t a, uint32_t b, int32_t r) {
    if (log_budget > 0) {
        log_budget--;
        std::fprintf(stderr, fmt, a, b, r);
    }
}

// ---- handlers ----

void hle_sceMpegInit(uint8_t*, recomp_context* ctx) { ret(ctx, 0); }

void hle_sceMpegFinish(uint8_t*, recomp_context* ctx) { ret(ctx, 0); }

void hle_sceMpegQueryMemSize(uint8_t*, recomp_context* ctx) { ret(ctx, (int32_t)MPEG_MEMSIZE); }

void hle_sceMpegRingbufferQueryMemSize(uint8_t*, recomp_context* ctx) {
    ret(ctx, (int32_t)((int32_t)arg(ctx, 0) * (104 + PACKET_SIZE)));
}

void hle_sceMpegRingbufferConstruct(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t ringAddr = arg(ctx, 0);
    const int32_t numPackets = (int32_t)arg(ctx, 1);
    const uint32_t data = arg(ctx, 2);
    const int32_t size = (int32_t)arg(ctx, 3);
    if (!gptr(rdram, ringAddr, sizeof(Ring))) {
        ret(ctx, ERR_KERNEL_ILLEGAL_ADDRESS);
        return;
    }
    if (size < 0) {
        ret(ctx, ERR_NO_MEMORY);
        return;
    }
    if ((int64_t)numPackets * (104 + PACKET_SIZE) > size && numPackets < 0x00100000) {
        ret(ctx, ERR_NO_MEMORY);
        return;
    }
    Ring r;
    read_ring(rdram, ringAddr, &r);
    r.packets = numPackets;
    r.packetsRead = 0;
    r.packetsWritePos = 0;
    r.packetsAvail = 0;
    r.packetSize = PACKET_SIZE;
    r.data = data;
    r.callbackAddr = (uint32_t)ctx->r[8];
    r.callbackArgs = ctx->r[9];
    r.dataUpperBound = (int32_t)(data + (uint32_t)numPackets * PACKET_SIZE);
    r.mpeg = 0;
    r.gp = (uint32_t)ctx->r[28];
    write_ring(rdram, ringAddr, r);
    ret(ctx, 0);
}

void hle_sceMpegRingbufferDestruct(uint8_t*, recomp_context* ctx) { ret(ctx, 0); }

void hle_sceMpegCreate(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mpegAddr = arg(ctx, 0);
    const uint32_t dataPtr = arg(ctx, 1);
    const uint32_t size = arg(ctx, 2);
    const uint32_t ringAddr = arg(ctx, 3);
    const uint32_t frameWidth = (uint32_t)ctx->r[8];
    if (!gptr(rdram, mpegAddr, 4)) {
        ret(ctx, -1);
        return;
    }
    if (size < MPEG_MEMSIZE) {
        ret(ctx, ERR_NO_MEMORY);
        return;
    }
    Ring r;
    const bool ringOk = read_ring(rdram, ringAddr, &r);
    if (ringOk) {
        r.packetsAvail = r.packetSize == 0
                             ? 0
                             : r.packets - (r.dataUpperBound - (int32_t)r.data) / r.packetSize;
        r.mpeg = mpegAddr;
        write_ring(rdram, ringAddr, r);
    }
    const uint32_t handle = dataPtr + 0x30;
    wr32(rdram, mpegAddr, handle);
    if (uint8_t* h = gptr(rdram, handle, 24)) {
        std::memcpy(h, "LIBMPEG\0", 8);
        std::memcpy(h + 8, "001\0", 4);
        wr32(rdram, handle + 12, 0xFFFFFFFFU);
        if (ringOk) {
            wr32(rdram, handle + 16, ringAddr);
            wr32(rdram, handle + 20, (uint32_t)r.dataUpperBound);
        }
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    auto c = std::make_unique<MpegCtx>();
    c->ringAddr = ringAddr;
    c->defaultFrameWidth = (int)frameWidth;
    g_ctxs[handle] = std::move(c);
    std::fprintf(stderr, "[HLE] sceMpegCreate mpeg=0x%08X handle=0x%08X ring=0x%08X fw=%u\n",
                 mpegAddr, handle, ringAddr, frameWidth);
    ret(ctx, 0);
}

void hle_sceMpegDelete(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t handle = 0;
    if (!get_ctx(rdram, arg(ctx, 0)) || !rd32(rdram, arg(ctx, 0), &handle)) {
        ret(ctx, -1);
        return;
    }
    g_ctxs.erase(handle);
    ret(ctx, 0);
}

void hle_sceMpegQueryStreamOffset(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t bufferAddr = arg(ctx, 1);
    const uint32_t offsetAddr = arg(ctx, 2);
    if (!gptr(rdram, bufferAddr, 1) || !gptr(rdram, offsetAddr, 4)) {
        ret(ctx, -1);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    Header h = parse_header(rdram, bufferAddr);
    if (h.magic != PSMF_MAGIC_LE) {
        wr32(rdram, offsetAddr, 0);
        ret(ctx, ERR_INVALID_VALUE);
        return;
    }
    if (h.version < 0) {
        wr32(rdram, offsetAddr, 0);
        ret(ctx, ERR_BAD_VERSION);
        return;
    }
    if ((h.offset & 2047) != 0 || h.offset == 0) {
        wr32(rdram, offsetAddr, 0);
        ret(ctx, ERR_INVALID_VALUE);
        return;
    }
    c->mpegOffset = h.offset;
    c->streamSize = h.size;
    c->firstTimestamp = h.firstTs;
    c->frameWidth = h.width;
    c->frameHeight = h.height;
    if (!c->analyzed) {
        Ring r{};
        read_ring(rdram, c->ringAddr, &r);
        reset_engine(c, r.packets);
        c->analyzed = true;
    }
    std::fprintf(stderr,
                 "[HLE] sceMpegQueryStreamOffset offset=0x%X size=0x%X firstTs=%lld %dx%d\n",
                 h.offset, h.size, (long long)h.firstTs, h.width, h.height);
    wr32(rdram, offsetAddr, h.offset);
    ret(ctx, 0);
}

void hle_sceMpegQueryStreamSize(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t bufferAddr = arg(ctx, 0);
    const uint32_t sizeAddr = arg(ctx, 1);
    if (!gptr(rdram, bufferAddr, 1) || !gptr(rdram, sizeAddr, 4)) {
        ret(ctx, -1);
        return;
    }
    Header h = parse_header(rdram, bufferAddr);
    if (h.magic != PSMF_MAGIC_LE || (h.offset & 2047) != 0) {
        wr32(rdram, sizeAddr, 0);
        ret(ctx, ERR_INVALID_VALUE);
        return;
    }
    wr32(rdram, sizeAddr, h.size);
    ret(ctx, 0);
}

void hle_sceMpegRegistStream(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    const int type = (int)arg(ctx, 1);
    const int num = (int)arg(ctx, 2);
    switch (type) {
        case MPEG_AVC_STREAM:
            c->avcRegistered = true;
            c->videoStreamNum = num;
            if (c->demux) {
                c->demux->setVideoStreamId(0xE0 | (num & 0xF));
            }
            break;
        case MPEG_AUDIO_STREAM:
        case MPEG_ATRAC_STREAM:
            c->atracRegistered = true;
            c->audioStreamNum = num;
            if (c->demux) {
                c->demux->setAudioChannel(num);
            }
            break;
        default:
            break;
    }
    const int sid = g_streamIdGen++;
    StreamInfo info;
    info.type = type;
    info.num = num;
    c->streams[sid] = info;
    std::fprintf(stderr, "[HLE] sceMpegRegistStream type=%d num=%d -> %d\n", type, num, sid);
    ret(ctx, sid);
}

void hle_sceMpegUnRegistStream(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    auto it = c->streams.find((int)arg(ctx, 1));
    if (it != c->streams.end()) {
        if (it->second.type == MPEG_AVC_STREAM) {
            c->avcRegistered = false;
        } else if (it->second.type == MPEG_ATRAC_STREAM || it->second.type == MPEG_AUDIO_STREAM) {
            c->atracRegistered = false;
        }
        c->streams.erase(it);
    }
    c->analyzed = false;
    ret(ctx, 0);
}

void hle_sceMpegMallocAvcEsBuf(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    for (int i = 0; i < MPEG_DATA_ES_BUFFERS; i++) {
        if (!c->esBuffers[i]) {
            c->esBuffers[i] = true;
            ret(ctx, i + 1);
            return;
        }
    }
    ret(ctx, 0);
}

void hle_sceMpegFreeAvcEsBuf(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    const int esBuf = (int)arg(ctx, 1);
    if (esBuf == 0) {
        ret(ctx, ERR_INVALID_VALUE);
        return;
    }
    if (esBuf >= 1 && esBuf <= MPEG_DATA_ES_BUFFERS) {
        c->esBuffers[esBuf - 1] = false;
    }
    ret(ctx, 0);
}

void hle_sceMpegInitAu(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    const uint32_t buf = arg(ctx, 1);
    const uint32_t auAddr = arg(ctx, 2);
    Au au = read_au(rdram, auAddr);
    au.esBuffer = 0;
    if (buf >= 1 && buf <= (uint32_t)MPEG_DATA_ES_BUFFERS && c->esBuffers[buf - 1]) {
        au.esSize = MPEG_AVC_ES_SIZE;
        au.dts = 0;
        au.pts = 0;
    } else {
        au.esSize = MPEG_ATRAC_ES_SIZE;
        au.pts = 0;
        au.dts = -1;
    }
    write_au(rdram, auAddr, au);
    ret(ctx, 0);
}

void hle_sceMpegQueryAtracEsSize(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t esSizeAddr = arg(ctx, 1);
    const uint32_t outSizeAddr = arg(ctx, 2);
    if (!gptr(rdram, esSizeAddr, 4) || !gptr(rdram, outSizeAddr, 4)) {
        ret(ctx, -1);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!get_ctx(rdram, arg(ctx, 0))) {
        ret(ctx, -1);
        return;
    }
    wr32(rdram, esSizeAddr, MPEG_ATRAC_ES_SIZE);
    wr32(rdram, outSizeAddr, MPEG_ATRAC_ES_OUTPUT_SIZE);
    ret(ctx, 0);
}

void hle_sceMpegRingbufferAvailableSize(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t ringAddr = arg(ctx, 0);
    int32_t result;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        Ring r;
        if (!read_ring(rdram, ringAddr, &r)) {
            ret(ctx, ERR_KERNEL_ILLEGAL_ADDRESS);
            return;
        }
        MpegCtx* c = get_ctx(rdram, r.mpeg);
        if (!c) {
            ret(ctx, ERR_NOT_YET_INIT);
            return;
        }
        c->ringAddr = ringAddr;
        if (c->analyzed && c->demux) {
            r.packetsAvail = occupied_packets(c, r.packets);
            write_ring(rdram, ringAddr, r);
        }
        result = r.packets - r.packetsAvail;
    }
    // Feeder loops poll this; let other threads (decoder, audio) run.
    sched_yield_point();
    ret(ctx, result);
}

void hle_sceMpegRingbufferPut(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t ringAddr = arg(ctx, 0);
    int numPackets = (int)arg(ctx, 1);
    const int available = (int)arg(ctx, 2);
    Ring r;
    if (!read_ring(rdram, ringAddr, &r) || r.packets <= 0) {
        ret(ctx, -1);
        return;
    }
    const int asked = numPackets;
    numPackets = std::min(numPackets, available);
    numPackets = std::min(numPackets, r.packets - r.packetsAvail);
    if (numPackets <= 0) {
        // A feeder that keeps asking for 0 packets while the ring has room has
        // no file data left (Patapon 3's movie reader passes min(free, bytes
        // left) and the file ends short of the PSMF header's stream size), so
        // the read callback never runs to report EOF. A short run of 0-asks
        // is only a reader waiting for its next async chunk (the opening
        // movie), so the end needs ~2 s of them in a row. Then the demuxer
        // releases its trailing AUs and the ring drains (the game ends a
        // movie only once sceMpegRingbufferAvailableSize reports it all free).
        if (asked == 0 && r.packets - r.packetsAvail > 0) {
            std::lock_guard<std::mutex> lk(g_mtx);
            MpegCtx* c = get_ctx(rdram, r.mpeg);
            if (c && c->fedBytes > 0 && !c->inputEnded && ++c->zeroAsks >= 60) {
                c->inputEnded = true;
                std::fprintf(stderr,
                             "[HLE] sceMpegRingbufferPut: feeder asked 0 packets %d times after "
                             "%zu bytes -> end of input\n",
                             c->zeroAsks, (size_t)c->fedBytes);
            }
        }
        ret(ctx, 0);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        MpegCtx* c = get_ctx(rdram, r.mpeg);
        if (!c) {
            ret(ctx, -1);
            return;
        }
        if (r.packetsRead == 0) {
            // Start of a stream (fresh or after FlushAllStream).
            reset_engine(c, r.packets);
            c->analyzed = true;
        }
    }
    if (r.callbackAddr == 0) {
        std::fprintf(stderr, "[HLE] sceMpegRingbufferPut: no callback\n");
        ret(ctx, 0);
        return;
    }

    int total = 0;
    int remaining = numPackets;
    while (remaining > 0) {
        read_ring(rdram, ringAddr, &r);
        const int writeOffset = r.packetsWritePos % r.packets;
        const int desired = std::min(remaining, r.packets - writeOffset);
        const uint32_t dst = r.data + (uint32_t)writeOffset * PACKET_SIZE;
        // The callback reads the movie file; run it without the lock.
        int added = call_guest(rdram, ctx, r.callbackAddr, dst, (uint32_t)desired,
                               (uint32_t)r.callbackArgs);
        std::lock_guard<std::mutex> lk(g_mtx);
        read_ring(rdram, ringAddr, &r);
        MpegCtx* c = get_ctx(rdram, r.mpeg);
        if (!c) {
            break;
        }
        if (added <= 0) {
            // The reader ran dry after the stream started: the file is
            // exhausted (it may end short of the PSMF header's stream
            // size). A 0 before any data is just "not ready yet".
            if (added == 0 && c->fedBytes > 0) {
                c->inputEnded = true;
            }
            if (added < 0 && total == 0) {
                ret(ctx, added);
                return;
            }
            break;
        }
        added = std::min(added, r.packets - r.packetsAvail);
        if (const uint8_t* src = gptr(rdram, dst, (uint32_t)added * PACKET_SIZE)) {
            feed(c, src, (size_t)added * PACKET_SIZE);
        }
        r.packetsRead += added;
        r.packetsWritePos += added;
        r.packetsAvail = std::max(r.packetsAvail + added, 0);
        r.packetsAvail = std::min(r.packetsAvail, r.packets);
        write_ring(rdram, ringAddr, r);
        total += added;
        remaining -= added;
        if (added < desired) {
            break;
        }
    }
    logf_once("[HLE] sceMpegRingbufferPut ring=0x%08X asked=%u -> %d\n", ringAddr,
              (uint32_t)numPackets, total);
    ret(ctx, total);
}

void hle_sceMpegGetAvcAu(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    const int streamId = (int)arg(ctx, 1);
    const uint32_t auAddr = arg(ctx, 2);
    const uint32_t attrAddr = arg(ctx, 3);
    Ring r;
    if (!read_ring(rdram, c->ringAddr, &r)) {
        ret(ctx, -1);
        return;
    }
    Au au = read_au(rdram, auAddr);
    if (r.packetsRead == 0 || r.packetsAvail == 0) {
        au.pts = 0;
        au.dts = 0;
        write_au(rdram, auAddr, au);
        ret(ctx, ERR_NO_DATA);
        return;
    }
    auto it = c->streams.find(streamId);
    if (it == c->streams.end()) {
        ret(ctx, -1);
        return;
    }
    if (it->second.needsReset) {
        au.pts = 0;
        it->second.needsReset = false;
    }
    au.esBuffer = (uint32_t)it->second.num;
    au.pts = c->videoPts + c->firstTimestamp;
    au.dts = au.pts - VIDEO_TS_STEP;
    int32_t result = 0;
    if (c->videoEnd) {
        au.dts = -1;
        result = ERR_NO_DATA;
        logf_once("[HLE] sceMpegGetAvcAu: video end (frames=%u pts=%u) -> 0x%08X\n",
                  (uint32_t)c->videoFrameCount, (uint32_t)au.pts, result);
    }
    write_au(rdram, auAddr, au);
    if (result == 0 && gptr(rdram, attrAddr, 4)) {
        wr32(rdram, attrAddr, 1);
    }
    ret(ctx, result);
}

void hle_sceMpegGetAtracAu(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    const int streamId = (int)arg(ctx, 1);
    const uint32_t auAddr = arg(ctx, 2);
    const uint32_t attrAddr = arg(ctx, 3);
    Ring r;
    if (!read_ring(rdram, c->ringAddr, &r)) {
        ret(ctx, -1);
        return;
    }
    Au au = read_au(rdram, auAddr);
    auto it = c->streams.find(streamId);
    if (it != c->streams.end() && it->second.needsReset) {
        au.pts = 0;
        it->second.needsReset = false;
    }
    if (r.packetsAvail == 0) {
        au.pts = 0;
        au.dts = 0;
        write_au(rdram, auAddr, au);
        ret(ctx, ERR_NO_DATA);
        return;
    }
    if (it != c->streams.end()) {
        au.esBuffer = (uint32_t)it->second.num;
    }
    au.pts = c->audioPts - AUDIO_TS_STEP + c->firstTimestamp;
    au.dts = au.pts;
    int32_t result = 0;
    bool noAudio = true;
    if (c->demux) {
        c->demux->demux();
        noAudio = !c->demux->hasNextAudioFrame(nullptr, nullptr, nullptr, nullptr);
    }
    if (noAudio) {
        au.dts = -1;
        result = ERR_NO_DATA;
        if (c->inputEnded) {
            c->audioEnded = true;
            sync_avail(rdram, c);
        }
        static int noAudioLogs = 0;
        if (++noAudioLogs <= 20 || noAudioLogs % 300 == 0) {
            std::fprintf(stderr,
                         "[HLE] sceMpegGetAtracAu: no audio frame #%d (audioFrames=%u videoFrames=%u "
                         "apts=%lld vpts=%lld pendingVideo=%zu inputEnded=%d)\n",
                         noAudioLogs, (uint32_t)c->audioFrameCount,
                         (uint32_t)c->videoFrameCount, (long long)c->audioPts,
                         (long long)c->videoPts,
                         c->demux ? c->demux->pendingVideoBytes() : 0,
                         c->inputEnded ? 1 : 0);
        }
    }
    write_au(rdram, auAddr, au);
    if (result == 0 && gptr(rdram, attrAddr, 4) && (attrAddr & 3) == 0) {
        wr32(rdram, attrAddr, 0);
    }
    ret(ctx, result);
}

void hle_sceMpegAtracDecode(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    const uint32_t auAddr = arg(ctx, 1);
    uint8_t* out = gptr(rdram, arg(ctx, 2), MPEG_ATRAC_ES_OUTPUT_SIZE);
    if (!out) {
        ret(ctx, -1);
        return;
    }
    Au au = read_au(rdram, auAddr);
    std::memset(out, 0, MPEG_ATRAC_ES_OUTPUT_SIZE);
    // PPSSPP MediaEngine::getNextAudioFrame + getAudioSamples.
    c->audioPts += AUDIO_TS_STEP;
    if (c->demux) {
        c->demux->demux();
        const uint8_t* frame = nullptr;
        int h1 = 0, h2 = 0;
        int64_t pts = 0;
        int size = c->demux->getNextAudioFrame(&frame, &h1, &h2, &pts);
        if (pts != 0) {
            c->audioPts = pts - c->firstTimestamp + AUDIO_TS_STEP;
        }
        if (size > 0) {
            int16_t pcm[2048 * 2];
            if (c->audio.decode(frame, size, h1, h2, pcm, 2048) > 0) {
                std::memcpy(out, pcm, sizeof(pcm));
            }
            c->audioFrameCount++;
        }
    }
    au.pts = c->audioPts - AUDIO_TS_STEP + c->firstTimestamp;
    write_au(rdram, auAddr, au);
    sync_avail(rdram, c);
    ret(ctx, 0);
}

void hle_sceMpegAvcDecodeYCbCr(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    const uint32_t auAddr = arg(ctx, 1);
    const uint32_t bufferAddr = arg(ctx, 2);
    const uint32_t initAddr = arg(ctx, 3);
    Au au = read_au(rdram, auAddr);
    Ring r;
    if (!read_ring(rdram, c->ringAddr, &r)) {
        ret(ctx, -1);
        return;
    }
    if (r.packetsRead == 0 || c->videoEnd) {
        ret(ctx, ERR_AVC_DECODE_FATAL);
        return;
    }
    if (!gptr(rdram, bufferAddr, 4) || !gptr(rdram, initAddr, 4)) {
        ret(ctx, -1);
        return;
    }
    if (step_video(c)) {
        c->avcFrameStatus = 1;
        c->videoFrameCount++;
    } else {
        c->avcFrameStatus = 0;
    }
    {
        static int calls = 0, misses = 0;
        calls++;
        misses += c->avcFrameStatus == 0;
        if ((calls % 120) == 0) {
            std::fprintf(stderr,
                         "[HLE] sceMpegAvcDecodeYCbCr stats: %d calls, %d without picture, "
                         "pending video bytes=%zu ended=%d\n",
                         calls, misses, c->demux ? c->demux->pendingVideoBytes() : 0,
                         c->inputEnded ? 1 : 0);
        }
    }
    r.packetsAvail = occupied_packets(c, r.packets);
    write_ring(rdram, c->ringAddr, r);
    au.pts = c->videoPts + c->firstTimestamp;
    write_au(rdram, auAddr, au);
    wr32(rdram, initAddr, (uint32_t)c->avcFrameStatus);
    if (c->videoFrameCount == 1 && c->avcFrameStatus == 1) {
        std::fprintf(stderr, "[HLE] sceMpegAvcDecodeYCbCr: first picture %dx%d (ffmpeg=%d)\n",
                     c->video.width(), c->video.height(), PspVideoDecoder::available() ? 1 : 0);
    }
    ret(ctx, 0);
}

void hle_sceMpegAvcDecodeStopYCbCr(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t statusAddr = arg(ctx, 2);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!get_ctx(rdram, arg(ctx, 0))) {
        ret(ctx, -1);
        return;
    }
    wr32(rdram, statusAddr, 0);
    ret(ctx, 0);
}

void hle_sceMpegAvcQueryYCbCrSize(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t width = arg(ctx, 2);
    const uint32_t height = arg(ctx, 3);
    const uint32_t resultAddr = (uint32_t)ctx->r[8];
    if ((width & 15) != 0 || (height & 15) != 0 || height > 272 || width > 480) {
        ret(ctx, ERR_INVALID_VALUE);
        return;
    }
    wr32(rdram, resultAddr, (width / 2) * (height / 2) * 6 + 128);
    ret(ctx, 0);
}

void hle_sceMpegAvcInitYCbCr(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    ret(ctx, get_ctx(rdram, arg(ctx, 0)) ? 0 : -1);
}

void hle_sceMpegAvcCsc(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t rangeAddr = arg(ctx, 2);
    int frameWidth = (int)arg(ctx, 3);
    const uint32_t destAddr = (uint32_t)ctx->r[8];
    uint8_t* range = gptr(rdram, rangeAddr, 16);
    if (!gptr(rdram, arg(ctx, 1), 1) || !range || !gptr(rdram, destAddr, 1)) {
        ret(ctx, -1);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    if (frameWidth == 0) {
        frameWidth = c->defaultFrameWidth ? c->defaultFrameWidth : c->frameWidth;
    }
    int32_t x, y, w, h;
    std::memcpy(&x, range, 4);
    std::memcpy(&y, range + 4, 4);
    std::memcpy(&w, range + 8, 4);
    std::memcpy(&h, range + 12, 4);
    if (x < 0 || y < 0 || w < 0 || h < 0) {
        ret(ctx, ERR_INVALID_VALUE);
        return;
    }
    const int bpp = psp_video_bytes_per_pixel(c->videoPixelMode);
    const uint64_t want = (uint64_t)frameWidth * (uint64_t)bpp * (uint64_t)h;
    const uint64_t room = PSP_MEM_SIZE - (destAddr & kAddrMask);
    uint8_t* dst = gptr(rdram, destAddr, 1);
    static int csc_logs = 0;
    if (csc_logs < 3 || (c->videoFrameCount % 200) == 0) {
        csc_logs++;
        std::fprintf(stderr,
                     "[HLE] sceMpegAvcCsc dest=0x%08X fw=%d mode=%d range=%d,%d %dx%d frame=%d\n",
                     destAddr, frameWidth, c->videoPixelMode, x, y, w, h, c->videoFrameCount);
    }
    c->video.writeImage(dst, (size_t)std::min(want, room), frameWidth, c->videoPixelMode, x, y, w,
                        h);
    ret(ctx, 0);
}

void hle_sceMpegFlushAllStream(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    MpegCtx* c = get_ctx(rdram, arg(ctx, 0));
    if (!c) {
        ret(ctx, -1);
        return;
    }
    c->analyzed = false;
    Ring r;
    if (read_ring(rdram, c->ringAddr, &r)) {
        r.packetsAvail = 0;
        r.packetsRead = 0;
        r.packetsWritePos = 0;
        write_ring(rdram, c->ringAddr, r);
    }
    ret(ctx, 0);
}

}  // namespace

void psp_hle_mpeg_set_guest_call(PspMpegGuestCall fn) { g_guest_call = fn; }

void psp_hle_mpeg_reset_for_tests() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_ctxs.clear();
    g_streamIdGen = 1;
}

void psp_hle_register_mpeg() {
    psp_hle_register("sceMpegInit", hle_sceMpegInit);
    psp_hle_register("sceMpegFinish", hle_sceMpegFinish);
    psp_hle_register("sceMpegCreate", hle_sceMpegCreate);
    psp_hle_register("sceMpegDelete", hle_sceMpegDelete);
    psp_hle_register("sceMpegQueryMemSize", hle_sceMpegQueryMemSize);
    psp_hle_register("sceMpegQueryStreamOffset", hle_sceMpegQueryStreamOffset);
    psp_hle_register("sceMpegQueryStreamSize", hle_sceMpegQueryStreamSize);
    psp_hle_register("sceMpegRegistStream", hle_sceMpegRegistStream);
    psp_hle_register("sceMpegUnRegistStream", hle_sceMpegUnRegistStream);
    psp_hle_register("sceMpegFlushAllStream", hle_sceMpegFlushAllStream);
    psp_hle_register("sceMpegMallocAvcEsBuf", hle_sceMpegMallocAvcEsBuf);
    psp_hle_register("sceMpegFreeAvcEsBuf", hle_sceMpegFreeAvcEsBuf);
    psp_hle_register("sceMpegInitAu", hle_sceMpegInitAu);
    psp_hle_register("sceMpegGetAtracAu", hle_sceMpegGetAtracAu);
    psp_hle_register("sceMpegGetAvcAu", hle_sceMpegGetAvcAu);
    psp_hle_register("sceMpegAtracDecode", hle_sceMpegAtracDecode);
    psp_hle_register("sceMpegAvcDecodeYCbCr", hle_sceMpegAvcDecodeYCbCr);
    psp_hle_register("sceMpegAvcDecodeStopYCbCr", hle_sceMpegAvcDecodeStopYCbCr);
    psp_hle_register("sceMpegAvcQueryYCbCrSize", hle_sceMpegAvcQueryYCbCrSize);
    psp_hle_register("sceMpegAvcInitYCbCr", hle_sceMpegAvcInitYCbCr);
    psp_hle_register("sceMpegAvcCsc", hle_sceMpegAvcCsc);
    psp_hle_register("sceMpegQueryAtracEsSize", hle_sceMpegQueryAtracEsSize);
    psp_hle_register("sceMpegRingbufferConstruct", hle_sceMpegRingbufferConstruct);
    psp_hle_register("sceMpegRingbufferDestruct", hle_sceMpegRingbufferDestruct);
    psp_hle_register("sceMpegRingbufferQueryMemSize", hle_sceMpegRingbufferQueryMemSize);
    psp_hle_register("sceMpegRingbufferPut", hle_sceMpegRingbufferPut);
    psp_hle_register("sceMpegRingbufferAvailableSize", hle_sceMpegRingbufferAvailableSize);
}
