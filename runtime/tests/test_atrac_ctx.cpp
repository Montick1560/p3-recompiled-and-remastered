// Unit tests for the ATRAC3/ATRAC3plus playback context (hle/psp_atrac_ctx.h),
// ported from PPSSPP Core/HLE/AtracCtx2.cpp. Standalone: no SDL/GL/scheduler.
//
// Expected values in the streaming scenarios were derived by hand from the
// PPSSPP algorithms (see the arithmetic in the comments), so an off-by-one in
// the wrap/loop bookkeeping fails here. A fake PCM decoder records which file
// frame each decode consumed, which proves the ring-buffer reads line up with
// the file data the "game" supplied.

#include "hle/psp_atrac_ctx.h"
#include "hle/psp_hle.h"
#include "psp_memory.h"
#include "recomp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace psp_atrac;

static int failures = 0;
static int tests_run = 0;

#define CHECK_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        long long a_ = (long long)(actual), e_ = (long long)(expected); \
        if (a_ != e_) { \
            std::fprintf(stderr, "FAIL: %s:%d: %s: got %lld (0x%llx), " \
                "expected %lld (0x%llx)\n", __FILE__, __LINE__, msg, a_, \
                (unsigned long long)a_, e_, (unsigned long long)e_); \
            failures++; \
        } \
    } while (0)

#define CHECK_TRUE(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, \
                msg); \
            failures++; \
        } \
    } while (0)

// ---- Synthetic RIFF builder ----

static void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
}
static void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xFF);
}
static void putTag(std::vector<uint8_t>& v, const char* t) {
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)t[i]);
}

struct WaveSpec {
    bool at3 = false;           // ATRAC3 (0x270) instead of ATRAC3plus
    int channels = 2;
    int blockAlign = 0x118;
    bool joint = false;         // ATRAC3 joint stereo flag
    bool fact = true;
    uint32_t factSamples = 36000;
    uint32_t factFirst = 2048;
    bool smpl = false;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    int frames = 20;            // frames in the data chunk
};

