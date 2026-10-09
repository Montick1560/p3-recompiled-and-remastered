// sceMpeg HLE test (psp_hle_mpeg.cpp): the real handlers are invoked by name
// through a stubbed registry, on a fake 128 MB guest RAM, with a host function
// playing the game's ringbuffer read callback. The real-data case plays the
// opening movie of the user's DATA_CMN.BND the way the game's movie thread
// does (Put / GetAvcAu / DecodeYCbCr / Csc / GetAtracAu / AtracDecode) and is
// skipped when the file is absent.

#include "hle/psp_hle.h"
#include "hle/psp_hle_mpeg.h"
#include "hle/psp_media_engine.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);         \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                      \
    do {                                                                         \
        checks++;                                                                \
        long long _a = (long long)(a), _b = (long long)(b);                      \
        if (_a != _b) {                                                          \
            std::fprintf(stderr, "FAIL: %s: got %lld (0x%llX), want %lld (0x%llX) (line %d)\n", \
                         msg, _a, (unsigned long long)_a, _b, (unsigned long long)_b, __LINE__); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

// ---- link stubs for the runtime pieces the handlers touch ----

static std::map<std::string, HleFunc>& registry() {
    static std::map<std::string, HleFunc> r;
    return r;
}
void psp_hle_register(const char* nid_name, HleFunc fn) { registry()[nid_name] = fn; }
void sched_yield_point() {}
FuncPtr RECOMP_LOOKUP(uint32_t) { return nullptr; }

static uint8_t* g_ram = nullptr;

static int32_t callHle(const char* name, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0,
                       uint32_t a3 = 0, uint32_t t0 = 0, uint32_t t1 = 0) {
    auto it = registry().find(name);
    if (it == registry().end()) {
        std::fprintf(stderr, "FAIL: HLE function %s not registered\n", name);
        failures++;
        return 0x7FFFFFFF;
    }
    recomp_context ctx{};
    ctx.r[2] = 0x12345678;
    ctx.r[4] = (int32_t)a0;
    ctx.r[5] = (int32_t)a1;
    ctx.r[6] = (int32_t)a2;
    ctx.r[7] = (int32_t)a3;
    ctx.r[8] = (int32_t)t0;
    ctx.r[9] = (int32_t)t1;
    ctx.r[29] = 0x09F00000;
    it->second(g_ram, &ctx);
    return ctx.r[2];
}

static uint32_t rd(uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, g_ram + (addr & 0x07FFFFFF), 4);
    return v;
}
static void wr(uint32_t addr, uint32_t v) { std::memcpy(g_ram + (addr & 0x07FFFFFF), &v, 4); }
static int64_t rd_pts(uint32_t addr) {  // SceMpegAu pts: high word first
    return (int64_t)((uint64_t)rd(addr) << 32 | rd(addr + 4));
}


constexpr int32_t ERR_NO_DATA = (int32_t)0x80618001U;
constexpr int32_t ERR_INVALID_VALUE = (int32_t)0x806101FEU;

// Guest layout used by the tests.
constexpr uint32_t MPEG = 0x08800000;      // the game's mpeg pointer cell
constexpr uint32_t MPEG_DATA = 0x08810000; // sceMpegCreate work area (64 KB)
constexpr uint32_t RING = 0x08830000;
constexpr uint32_t AU_V = 0x08830100;
constexpr uint32_t AU_A = 0x08830140;
constexpr uint32_t ATTR = 0x08830180;
constexpr uint32_t BUFP = 0x08830190;     // holds a pointer to the YCbCr buffer
constexpr uint32_t INITP = 0x088301A0;
constexpr uint32_t RANGE = 0x088301B0;
constexpr uint32_t OUTSZ = 0x088301D0;
constexpr uint32_t HDR = 0x08840000;      // PSMF header (2048 bytes)
constexpr uint32_t YCBCR = 0x08850000;
constexpr uint32_t PCM = 0x088A0000;      // 8192 bytes
constexpr uint32_t FRAME = 0x088B0000;    // 512 x 272 x 4
constexpr uint32_t RING_DATA = 0x08A00000;
constexpr uint32_t CALLBACK = 0x08900100;
constexpr int PACKETS = 0x3C0;

// The "game" callback: copies the next packets of the movie into the ring.
static std::vector<uint8_t> g_stream;
static size_t g_stream_pos = 0;
static int g_callback_calls = 0;

static int32_t fake_guest_call(uint8_t* rdram, recomp_context*, uint32_t fn, uint32_t a0,
                               uint32_t a1, uint32_t a2) {
    g_callback_calls++;
    if (fn != CALLBACK || a2 != 0x1234) {
        std::fprintf(stderr, "FAIL: callback fn=0x%08X arg=0x%X\n", fn, a2);
        failures++;
        return -1;
    }
    size_t want = (size_t)a1 * 2048;
    size_t n = std::min(want, g_stream.size() - g_stream_pos);
    n -= n % 2048;
    std::memcpy(rdram + (a0 & 0x07FFFFFF), g_stream.data() + g_stream_pos, n);
    g_stream_pos += n;
    return (int32_t)(n / 2048);
}

static bool read_file_at(const char* path, uint64_t off, std::vector<uint8_t>* out, size_t n) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg((std::streamoff)off);
    out->resize(n);
    f.read(reinterpret_cast<char*>(out->data()), (std::streamsize)n);
    return (size_t)f.gcount() == n;
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void setup_mpeg() {
    psp_hle_mpeg_reset_for_tests();
    CHECK_EQ(callHle("sceMpegInit"), 0, "init");
    CHECK_EQ(callHle("sceMpegRingbufferQueryMemSize", PACKETS), PACKETS * 2152,
             "ring mem size = packets * (104 + 2048)");
    CHECK_EQ(callHle("sceMpegQueryMemSize", 0), 0x10000, "mpeg mem size");
    CHECK_EQ(callHle("sceMpegRingbufferConstruct", RING, PACKETS, RING_DATA, PACKETS * 2152,
                     CALLBACK, 0x1234),
             0, "construct");
    CHECK_EQ(rd(RING + 0), PACKETS, "ring packets");
    CHECK_EQ(rd(RING + 16), 2048, "ring packet size");
    CHECK_EQ(rd(RING + 24), CALLBACK, "ring callback");
    CHECK_EQ(callHle("sceMpegCreate", MPEG, MPEG_DATA, 0x10000, RING, 512, 0, 0), 0, "create");
    CHECK_EQ(rd(MPEG), MPEG_DATA + 0x30, "handle written to the mpeg cell");
    CHECK_EQ(rd(RING + 40), MPEG, "ring->mpeg");
    CHECK(std::memcmp(g_ram + ((MPEG_DATA + 0x30) & 0x07FFFFFF), "LIBMPEG", 7) == 0,
          "LIBMPEG tag");
}

static void test_registration() {
    psp_hle_register_mpeg();
    const char* names[] = {"sceMpegInit", "sceMpegCreate", "sceMpegRingbufferPut",
                           "sceMpegGetAvcAu", "sceMpegAvcDecodeYCbCr", "sceMpegAvcCsc",
                           "sceMpegAtracDecode", "sceMpegRingbufferAvailableSize"};
    for (const char* n : names) {
        CHECK(registry().count(n) == 1, n);
    }
}

static void test_errors() {
    setup_mpeg();
    CHECK_EQ(callHle("sceMpegGetAvcAu", 0x08870000, 1, AU_V, ATTR), -1, "unknown mpeg -> -1");
    std::memcpy(g_ram + (HDR & 0x07FFFFFF), "XXXX", 4);
    wr(OUTSZ, 0xDEAD);
    CHECK_EQ(callHle("sceMpegQueryStreamOffset", MPEG, HDR, OUTSZ), ERR_INVALID_VALUE,
             "bad magic");
    CHECK_EQ(rd(OUTSZ), 0, "bad magic writes 0");
    CHECK_EQ(callHle("sceMpegRingbufferAvailableSize", RING), PACKETS, "empty ring is all free");
    int sid = callHle("sceMpegRegistStream", MPEG, 0, 0);
    CHECK_EQ(callHle("sceMpegGetAvcAu", MPEG, sid, AU_V, ATTR), ERR_NO_DATA, "no data yet");
    CHECK_EQ(callHle("sceMpegAvcQueryYCbCrSize", MPEG, 1, 480, 272, OUTSZ), 0, "ycbcr size");
    CHECK_EQ(rd(OUTSZ), 240 * 136 * 6 + 128, "ycbcr size value");
    CHECK_EQ(callHle("sceMpegAvcQueryYCbCrSize", MPEG, 1, 481, 272, OUTSZ), ERR_INVALID_VALUE,
             "ycbcr bad width");
    CHECK_EQ(callHle("sceMpegMallocAvcEsBuf", MPEG), 1, "es buf 1");
    CHECK_EQ(callHle("sceMpegMallocAvcEsBuf", MPEG), 2, "es buf 2");
    CHECK_EQ(callHle("sceMpegMallocAvcEsBuf", MPEG), 0, "no es buf left");
    CHECK_EQ(callHle("sceMpegFreeAvcEsBuf", MPEG, 0), ERR_INVALID_VALUE, "free es buf 0");
    CHECK_EQ(callHle("sceMpegDelete", MPEG), 0, "delete");
    CHECK_EQ(callHle("sceMpegDelete", MPEG), -1, "double delete");
}

static void test_real_movie() {
    const char* env = std::getenv("PSPRECOMP_TEST_BND");
    const char* path =
        (env && *env) ? env : "C:/Users/torso/Documents/deco/disc0/PSP_GAME/USRDIR/DATA_CMN.BND";
    const uint64_t kHeader = 0x13F771B;
    std::vector<uint8_t> hdr;
    if (!read_file_at(path, kHeader, &hdr, 2048)) {
        std::printf("test_real_movie: skipped (%s not readable)\n", path);
        return;
    }
    const uint32_t offset = be32(&hdr[8]);
    const uint32_t size = be32(&hdr[12]);
    CHECK(read_file_at(path, kHeader + offset, &g_stream, size), "stream readable");
    g_stream_pos = 0;
    g_callback_calls = 0;

    setup_mpeg();
    std::memcpy(g_ram + (HDR & 0x07FFFFFF), hdr.data(), hdr.size());
    CHECK_EQ(callHle("sceMpegQueryStreamOffset", MPEG, HDR, OUTSZ), 0, "stream offset");
    CHECK_EQ(rd(OUTSZ), offset, "offset value");
    CHECK_EQ(callHle("sceMpegQueryStreamSize", HDR, OUTSZ), 0, "stream size");
    CHECK_EQ(rd(OUTSZ), size, "size value");

    const int vsid = callHle("sceMpegRegistStream", MPEG, 0, 0);
    const int asid = callHle("sceMpegRegistStream", MPEG, 1, 0);
    CHECK(vsid > 0 && asid > 0 && vsid != asid, "stream ids");
    CHECK_EQ(callHle("sceMpegInitAu", MPEG, callHle("sceMpegMallocAvcEsBuf", MPEG), AU_V), 0,
             "init video au");
    CHECK_EQ(callHle("sceMpegInitAu", MPEG, 0, AU_A), 0, "init audio au");
    CHECK_EQ(callHle("sceMpegQueryAtracEsSize", MPEG, OUTSZ, OUTSZ + 4), 0, "atrac es size");
    CHECK_EQ(rd(OUTSZ + 4), 8192, "atrac out size");
    wr(BUFP, YCBCR);
    wr(RANGE, 0);
    wr(RANGE + 4, 0);
    wr(RANGE + 8, 480);
    wr(RANGE + 12, 272);

    int pictures = 0, decodes = 0, audioFrames = 0, nonSilent = 0;
    bool ended = false;
    int noDataAfterEof = 0;
    int64_t lastPts = 0;
    bool ptsMonotonic = true;
    for (int iter = 0; iter < 20000 && !ended; iter++) {
        int freePackets = callHle("sceMpegRingbufferAvailableSize", RING);
        if (freePackets > 0) {
            callHle("sceMpegRingbufferPut", RING, freePackets, freePackets);
        }
        int r = callHle("sceMpegGetAvcAu", MPEG, vsid, AU_V, ATTR);
        if (r == ERR_NO_DATA) {
            // Like the game: the whole file was read and no AU comes out.
            noDataAfterEof = g_stream_pos >= g_stream.size() ? noDataAfterEof + 1 : 0;
            ended = noDataAfterEof >= 3;
            continue;
        }
        CHECK_EQ(r, 0, "get avc au");
        r = callHle("sceMpegAvcDecodeYCbCr", MPEG, AU_V, BUFP, INITP);
        if (r != 0) {
            continue;
        }
        decodes++;
        if (rd(INITP) == 1) {
            pictures++;
            CHECK_EQ(callHle("sceMpegAvcCsc", MPEG, YCBCR, RANGE, 512, FRAME), 0, "csc");
            int64_t pts = rd_pts(AU_V);
            ptsMonotonic = ptsMonotonic && pts > lastPts;
            lastPts = pts;
        }
        if (callHle("sceMpegGetAtracAu", MPEG, asid, AU_A, ATTR) == 0) {
            CHECK_EQ(callHle("sceMpegAtracDecode", MPEG, AU_A, PCM, 1), 0, "atrac decode");
            audioFrames++;
            const int16_t* s = reinterpret_cast<const int16_t*>(g_ram + (PCM & 0x07FFFFFF));
            for (int i = 0; i < 4096; i++) {
                if (s[i] != 0) {
                    nonSilent++;
                    break;
                }
            }
        }
    }
    std::printf("test_real_movie: %d decodes, %d pictures, %d audio frames (%d non-silent), "
                "%d callback calls, fed %zu/%zu, ended=%d, last pts=%lld, ffmpeg=%d\n",
                decodes, pictures, audioFrames, nonSilent, g_callback_calls, g_stream_pos,
                g_stream.size(), ended ? 1 : 0, (long long)lastPts,
                PspVideoDecoder::available() ? 1 : 0);
    CHECK_EQ(g_stream_pos, g_stream.size(), "the whole stream was fed");
    CHECK(ended, "end of movie reported (GetAvcAu NO_DATA)");
    CHECK(pictures >= 850, "about 862 pictures");
    CHECK(ptsMonotonic, "pts increases");
    CHECK(audioFrames >= 500, "audio frames decoded");
    CHECK_EQ(callHle("sceMpegAvcDecodeYCbCr", MPEG, AU_V, BUFP, INITP), (int32_t)0x80628002U,
             "decode after the end is fatal");
    if (PspVideoDecoder::available()) {
        CHECK(nonSilent > audioFrames / 2, "movie audio is not silent");
        // The last picture is in FRAME as 8888 rows 512 px apart.
        const uint8_t* f = g_ram + (FRAME & 0x07FFFFFF);
        bool varied = false;
        for (int i = 4; i < 480 * 4 * 100 && !varied; i += 4) {
            varied = std::memcmp(f, f + i, 3) != 0;
        }
        CHECK(varied, "picture is not a single colour");
    }
    callHle("sceMpegFlushAllStream", MPEG);
    CHECK_EQ(rd(RING + 4), 0, "flush resets packetsRead");
    CHECK_EQ(callHle("sceMpegDelete", MPEG), 0, "delete");
}

int main() {
    g_ram = static_cast<uint8_t*>(std::calloc(PSP_MEM_SIZE, 1));
    if (!g_ram) {
        std::fprintf(stderr, "cannot allocate guest RAM\n");
        return 2;
    }
    psp_hle_mpeg_set_guest_call(fake_guest_call);
    test_registration();
    test_errors();
    test_real_movie();
    std::printf("%d checks, %d failures\n", checks, failures);
    std::free(g_ram);
    return failures == 0 ? 0 : 1;
}