// Frame k's first 4 bytes carry k (for the fake decoder); the rest is a
// per-frame filler.
static std::vector<uint8_t> buildWave(const WaveSpec& s) {
    std::vector<uint8_t> body;
    putTag(body, "WAVE");
    // fmt
    putTag(body, "fmt ");
    if (s.at3) {
        put32(body, 32);
        put16(body, 0x0270);
    } else {
        put32(body, 52);
        put16(body, 0xFFFE);
    }
    put16(body, s.channels);
    put32(body, 44100);
    put32(body, 16000);
    put16(body, s.blockAlign);
    put16(body, s.at3 ? 0 : 0x0800);
    if (s.at3) {
        put16(body, 14);          // cbSize
        put16(body, 1);           // extra[0..1]
        put16(body, 0x0800);      // extra[2..3] samples per channel
        put16(body, 0);           // extra[4..5]
        put16(body, s.joint ? 1 : 0);  // extra[6..7] coding mode
        put16(body, s.joint ? 1 : 0);  // extra[8..9]
        put16(body, 1);           // extra[10..11] frame factor
        put16(body, 0);           // extra[12..13]
    } else {
        put16(body, 34);          // cbSize
        put16(body, 0x0800);      // valid bits
        put32(body, 3);           // channel mask
        static const uint8_t guid[16] = {
            0xbf, 0xaa, 0x23, 0xe9, 0x58, 0xcb, 0x71, 0x44,
            0xa1, 0x19, 0xff, 0xfa, 0x01, 0xe4, 0xce, 0x62};
        body.insert(body.end(), guid, guid + 16);
        put16(body, 1);
        put16(body, 0x2228);      // codec extra data
        for (int i = 0; i < 8; i++) body.push_back(0);
    }
    if (s.fact) {
        putTag(body, "fact");
        put32(body, 8);
        put32(body, s.factSamples);
        put32(body, s.factFirst);
    }
    if (s.smpl) {
        putTag(body, "smpl");
        put32(body, 36 + 24);
        for (int i = 0; i < 7; i++) put32(body, 0);
        put32(body, 1);           // numSampleLoops
        put32(body, 0);           // samplerData
        put32(body, 0);           // cuePointID
        put32(body, 0);           // type
        put32(body, s.loopStart);
        put32(body, s.loopEnd);
        put32(body, 0);           // fraction
        put32(body, 0);           // playCount
    }
    putTag(body, "data");
    put32(body, (uint32_t)s.frames * s.blockAlign);
    for (int f = 0; f < s.frames; f++) {
        size_t at = body.size();
        put32(body, (uint32_t)f);
        body.resize(at + s.blockAlign, (uint8_t)(0x40 + f));
        // restore the marker (resize only appended).
        std::memcpy(&body[at], &f, 4);
    }
    std::vector<uint8_t> out;
    putTag(out, "RIFF");
    put32(out, (uint32_t)body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// ---- Fake PCM decoder ----

static std::vector<int> g_seen;  // file frame index of every decode

class FakeDecoder : public PcmDecoder {
public:
    FakeDecoder(int spf) : spf_(spf) {}
    bool Decode(const uint8_t* in, int inBytes, int* consumed, int outChannels,
                int16_t* out, int* outSamples) override {
        int idx = 0;
        std::memcpy(&idx, in, 4);
        g_seen.push_back(idx);
        *consumed = inBytes;
        *outSamples = spf_;
        if (out) {
            for (int i = 0; i < spf_ * outChannels; i++) {
                out[i] = (int16_t)(idx * 100 + (i % 7));
            }
        }
        return true;
    }
private:
    int spf_;
};

static std::unique_ptr<PcmDecoder> fakeFactory(
    int codecType, int, int, bool) {
    return std::unique_ptr<PcmDecoder>(new FakeDecoder(
        codecType == CODEC_AT3PLUS ? 2048 : 1024));
}

// ---- Guest memory ----

static constexpr size_t kRam = 16u * 1024 * 1024;   // test "rdram"
static constexpr uint32_t kBuf = 0x08800000u;       // masks to 0x00800000
static constexpr uint32_t kOut = 0x08900000u;
static std::vector<uint8_t> g_ram(kRam);

static GuestMem mem() { return GuestMem(g_ram.data(), kRam); }

// ---- 1. Header parsing ----

static void test_parse_at3plus_full() {
    WaveSpec s;
    s.smpl = true;
    s.loopStart = 4000;
    s.loopEnd = 38047;
    std::vector<uint8_t> w = buildWave(s);
    TrackInfo t;
    int r = ParseWave(w.data(), (uint32_t)w.size(), &t);
    CHECK_EQ(r, 0, "parse ok");
    CHECK_EQ(t.codec, CODEC_AT3PLUS, "codec AT3+");
    CHECK_EQ(t.numChans, 2, "channels");
    CHECK_EQ(t.blockAlign, 0x118, "block align");
    CHECK_EQ(t.endSample, 36000, "fact sample count");
    CHECK_EQ(t.firstSampleOffset, 2048, "fact first sample offset");
    CHECK_EQ(t.loopStart, 4000, "smpl loop start");
    CHECK_EQ(t.loopEnd, 38047, "smpl loop end");
    // RIFF(12) + fmt(8+52) + fact(8+8) + smpl(8+60) + data hdr(8) = 164
    CHECK_EQ(t.dataOff, 164, "data offset");
    CHECK_EQ(t.waveDataSize, 20 * 0x118, "data size");
}

static void test_parse_at3plus_no_loop() {
    WaveSpec s;
    std::vector<uint8_t> w = buildWave(s);
    TrackInfo t;
    CHECK_EQ(ParseWave(w.data(), (uint32_t)w.size(), &t), 0, "parse ok");
    CHECK_EQ(t.loopStart, -1, "no loop start");
    CHECK_EQ(t.loopEnd, -1, "no loop end");
    CHECK_EQ(t.dataOff, 96, "data offset = 12+60+16+8");
}

static void test_parse_at3_joint() {
    WaveSpec s;
    s.at3 = true;
    s.blockAlign = 0xC0;
    s.joint = true;
    s.fact = false;
    std::vector<uint8_t> w = buildWave(s);
    TrackInfo t;
    CHECK_EQ(ParseWave(w.data(), (uint32_t)w.size(), &t), 0, "parse ok");
    CHECK_EQ(t.codec, CODEC_AT3, "codec AT3");
    CHECK_EQ(t.blockAlign, 0xC0, "block align");
    CHECK_TRUE(t.jointStereo, "joint stereo flag from fmt extra data");
    CHECK_EQ(t.endSample, 0, "no fact -> endSample 0");
}

static void test_parse_errors() {
    WaveSpec s;
    std::vector<uint8_t> w = buildWave(s);
    TrackInfo t;
    CHECK_EQ(ParseWave(w.data(), 71, &t), ERR_SIZE_TOO_SMALL,
             "under 72 bytes");
    std::vector<uint8_t> bad = w;
    bad[0] = 'X';
    CHECK_EQ(ParseWave(bad.data(), (uint32_t)bad.size(), &t),
             ERR_UNKNOWN_FORMAT, "not RIFF");
    bad = w;
    bad[8] = 'X';
    CHECK_EQ(ParseWave(bad.data(), (uint32_t)bad.size(), &t),
             ERR_UNKNOWN_FORMAT, "not WAVE");
    WaveSpec three = s;
    three.channels = 3;
    std::vector<uint8_t> w3 = buildWave(three);
    CHECK_EQ(ParseWave(w3.data(), (uint32_t)w3.size(), &t),
             ERR_UNKNOWN_FORMAT, "3 channels");
    // Header cut before the data chunk.
    CHECK_EQ(ParseWave(w.data(), 90, &t), ERR_SIZE_TOO_SMALL,
             "data chunk header missing");
    // Corrupt chunk size must not run off the buffer.
    bad = w;
    bad[16] = 0xFF; bad[17] = 0xFF; bad[18] = 0xFF; bad[19] = 0x7F;
    int r = ParseWave(bad.data(), (uint32_t)bad.size(), &t);
    CHECK_TRUE(r < 0, "oversized fmt chunk rejected");
}

// ---- 2. Out-of-range guest addresses ----

static void test_out_of_range_addresses() {
    GuestMem m = mem();
    CHECK_TRUE(!m.IsValid(0), "null invalid");
    CHECK_TRUE(!m.IsValid(0x1000), "null page invalid");
    CHECK_TRUE(m.IsValid(0x08800000u), "ram valid");
    CHECK_TRUE(!m.IsValid(0x0B000000u), "past the (small) mapping invalid");
    CHECK_TRUE(!m.IsValidRange(0x08FFFFF0u, 0x100), "range straddling end");
    CHECK_TRUE(m.Ptr(0x08FFFFF0u, 0x100) == nullptr, "Ptr rejects range");

    AtracCtx c(m, CODEC_AT3PLUS, fakeFactory);
    // Pointer outside RAM: treated as zero data -> format error, no crash.
    CHECK_EQ(c.SetData(0x0B000000u, 0x1000, 0x1000, 2), ERR_UNKNOWN_FORMAT,
             "SetData with unmapped buffer");
    CHECK_EQ(c.SetData(0, 10, 10, 2), ERR_SIZE_TOO_SMALL,
             "SetData null + tiny size");
    CHECK_EQ(c.SetData(0xFFFFFFF0u, 0x1000, 0x1000, 2), ERR_UNKNOWN_FORMAT,
             "SetData wild buffer");
    CHECK_EQ(c.BufferState(), STATUS_NO_DATA, "still NO_DATA");
    CHECK_EQ(c.ValidateData(), ERR_NO_DATA, "validate: no data");
}

static void test_decode_to_bad_output_address() {
    WaveSpec w;
    w.frames = 8;
    w.factSamples = 12000;
    std::vector<uint8_t> file = buildWave(w);
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], file.data(), file.size());
    AtracCtx c(mem(), CODEC_AT3PLUS, fakeFactory);
    CHECK_EQ(c.SetData(kBuf, (uint32_t)file.size(), (uint32_t)file.size(), 2),
             0, "SetData ok");
    int samples = 0, finish = 0, remains = 0;
    CHECK_EQ(c.DecodeData(0x0B000000u, &samples, &finish, &remains),
             ERR_SIZE_TOO_SMALL, "unmapped output -> error, no crash");
    CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains), 0,
             "mapped output decodes");
}

// ---- 3. Streaming bookkeeping across a wrap ----
//
// 280-byte AT3+ frames, 20 in the file, header 96 bytes -> dataOff 96,
// fileDataEnd 5696. Streaming buffer 1220 bytes, initially filled with the
// header plus 3 frames (readSize 936).

static constexpr int kS = 0x118;       // 280
static constexpr uint32_t kBufBytes = 1220;

struct Game {
    std::vector<uint8_t> file;
    AtracCtx* ctx;
    // Plays the game's part: copy the requested file range into the ring
    // buffer at the offered address and report it.
    bool feedOnce(int maxBytes = 1 << 30) {
        uint32_t wp = 0, wb = 0, ro = 0;
        ctx->GetStreamDataInfo(&wp, &wb, &ro);
        uint32_t n = std::min<uint32_t>(wb, (uint32_t)maxBytes);
        if (n == 0) return false;
        if (ro + n > file.size()) n = (uint32_t)file.size() - ro;
        std::memcpy(&g_ram[wp & GuestMem::kAddrMask], &file[ro], n);
        return ctx->AddStreamData(n) == 0;
    }
};

static void test_streaming_wrap_bookkeeping() {
    WaveSpec w;
    Game g;
    g.file = buildWave(w);
    CHECK_EQ(g.file.size(), 5696u, "file size");
    uint32_t readSize = 96 + 3 * kS;  // 936
    std::memset(&g_ram[kBuf & GuestMem::kAddrMask], 0, kBufBytes);
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], g.file.data(), readSize);
    AtracCtx c(mem(), CODEC_AT3PLUS, fakeFactory);
    g.ctx = &c;
    g_seen.clear();
    CHECK_EQ(c.SetData(kBuf, readSize, kBufBytes, 2), 0, "SetData streaming");
    const IdInfo& i = c.Info();
    CHECK_EQ(i.state, STATUS_STREAMED_WITHOUT_LOOP, "state");
    CHECK_EQ(i.dataOff, 96, "dataOff");
    CHECK_EQ(i.fileDataEnd, 5696, "fileDataEnd");
    // endSample = fact(36000) + firstValid(2048+368) - 1
    CHECK_EQ(i.endSample, 38415, "endSample");
    CHECK_EQ(i.firstValidSample, 2416, "firstValidSample");
    CHECK_EQ(i.decodePos, 2416, "decodePos");
    // 2416 >> 11 = 1 frame is decoded and thrown away by SetData.
    CHECK_EQ((int)g_seen.size(), 1, "one skipped frame decoded");
    CHECK_EQ(g_seen[0], 0, "skipped frame is file frame 0");
    CHECK_EQ(i.numSkipFrames, 0, "no frames left to skip");
    CHECK_EQ(i.curFileOff, 96 + kS, "curFileOff after skip");
    CHECK_EQ(i.streamDataByte, 840 - kS, "streamDataByte after skip");
    CHECK_EQ(i.streamOff, 96 + kS, "streamOff after skip");

    uint32_t wp = 0, wb = 0, ro = 0;
    c.GetStreamDataInfo(&wp, &wb, &ro);
    // rounded end = 376 + floor((1220-376)/280)*280 = 1216; used 560 ->
    // streamPos 936 -> space 280; read from 376+560 = 936.
    CHECK_EQ(wp, kBuf + 936, "writePtr before wrap");
    CHECK_EQ(wb, 280, "writable before wrap");
    CHECK_EQ(ro, 936, "readOffset before wrap");
    CHECK_EQ(c.RemainingFrames(), 2, "remaining frames (560/280)");

    CHECK_EQ(c.AddStreamData(kS), 0, "AddStreamData");
    // (data not copied: this probe only looks at the offsets)
    c.GetStreamDataInfo(&wp, &wb, &ro);
    // streamPos 1216 >= 1216 -> space = 1216 - 840 = 376, written at the
    // start of the buffer (wrap); file offset continues at 1216.
    CHECK_EQ(wp, kBuf, "writePtr wraps to buffer start");
    CHECK_EQ(wb, 376, "writable after wrap");
    CHECK_EQ(ro, 1216, "readOffset after wrap");

    // Restore consistency: make the buffer really hold frames 1,2,3 at
    // 376/656/936 (the AddStreamData above claimed frame 3 was there).
    std::memcpy(&g_ram[(kBuf + 936) & GuestMem::kAddrMask],
                &g.file[936], kS);
    g_seen.clear();
    int samples = 0, finish = 0, remains = 0;
    // First real frame: decodePos 2416 -> only 2048-368 samples are valid.
    CHECK_EQ(c.GetNextSamples(), 1680u, "partial first frame");
    CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains), 0, "decode 1");
    CHECK_EQ(samples, 1680, "first frame sample count");
    CHECK_EQ(finish, 0, "not finished");
    CHECK_EQ(g_seen.back(), 1, "decoded file frame 1");
    CHECK_EQ(i.decodePos, 4096, "decodePos is frame aligned now");
    CHECK_EQ(c.GetNextSamples(), 2048u, "full frames from here");
    CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains), 0, "decode 2");
    CHECK_EQ(samples, 2048, "full frame");
    CHECK_EQ(g_seen.back(), 2, "decoded file frame 2");
    CHECK_EQ(i.streamOff, 936, "streamOff at last slot");
    CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains), 0, "decode 3");
    CHECK_EQ(g_seen.back(), 3, "decoded file frame 3");
    // 936 + 280 = 1216; 1216 + 280 > 1220 -> wraps to 0.
    CHECK_EQ(i.streamOff, 0, "streamOff wrapped");
    CHECK_EQ(i.streamDataByte, 0, "buffer drained");
    CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains),
             ERR_BUFFER_IS_EMPTY, "empty ring buffer");
    CHECK_EQ(samples, 0, "no samples on error");
    CHECK_EQ(finish, 0, "empty buffer is not the end");
    CHECK_EQ(i.curFileOff, 96 + 4 * kS, "curFileOff = 1216");
    // PCM landed in guest memory: frame 3 decoded last (value 300 + i%7).
    CHECK_EQ(g_ram[(kOut) & GuestMem::kAddrMask], (300 & 0xFF),
             "PCM low byte written");
}

// Plays a whole synthetic file through the ring buffer, with the "game"
// refilling it from the file exactly as GetStreamDataInfo asks, and returns
// the sequence of file frames the decoder consumed. `loopNum` > 0 requires a
// file whose smpl loop ends at its last sample (STREAMED_LOOP_FROM_END).
struct PlayResult {
    bool setDataOk = false;
    std::vector<int> frames;
    int finish = 0;
    int stalls = 0;      // BUFFER_IS_EMPTY while the game had data to give
    long long samples = 0;
    int jumps = 0;
    int lastError = 0;
    int lastRemains = 0;
    IdInfo end;
};

static PlayResult playThrough(const WaveSpec& w, uint32_t bufBytes,
                              int loopNum) {
    PlayResult res;
    Game g;
    g.file = buildWave(w);
    TrackInfo t;
    if (ParseWave(g.file.data(), (uint32_t)g.file.size(), &t) != 0) return res;
    uint32_t readSize = (uint32_t)t.dataOff + 3 * (uint32_t)w.blockAlign;
    std::memset(&g_ram[kBuf & GuestMem::kAddrMask], 0, bufBytes);
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], g.file.data(), readSize);
    AtracCtx c(mem(), CODEC_AT3PLUS, fakeFactory);
    g.ctx = &c;
    g_seen.clear();
    res.setDataOk = c.SetData(kBuf, readSize, bufBytes, 2) == 0;
    if (!res.setDataOk) return res;
    if (loopNum) c.SetLoopNum(loopNum);
    int samples = 0, finish = 0, remains = 0, guard = 0;
    int prevPos = c.Info().decodePos;
    while (!finish && guard++ < 2000) {
        while (g.feedOnce()) {}
        int r = c.DecodeData(kOut, &samples, &finish, &remains);
        if (r == ERR_BUFFER_IS_EMPTY) { res.stalls++; break; }
        if (r != 0) { res.lastError = r; break; }
        res.samples += samples;
        res.lastRemains = remains;
        if (c.Info().decodePos < prevPos) res.jumps++;
        prevPos = c.Info().decodePos;
    }
    res.finish = finish;
    res.frames = g_seen;
    res.end = c.Info();
    return res;
}

static std::vector<int> range(int from, int to) {  // inclusive
    std::vector<int> v;
    for (int i = from; i <= to; i++) v.push_back(i);
    return v;
}

static std::vector<int> concat(std::vector<int> a, const std::vector<int>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static void test_streaming_full_pass_in_order() {
    WaveSpec w;  // 20 frames, endSample = 36000 + 2416 - 1 = 38415 (frame 18)
    PlayResult r = playThrough(w, kBufBytes, 0);
    CHECK_TRUE(r.setDataOk, "SetData");
    CHECK_TRUE(r.frames == range(0, 18), "frames 0..18 in order");
    CHECK_EQ(r.finish, 1, "finish flag raised at the end");
    // Valid samples = endSample - firstValid + 1.
    CHECK_EQ(r.samples, 38415 - 2416 + 1, "total output samples");
    CHECK_EQ(r.stalls, 0, "no empty-buffer stalls");
    CHECK_EQ(r.end.decodePos, 38416, "decodePos ends one past endSample");
    // The whole file was fed: the unplayed frame 19 (280 bytes) is exactly
    // what is left in the buffer and in the file -> "all on memory".
    CHECK_EQ(r.lastRemains, REMAIN_NONLOOP_STREAM_DATA_IS_ON_MEMORY,
             "remain frames = -2 once the file tail is buffered");
}

// Every buffer size, including ones where the last packet in the ring ends
// exactly on the buffer end (>= vs > on the wrap test) or leaves a few spare
// bytes, must deliver the same frames.
static void test_streaming_buffer_size_sweep() {
    WaveSpec w;
    int bad = 0, tested = 0;
    for (uint32_t bytes = 96 + 3 * kS; bytes < 2400; bytes++) {
        PlayResult r = playThrough(w, bytes, 0);
        tested++;
        if (!r.setDataOk || r.frames != range(0, 18) || !r.finish ||
            r.stalls) {
            if (bad++ < 5) {
                std::fprintf(stderr, "sweep: buffer %u: ok=%d frames=%zu "
                             "finish=%d stalls=%d err=%x\n", bytes,
                             r.setDataOk, r.frames.size(), r.finish,
                             r.stalls, (unsigned)r.lastError);
            }
        }
    }
    CHECK_EQ(bad, 0, "all buffer sizes stream the file in order");
    CHECK_TRUE(tested > 1000, "swept many buffer sizes");
}

// The last sample of the track sits exactly on a frame boundary: the frame
// before it must NOT raise the finish flag; a 1-sample frame follows.
static void test_end_on_frame_boundary() {
    WaveSpec w;
    w.factSamples = 34449;   // endSample = 34449 + 2416 - 1 = 36864 = 18*2048
    PlayResult r = playThrough(w, kBufBytes, 0);
    CHECK_TRUE(r.setDataOk, "SetData");
    CHECK_EQ(r.end.endSample, 36864, "endSample on a frame boundary");
    CHECK_TRUE(r.frames == range(0, 18), "frames 0..18 (18 has 1 sample)");
    CHECK_EQ(r.samples, 36864 - 2416 + 1, "total samples");
    CHECK_EQ(r.finish, 1, "finish only after the 1-sample frame");
    CHECK_EQ(r.end.decodePos, 36865, "decodePos");

    // Step manually (whole file in memory) around the end.
    std::vector<uint8_t> file = buildWave(w);
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], file.data(), file.size());
    AtracCtx c(mem(), CODEC_AT3PLUS, fakeFactory);
    CHECK_EQ(c.SetData(kBuf, (uint32_t)file.size(), (uint32_t)file.size(), 2),
             0, "SetData whole file");
    int samples = 0, finish = 0, remains = 0, pos = -1;
    for (int n = 0; n < 17; n++) {  // frames 1..17
        CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains), 0, "decode");
    }
    CHECK_EQ(c.Info().decodePos, 36864, "decodePos == endSample");
    CHECK_EQ(c.GetNextDecodePosition(&pos), 0, "one sample still to decode");
    CHECK_EQ(pos, 36864 - 2416, "next decode position");
    CHECK_EQ(c.GetNextSamples(), 1u, "single trailing sample");
    CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains), 0, "last frame");
    CHECK_EQ(samples, 1, "1 sample");
    CHECK_EQ(finish, 1, "finish");
    CHECK_EQ(c.GetNextDecodePosition(&pos), ERR_ALL_DATA_DECODED, "all done");
    CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains),
             ERR_ALL_DATA_DECODED, "decode past the end");
    CHECK_EQ(finish, 1, "finish stays set");
    CHECK_EQ(samples, 0, "no samples past the end");
}

// ---- 4. Loop handling ----

static WaveSpec loopSpec(uint32_t rawLoopStart, bool endOnBoundary) {
    WaveSpec w;
    w.smpl = true;
    w.loopStart = rawLoopStart;
    if (endOnBoundary) {
        w.factSamples = 34449;           // endSample 36864
        w.loopEnd = 36864 - 368;         // +0x170 == endSample -> FROM_END
    } else {
        w.loopEnd = 38415 - 368;
    }
    return w;
}

static void test_streaming_loop_from_end() {
    WaveSpec w = loopSpec(4000, false);
    std::vector<uint8_t> file = buildWave(w);
    TrackInfo t;
    CHECK_EQ(ParseWave(file.data(), (uint32_t)file.size(), &t), 0, "parse");
    uint32_t readSize = (uint32_t)t.dataOff + 3 * kS;
    std::memset(&g_ram[kBuf & GuestMem::kAddrMask], 0, kBufBytes);
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], file.data(), readSize);
    AtracCtx c(mem(), CODEC_AT3PLUS, fakeFactory);
    CHECK_EQ(c.SetData(kBuf, readSize, kBufBytes, 2), 0, "SetData loop file");
    const IdInfo& i = c.Info();
    CHECK_EQ(i.state, STATUS_STREAMED_LOOP_FROM_END, "state LOOP_FROM_END");
    CHECK_EQ(i.loopStart, 4368, "loopStart = raw + 0x170");
    CHECK_EQ(i.loopEnd, 38415, "loopEnd = raw + 0x170");
    CHECK_EQ(c.LoopStatus(), 1, "before the loop point -> status 1");
    CHECK_EQ(c.SetLoopNum(2), 0, "SetLoopNum on a looping file");
    CHECK_EQ(c.LoopNum(), 2, "loop counter set");
    CHECK_EQ(c.LoopStatus(), 1, "loop pending");

    // GetSoundSample reports positions relative to the first valid sample.
    int es = 0, ls = 0, le = 0;
    CHECK_EQ(c.GetSoundSample(&es, &ls, &le), 0, "GetSoundSample");
    CHECK_EQ(es, 38415 - 2416, "end sample");
    CHECK_EQ(ls, 4368 - 2416, "loop start sample");
    CHECK_EQ(le, 38415 - 2416, "loop end sample");
}

static void checkLoopPlay(const char* name, const WaveSpec& w, int loopNum,
                          uint32_t bufBytes, int expectFirstAfterLoop) {
    PlayResult r = playThrough(w, bufBytes, loopNum);
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s buf=%u: SetData", name, bufBytes);
    CHECK_TRUE(r.setDataOk, msg);
    const int last = r.end.endSample / 2048;
    std::vector<int> expect = range(0, last);
    for (int n = 0; n < loopNum; n++) {
        expect = concat(expect, range(expectFirstAfterLoop, last));
    }
    std::snprintf(msg, sizeof msg, "%s buf=%u: decoded frame sequence",
                  name, bufBytes);
    CHECK_TRUE(r.frames == expect, msg);
    if (r.frames != expect) {
        std::fprintf(stderr, "  got %zu frames, expected %zu; first diff:",
                     r.frames.size(), expect.size());
        for (size_t k = 0; k < std::min(r.frames.size(), expect.size()); k++) {
            if (r.frames[k] != expect[k]) {
                std::fprintf(stderr, " idx %zu got %d want %d", k, r.frames[k],
                             expect[k]);
                break;
            }
        }
        std::fprintf(stderr, "\n");
    }
    std::snprintf(msg, sizeof msg, "%s buf=%u: jump count", name, bufBytes);
    CHECK_EQ(r.jumps, loopNum, msg);
    std::snprintf(msg, sizeof msg, "%s buf=%u: finish after last pass", name,
                  bufBytes);
    CHECK_EQ(r.finish, 1, msg);
    CHECK_EQ(r.end.loopNum, 0, "loop counter exhausted");
}

static void test_loop_playback() {
    // loopStart raw 4000 -> info 4368: (4368 & 2047) = 272 < 368, so the
    // loop restarts two frames early, at the very first frame (skip 2).
    checkLoopPlay("loop@272", loopSpec(4000, false), 2, kBufBytes, 0);
    // raw 4096 -> info 4464: (4464 & 2047) == 368 == SkipSamples exactly:
    // only one frame is skipped, restart at file frame 1.
    checkLoopPlay("loop@368", loopSpec(4096, false), 2, kBufBytes, 1);
    checkLoopPlay("loop@369", loopSpec(4097, false), 2, kBufBytes, 1);
    checkLoopPlay("loop@367", loopSpec(4095, false), 2, kBufBytes, 0);
    // Loop end on a frame boundary: the jump happens after the 1-sample
    // frame (decodePos 36865 > loopEnd 36864), not before.
    checkLoopPlay("end@2048k", loopSpec(4000, true), 2, kBufBytes, 0);
    checkLoopPlay("loop@368,end@2048k", loopSpec(4096, true), 1, kBufBytes, 1);
    // Different buffer sizes.
    for (uint32_t bytes = 164 + 3 * kS; bytes < 2000; bytes += 7) {
        checkLoopPlay("sweep", loopSpec(4000, false), 2, bytes, 0);
    }
}

static void test_set_loop_num_without_loop() {
    WaveSpec w;
    w.frames = 8;
    w.factSamples = 12000;
    std::vector<uint8_t> file = buildWave(w);
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], file.data(), file.size());
    AtracCtx c(mem(), CODEC_AT3PLUS, fakeFactory);
    CHECK_EQ(c.SetData(kBuf, (uint32_t)file.size(), (uint32_t)file.size(), 2),
             0, "SetData whole file");
    CHECK_EQ(c.BufferState(), STATUS_ALL_DATA_LOADED, "all data loaded");
    CHECK_EQ(c.SetLoopNum(1), ERR_NO_LOOP_INFORMATION, "no loop in file");
    CHECK_EQ(c.LoopStatus(), 0, "no loop status");
    CHECK_EQ(c.RemainingFrames(), REMAIN_ALLDATA_IS_ON_MEMORY,
             "remain = -1 when all loaded");
}

// ---- 5. Reset play position ----

static void test_reset_play_position() {
    WaveSpec w;
    Game g;
    g.file = buildWave(w);
    uint32_t readSize = 96 + 3 * kS;
    std::memset(&g_ram[kBuf & GuestMem::kAddrMask], 0, kBufBytes);
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], g.file.data(), readSize);
    AtracCtx c(mem(), CODEC_AT3PLUS, fakeFactory);
    g.ctx = &c;
    CHECK_EQ(c.SetData(kBuf, readSize, kBufBytes, 2), 0, "SetData");

    ResetBufferInfo ri;
    bool delay = false;
    // sample 10000 + firstValid 2416 = 12416 -> frame 6, offset 128 < 368:
    // file offset ((6-1)*280 - 280) + 96 = 1216; two frames to skip.
    CHECK_EQ(c.GetBufferInfoForResetting(&ri, 10000, &delay), 0, "reset info");
    CHECK_EQ(ri.first.writePosPtr, kBuf, "first.writePosPtr");
    CHECK_EQ(ri.first.writableBytes, 1120, "first.writableBytes (1220 -> 4*280)");
    CHECK_EQ(ri.first.minWriteBytes, 3 * kS, "first.minWriteBytes (skip 2 + 1)");
    CHECK_EQ(ri.first.filePos, 1216, "first.filePos");
    CHECK_EQ(ri.second.writePosPtr, kBuf, "second.writePosPtr");
    CHECK_EQ(ri.second.writableBytes, 0, "second.writableBytes");
    CHECK_EQ(ri.second.minWriteBytes, 0, "second.minWriteBytes");
    CHECK_EQ(ri.second.filePos, 0, "second.filePos");
    CHECK_TRUE(!delay, "no skipped frames yet");

    CHECK_EQ(c.GetBufferInfoForResetting(&ri, 99999999, &delay),
             ERR_BAD_SAMPLE, "sample past the end");
    // The last valid seek position is endSample - firstValid = 35999.
    CHECK_EQ(c.GetBufferInfoForResetting(&ri, 36000, &delay), ERR_BAD_SAMPLE,
             "one past the last sample");
    CHECK_EQ(c.GetBufferInfoForResetting(&ri, 35999, &delay), 0,
             "last sample is seekable");
    CHECK_EQ(c.ResetPlayPosition(36000, 840, 0, &delay), ERR_BAD_SAMPLE,
             "reset one past the last sample");
    // (seek + 2416) & 2047 == 368 exactly: one skip frame, file offset
    // (3-1)*280 + 96 = 656, min write (1+1) frames.
    CHECK_EQ(c.GetBufferInfoForResetting(&ri, 4096, &delay), 0, "boundary 368");
    CHECK_EQ(ri.first.filePos, 656, "filePos at skip boundary");
    CHECK_EQ(ri.first.minWriteBytes, 2 * kS, "minWrite at skip boundary");
    // One sample earlier: 367 < 368 -> two skip frames, one packet further back.
    CHECK_EQ(c.GetBufferInfoForResetting(&ri, 4095, &delay), 0, "boundary 367");
    CHECK_EQ(ri.first.filePos, 376, "filePos below skip boundary");
    CHECK_EQ(ri.first.minWriteBytes, 3 * kS, "minWrite below skip boundary");
    // Recompute the 10000 case (the calls above may have skipped frames).

    // Too few / too many bytes written.
    CHECK_EQ(c.ResetPlayPosition(10000, 3 * kS - 1, 0, &delay),
             ERR_BAD_FIRST_RESET_SIZE, "first buffer too small");
    CHECK_EQ(c.ResetPlayPosition(10000, 1121, 0, &delay),
             ERR_BAD_FIRST_RESET_SIZE, "first buffer too big");
    // The writable maximum itself is accepted.
    CHECK_EQ(c.ResetPlayPosition(10000, 1120, 0, &delay), 0,
             "exactly the writable bytes is fine");
    CHECK_EQ(c.Info().streamDataByte, 1120 - 2 * kS, "adopted all bytes");
    // Seeking to the very last sample: frame 18, one skip frame, file offset
    // 17*280 + 96 = 4856, min write 2 frames, writable 5696 - 4856 = 840.
    CHECK_EQ(c.GetBufferInfoForResetting(&ri, 35999, &delay), 0, "last info");
    CHECK_EQ(ri.first.filePos, 4856, "last sample filePos");
    CHECK_EQ(ri.first.minWriteBytes, 2 * kS, "last sample minWrite");
    CHECK_EQ(ri.first.writableBytes, 840, "last sample writable");
    CHECK_EQ(c.ResetPlayPosition(35999, 2 * kS, 0, &delay), 0,
             "reset to the last valid sample");
    CHECK_EQ(c.Info().decodePos, 38415, "decodePos == endSample");
    CHECK_EQ(c.ResetPlayPosition(10000, 3 * kS, 1, &delay),
             ERR_BAD_SECOND_RESET_SIZE, "second buffer must be 0");

    // The game supplies file[1216 .. 1216+840) at the buffer start.
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], &g.file[1216], 3 * kS);
    g_seen.clear();
    CHECK_EQ(c.ResetPlayPosition(10000, 3 * kS, 0, &delay), 0, "reset ok");
    CHECK_TRUE(delay, "skip frames decoded -> delay");
    const IdInfo& i = c.Info();
    CHECK_EQ(i.decodePos, 12416, "decodePos");
    CHECK_EQ(i.curFileOff, 1216 + 2 * kS, "two frames skipped");
    CHECK_EQ(i.streamDataByte, kS, "one frame left buffered");
    CHECK_EQ(i.streamOff, 2 * kS, "streamOff after skips");
    CHECK_EQ(i.numSkipFrames, 0, "skips done");
    CHECK_EQ((int)g_seen.size(), 2, "two skip decodes");
    CHECK_EQ(g_seen[0], (1216 - 96) / kS, "first skipped = file frame 4");
    CHECK_EQ(g_seen[1], (1216 - 96) / kS + 1, "second skipped = frame 5");
    CHECK_EQ(c.InternalCodecError(), 0u, "error cleared by good decodes");
    int samples = 0, finish = 0, remains = 0;
    CHECK_EQ(c.DecodeData(kOut, &samples, &finish, &remains), 0, "decode");
    CHECK_EQ(g_seen.back(), 6, "next frame is 6");
    CHECK_EQ(samples, 2048 - 128, "partial: 2048 - (12416 & 2047)");
}

// ---- 6. Real ATRAC3plus data (skipped when the disc is not extracted) ----

static std::string findBgmDat() {
    namespace fs = std::filesystem;
    std::vector<std::string> roots;
    if (const char* e = std::getenv("PSPRECOMP_DISC0")) roots.push_back(e);
    roots.push_back("C:/Users/torso/Documents/deco/disc0");
    for (const std::string& r : roots) {
        std::error_code ec;
        fs::path p = fs::path(r) / "SOUND" / "BGM_30" / "BGM.DAT";
        if (fs::exists(p, ec)) return p.string();
        if (!fs::is_directory(fs::path(r) / "SOUND", ec)) continue;
        for (auto it = fs::recursive_directory_iterator(
                 fs::path(r) / "SOUND", ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->path().filename() == "BGM.DAT") return it->path().string();
        }
    }
    return std::string();
}

static void test_real_atrac3plus() {
    std::string path = findBgmDat();
    if (path.empty()) {
        std::printf("SKIPPED: real-data test (BGM.DAT not found under disc0)\n");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    size_t at = std::string::npos;
    for (size_t i = 0; i + 12 <= dat.size(); i++) {
        if (!std::memcmp(&dat[i], "RIFF", 4) &&
            !std::memcmp(&dat[i + 8], "WAVE", 4)) { at = i; break; }
    }
    if (at == std::string::npos) {
        std::printf("SKIPPED: real-data test (no RIFF/WAVE in %s)\n",
                    path.c_str());
        return;
    }
    uint32_t riff = 0;
    std::memcpy(&riff, &dat[at + 4], 4);
    size_t len = std::min<size_t>(riff + 8, dat.size() - at);
    std::memcpy(&g_ram[kBuf & GuestMem::kAddrMask], &dat[at], len);

    AtracCtx c(mem(), CODEC_AT3PLUS, nullptr);  // real decoder
    int r = c.SetData(kBuf, (uint32_t)len, (uint32_t)len, 2);
    CHECK_EQ(r, 0, "SetData on real ATRAC3plus");
    CHECK_EQ(c.BufferState(), STATUS_ALL_DATA_LOADED, "all data loaded");
    CHECK_EQ(c.SamplesPerFrame(), 2048, "ATRAC3plus frame = 2048 samples");
    CHECK_EQ(c.Channels(), 2, "stereo");

    int samples = 0, finish = 0, remains = 0, frames = 0;
    long long energy = 0;
    int peak = 0, firstSamples = 0, secondSamples = 0;
    for (int n = 0; n < 12; n++) {
        std::memset(&g_ram[kOut & GuestMem::kAddrMask], 0, 8192);
        r = c.DecodeData(kOut, &samples, &finish, &remains);
        CHECK_EQ(r, 0, "real decode");
        if (r != 0) break;
        if (n == 0) firstSamples = samples;
        if (n == 1) secondSamples = samples;
        frames++;
        const int16_t* pcm =
            (const int16_t*)&g_ram[kOut & GuestMem::kAddrMask];
        for (int k = 0; k < samples * 2; k++) {
            energy += (long long)pcm[k] * pcm[k];
            peak = std::max(peak, std::abs((int)pcm[k]));
        }
    }
    CHECK_EQ(frames, 12, "decoded 12 frames");
    CHECK_EQ(firstSamples, 2048 - 368, "first frame is partial (368 skipped)");
    CHECK_EQ(secondSamples, 2048, "subsequent frames are 2048 samples");
    CHECK_TRUE(energy > 0 && peak > 100, "decoded audio is not silent");
    std::printf("real-data: %s @%zu, %zu bytes, peak=%d, rms=%.1f, "
                "frame=%d samples\n", path.c_str(), at, len, peak,
                std::sqrt((double)energy / (12.0 * 2048 * 2)),
                secondSamples);

    // Run to the end: the finish flag must come exactly once, at the last
    // sample, and the decoder must never fail on this stream.
    int guard = 0;
    while (!finish && guard++ < 1000) {
        r = c.DecodeData(kOut, &samples, &finish, &remains);
        if (r != 0) break;
    }
    CHECK_EQ(r, 0, "decode to end of stream");
    CHECK_EQ(finish, 1, "finish flag at end");
    CHECK_EQ(c.Info().decodePos, c.Info().endSample + 1,
             "decodePos ends one past endSample");
}


// ---- 7. HLE glue (psp_hle_atrac.cpp): argument order, output-pointer writes,
// return codes. The registry is stubbed so the real handlers are invoked by
// name, as the syscall table would. ----

static std::map<std::string, HleFunc>& registry() {
    static std::map<std::string, HleFunc> r;
    return r;
}
void psp_hle_register(const char* nid_name, HleFunc fn) {
    registry()[nid_name] = fn;
}

static uint8_t* g_hle_ram = nullptr;   // full 128MB rdram (lazily committed)

static int32_t callHle(const char* name, uint32_t a0 = 0, uint32_t a1 = 0,
                       uint32_t a2 = 0, uint32_t a3 = 0, uint32_t t0 = 0) {
    auto it = registry().find(name);
    if (it == registry().end()) {
        std::fprintf(stderr, "FAIL: HLE function %s not registered\n", name);
        failures++;
        return 0x7FFFFFFF;
    }
    recomp_context ctx{};
    ctx.r[2] = 0x12345678;  // sentinel: the handler must overwrite it
    ctx.r[4] = (int32_t)a0; ctx.r[5] = (int32_t)a1; ctx.r[6] = (int32_t)a2;
    ctx.r[7] = (int32_t)a3; ctx.r[8] = (int32_t)t0;
    it->second(g_hle_ram, &ctx);
    return ctx.r[2];
}

static uint32_t hleRd(uint32_t addr) {
    uint32_t v = 0;
    std::memcpy(&v, g_hle_ram + (addr & 0x07FFFFFFu), 4);
    return v;
}

static void test_hle_registration() {
    psp_hle_register_atrac();
    const char* names[] = {
        "sceAtracGetAtracID", "sceAtracReleaseAtracID", "sceAtracSetData",
        "sceAtracReinit", "sceAtracDecodeData", "sceAtracGetNextSample",
        "sceAtracGetStreamDataInfo", "sceAtracAddStreamData",
        "sceAtracSetLoopNum", "sceAtracGetRemainFrame",
        "sceAtracGetSoundSample", "sceAtracResetPlayPosition",
        "sceAtracGetBufferInfoForResetting"};
    for (const char* n : names) {
        CHECK_TRUE(registry().count(n) == 1, n);
    }
}

static void test_hle_ids_and_reinit() {
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1000), 0, "first AT3+ id");
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1000), 1, "second AT3+ id");
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1000), ERR_NO_ATRACID,
             "only two AT3+ slots by default");
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1001), 2, "first AT3 id");
    CHECK_EQ(callHle("sceAtracGetAtracID", 5), ERR_INVALID_CODECTYPE,
             "bad codec type");
    CHECK_EQ(callHle("sceAtracDecodeData", 99, 0x08800000u), ERR_BAD_ATRACID,
             "bad id");
    CHECK_EQ(callHle("sceAtracDecodeData", 0, 0x08800000u), ERR_NO_DATA,
             "no data yet");
    CHECK_EQ(callHle("sceAtracReinit", 1, 1), (int32_t)0x80000021U,
             "reinit with ids in use -> BUSY");
    CHECK_EQ(callHle("sceAtracReleaseAtracID", 1), 0, "release");
    CHECK_EQ(callHle("sceAtracReleaseAtracID", 1), ERR_BAD_ATRACID,
             "double release");
    CHECK_EQ(callHle("sceAtracReleaseAtracID", 0), 0, "release 0");
    CHECK_EQ(callHle("sceAtracReleaseAtracID", 2), 0, "release 2");
    // Reinit(at3, at3plus): AT3+ costs two slots of six.
    CHECK_EQ(callHle("sceAtracReinit", 1, 2), 0, "reinit(1,2) fits");
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1000), 0, "AT3+ slot 0");
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1000), 1, "AT3+ slot 1");
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1000), ERR_NO_ATRACID, "no 3rd");
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1001), 2, "AT3 slot 2");
    callHle("sceAtracReleaseAtracID", 0);
    callHle("sceAtracReleaseAtracID", 1);
    callHle("sceAtracReleaseAtracID", 2);
    CHECK_EQ(callHle("sceAtracReinit", 4, 4), (int32_t)0x80000022U,
             "reinit too big -> OUT_OF_MEMORY (partially applied)");
    CHECK_EQ(callHle("sceAtracReinit", 0, 0), 0, "deinit");
    CHECK_EQ(callHle("sceAtracGetAtracID", 0x1000), ERR_NO_ATRACID,
             "no slots after deinit");
    // Restore the default layout for the following tests.
    CHECK_EQ(callHle("sceAtracReinit", 2, 2), 0, "reinit(2,2)");
}

static void test_hle_real_playback() {
    std::string path = findBgmDat();
    if (path.empty()) {
        std::printf("SKIPPED: HLE real-data test (BGM.DAT not found)\n");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    size_t at = std::string::npos;
    for (size_t i = 0; i + 12 <= dat.size(); i++) {
        if (!std::memcmp(&dat[i], "RIFF", 4) &&
            !std::memcmp(&dat[i + 8], "WAVE", 4)) { at = i; break; }
    }
    if (at == std::string::npos) return;
    uint32_t riff = 0;
    std::memcpy(&riff, &dat[at + 4], 4);
    uint32_t len = (uint32_t)std::min<size_t>(riff + 8, dat.size() - at);
    const uint32_t BUF = 0x08A00000u, OUT = 0x08B00000u, P = 0x08C00000u;
    std::memcpy(g_hle_ram + (BUF & 0x07FFFFFFu), &dat[at], len);

    int id = callHle("sceAtracGetAtracID", 0x1000);
    CHECK_EQ(id, 0, "AT3+ id");
    // An AT3 slot must refuse AT3+ data.
    int id3 = callHle("sceAtracGetAtracID", 0x1001);
    CHECK_EQ(callHle("sceAtracSetData", id3, BUF, len), ERR_WRONG_CODECTYPE,
             "AT3 id + AT3+ data");
    CHECK_EQ(callHle("sceAtracSetData", id, BUF, 40), ERR_SIZE_TOO_SMALL,
             "tiny buffer");
    CHECK_EQ(callHle("sceAtracSetData", id, BUF, len), 0, "SetData");

    CHECK_EQ(callHle("sceAtracGetNextSample", id, P), 0, "GetNextSample");
    CHECK_EQ(hleRd(P), 1680u, "next sample count (first frame is partial)");
    CHECK_EQ(callHle("sceAtracGetMaxSample", id, P), 0, "GetMaxSample");
    CHECK_EQ(hleRd(P), 2048u, "max sample");
    CHECK_EQ(callHle("sceAtracGetChannel", id, P), 0, "GetChannel");
    CHECK_EQ(hleRd(P), 2u, "channels");
    CHECK_EQ(callHle("sceAtracGetBitrate", id, P), 0, "GetBitrate");
    CHECK_EQ(hleRd(P), 48u, "bitrate (PPSSPP formula for 280-byte frames)");
    CHECK_EQ(callHle("sceAtracGetRemainFrame", id, P), 0, "GetRemainFrame");
    CHECK_EQ((int32_t)hleRd(P), -1, "all data on memory -> -1");

    // Misaligned / bogus output pointers.
    CHECK_EQ(callHle("sceAtracDecodeData", id, OUT + 1, P, P + 4, P + 8),
             ERR_BAD_ALIGNMENT, "odd output address");
    CHECK_EQ(callHle("sceAtracDecodeData", id, 0x8000, P, P + 4, P + 8),
             ERR_SIZE_TOO_SMALL, "output in the NULL page");

    std::memset(g_hle_ram + (P & 0x07FFFFFFu), 0xEE, 16);
    CHECK_EQ(callHle("sceAtracDecodeData", id, OUT, P, P + 4, P + 8), 0,
             "DecodeData (5 args incl. t0)");
    CHECK_EQ(hleRd(P), 1680u, "numSamples written to a2");
    CHECK_EQ(hleRd(P + 4), 0u, "finish flag written to a3");
    CHECK_EQ((int32_t)hleRd(P + 8), -1, "remain frames written to t0");
    long long energy = 0;
    const int16_t* pcm = (const int16_t*)(g_hle_ram + (OUT & 0x07FFFFFFu));
    for (int k = 0; k < 1680 * 2; k++) energy += std::abs((int)pcm[k]);
    CHECK_TRUE(energy > 0, "PCM written to the guest buffer");
    // Null pointers for the optional outputs are fine.
    CHECK_EQ(callHle("sceAtracDecodeData", id, OUT, 0, 0, 0), 0,
             "decode with null out-params");

    CHECK_EQ(callHle("sceAtracGetSoundSample", id, P, P + 4, P + 8), 0,
             "GetSoundSample");
    CHECK_EQ((int32_t)hleRd(P), 191999, "end sample (fact 192000 - 1)");
    CHECK_EQ((int32_t)hleRd(P + 4), -1, "no loop start");
    CHECK_EQ((int32_t)hleRd(P + 8), -1, "no loop end");
    CHECK_EQ(callHle("sceAtracSetLoopNum", id, 1),
             ERR_NO_LOOP_INFORMATION, "SetLoopNum without loop info");

    std::memset(g_hle_ram + (P & 0x07FFFFFFu), 0xEE, 16);
    CHECK_EQ(callHle("sceAtracGetStreamDataInfo", id, P, P + 4, P + 8), 0,
             "GetStreamDataInfo");
    CHECK_EQ(hleRd(P), BUF, "writePtr = buffer (all data loaded)");
    CHECK_EQ(hleRd(P + 4), 0u, "writable bytes");
    CHECK_EQ(hleRd(P + 8), 0u, "read offset");
    CHECK_EQ(callHle("sceAtracAddStreamData", id, 0), ERR_ALL_DATA_LOADED,
             "AddStreamData when everything is loaded");

    CHECK_EQ(callHle("sceAtracGetBufferInfoForResetting", id, 0, P), 0,
             "GetBufferInfoForResetting");
    CHECK_EQ(hleRd(P), BUF, "first.writePosPtr");
    CHECK_EQ(hleRd(P + 4), 0u, "first.writableBytes");
    CHECK_EQ(hleRd(P + 8), 0u, "first.minWriteBytes");
    CHECK_EQ(hleRd(P + 12), 0u, "first.filePos");
    CHECK_EQ(hleRd(P + 16), BUF, "second.writePosPtr");
    CHECK_EQ(callHle("sceAtracGetBufferInfoForResetting", id, 0, 0x8000),
             ERR_KERNEL_ILLEGAL_ADDR, "bad info pointer");
    CHECK_EQ(callHle("sceAtracGetBufferInfoForResetting", id, 99999999, P),
             ERR_BAD_SAMPLE, "sample past the end");
    CHECK_EQ(callHle("sceAtracResetPlayPosition", id, 50000, 0, 0), 0,
             "ResetPlayPosition when everything is loaded");
    CHECK_EQ(callHle("sceAtracGetNextSample", id, P), 0, "GetNextSample");
    CHECK_EQ(hleRd(P), 2048u - (50000 + 368) % 2048u,
             "partial first frame at the new position");

    CHECK_EQ(callHle("sceAtracReleaseAtracID", id), 0, "release");
    CHECK_EQ(callHle("sceAtracDecodeData", id, OUT, 0, 0, 0), ERR_BAD_ATRACID,
             "decode on a released id");
    callHle("sceAtracReleaseAtracID", id3);
}

int main() {
    g_hle_ram = (uint8_t*)std::calloc(1, PSP_MEM_SIZE);
    if (!g_hle_ram) { std::fprintf(stderr, "no memory for rdram\n"); return 2; }
    test_parse_at3plus_full();
    test_parse_at3plus_no_loop();
    test_parse_at3_joint();
    test_parse_errors();
    test_out_of_range_addresses();
    test_decode_to_bad_output_address();
    test_streaming_wrap_bookkeeping();
    test_streaming_full_pass_in_order();
    test_streaming_buffer_size_sweep();
    test_end_on_frame_boundary();
    test_streaming_loop_from_end();
    test_loop_playback();
    test_set_loop_num_without_loop();
    test_reset_play_position();
    test_real_atrac3plus();
    test_hle_registration();
    test_hle_ids_and_reinit();
    test_hle_real_playback();

    if (failures) {
        std::fprintf(stderr, "%d of %d checks FAILED\n", failures, tests_run);
        return 1;
    }
    std::printf("All %d atrac ctx checks passed\n", tests_run);
    return 0;
}
