// scePsmf HLE glue tests. Handlers run through a stub registry against a
// fake guest RAM, the same way test_atrac_ctx drives psp_hle_atrac.cpp.
// Oracle: PPSSPP Core/HLE/scePsmf.cpp. The real-data case reads 2048 bytes
// of the opening movie out of DATA_CMN.BND ($PSPRECOMP_TEST_BND, else the
// default disc0 path) and prints "skipped" when that file is unreadable.

#include "hle/psp_hle.h"
#include "hle/psp_psmf.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
static int tests_run = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        tests_run++;                                                           \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);       \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected, msg)                                        \
    do {                                                                       \
        tests_run++;                                                           \
        const long long a_ = static_cast<long long>(actual);                   \
        const long long e_ = static_cast<long long>(expected);                 \
        if (a_ != e_) {                                                        \
            std::fprintf(stderr,                                               \
                         "FAIL: %s: got %lld (0x%llx), expected %lld (0x%llx) " \
                         "(line %d)\n",                                         \
                         msg, a_, (unsigned long long)a_, e_,                  \
                         (unsigned long long)e_, __LINE__);                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static constexpr int32_t ERR_ILLEGAL_ADDRESS = (int32_t)0x8002006AU;
static constexpr int32_t ERR_NOT_INITIALIZED = (int32_t)0x80615001;
static constexpr int32_t ERR_BAD_VERSION = (int32_t)0x80615002;
static constexpr int32_t ERR_NOT_FOUND = (int32_t)0x80615025;
static constexpr int32_t ERR_INVALID_ID = (int32_t)0x80615100;
static constexpr int32_t ERR_INVALID_VALUE = (int32_t)0x806151FE;
static constexpr int32_t ERR_INVALID_PSMF = (int32_t)0x80615501;

static const char* kDefaultBnd =
    "C:/Users/torso/Documents/deco/disc0/PSP_GAME/USRDIR/DATA_CMN.BND";
static constexpr long kMovieHeaderOffset = 0x13F771B;

static std::map<std::string, HleFunc>& registry() {
    static std::map<std::string, HleFunc> r;
    return r;
}

void psp_hle_register(const char* nid_name, HleFunc fn) {
    registry()[nid_name] = fn;
}

static uint8_t* g_ram = nullptr;

static int32_t callHle(const char* name, uint32_t a0 = 0, uint32_t a1 = 0,
                       uint32_t a2 = 0, uint32_t a3 = 0) {
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
    it->second(g_ram, &ctx);
    return ctx.r[2];
}

static uint32_t rd32(uint32_t addr) {
    uint32_t v = 0;
    std::memcpy(&v, g_ram + (addr & 0x07FFFFFFu), 4);
    return v;
}

static void put_be32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = static_cast<uint8_t>(v >> 24);
    b[off + 1] = static_cast<uint8_t>(v >> 16);
    b[off + 2] = static_cast<uint8_t>(v >> 8);
    b[off + 3] = static_cast<uint8_t>(v);
}

static std::vector<uint8_t> make_header() {
    std::vector<uint8_t> h(2048, 0);
    std::memcpy(h.data(), "PSMF0015", 8);
    put_be32(h, 8, 2048);
    put_be32(h, 12, 0x40000);
    h[0x80] = 0;
    h[0x81] = 2;
    h[0x82] = 0xE0;
    h[0x82 + 12] = 30;
    h[0x82 + 13] = 17;
    h[0x82 + 16] = 0xBD;
    h[0x82 + 17] = 0x00;
    h[0x82 + 16 + 14] = 2;
    h[0x82 + 16 + 15] = 2;
    return h;
}

static void plant(uint32_t addr, const uint8_t* data, size_t n) {
    std::memcpy(g_ram + (addr & 0x07FFFFFFu), data, n);
}

static void test_registered() {
    const char* names[] = {
        "scePsmfSetPsmf",
        "scePsmfGetNumberOfStreams",
        "scePsmfGetNumberOfSpecificStreams",
        "scePsmfSpecifyStream",
        "scePsmfGetVideoInfo",
        "scePsmfGetCurrentStreamType",
        "scePsmfGetPsmfVersion",
    };
    for (const char* n : names) {
        CHECK(registry().count(n) == 1, n);
    }
}

static void test_synthetic() {
    const uint32_t st = 0x08810000u;
    const uint32_t data = 0x08820000u;
    const uint32_t info = 0x08830000u;
    const uint32_t typeAddr = 0x08830010u;
    const uint32_t chanAddr = 0x08830014u;
    const uint32_t unknown = 0x08840000u;

    std::vector<uint8_t> h = make_header();
    plant(data, h.data(), h.size());
    std::memset(g_ram + (st & 0x07FFFFFFu), 0xA5, 32);

    CHECK_EQ(callHle("scePsmfSetPsmf", st, data), 0, "SetPsmf");
    CHECK_EQ(rd32(st + 0), PSMF_VERSION_0015, "struct version word");
    CHECK_EQ(rd32(st + 4), 0x800u, "struct headerSize");
    CHECK_EQ(rd32(st + 8), data, "struct headerOffset is the data pointer");
    CHECK_EQ(rd32(st + 12), 0x40000u, "struct streamSize");
    CHECK_EQ(rd32(st + 16), 0u, "struct streamOffset left zero");
    CHECK_EQ(rd32(st + 20), 0u, "struct streamNum starts at 0");

    CHECK_EQ(callHle("scePsmfGetPsmfVersion", st), (int32_t)PSMF_VERSION_0015,
             "GetPsmfVersion is the raw version word");
    CHECK_EQ(callHle("scePsmfGetNumberOfStreams", st), 2, "NumberOfStreams");
    CHECK_EQ(callHle("scePsmfGetNumberOfSpecificStreams", st, PSMF_AVC_STREAM), 1,
             "one AVC stream");
    CHECK_EQ(callHle("scePsmfGetNumberOfSpecificStreams", st, PSMF_ATRAC_STREAM), 1,
             "one ATRAC stream");
    CHECK_EQ(callHle("scePsmfGetNumberOfSpecificStreams", st, PSMF_AUDIO_STREAM), 1,
             "AUDIO matches ATRAC");

    std::memset(g_ram + (info & 0x07FFFFFFu), 0xEE, 8);
    CHECK_EQ(callHle("scePsmfGetVideoInfo", st, info), 0, "GetVideoInfo");
    CHECK_EQ(rd32(info), 480u, "video width");
    CHECK_EQ(rd32(info + 4), 272u, "video height");

    std::memset(g_ram + (typeAddr & 0x07FFFFFFu), 0xEE, 8);
    CHECK_EQ(callHle("scePsmfSpecifyStream", st, 0), 0, "SpecifyStream(0)");
    CHECK_EQ(callHle("scePsmfGetCurrentStreamType", st, typeAddr, chanAddr), 0,
             "GetCurrentStreamType");
    CHECK_EQ(rd32(typeAddr), (uint32_t)PSMF_AVC_STREAM, "current type is AVC");
    CHECK_EQ(rd32(chanAddr), 0u, "current channel is 0");

    CHECK_EQ(callHle("scePsmfGetNumberOfStreams", unknown), ERR_NOT_INITIALIZED,
             "unknown struct NumberOfStreams");
    CHECK_EQ(callHle("scePsmfGetPsmfVersion", unknown), ERR_NOT_FOUND,
             "unknown struct GetPsmfVersion");
    CHECK_EQ(callHle("scePsmfSpecifyStream", st, 5), ERR_INVALID_ID,
             "SpecifyStream out of range");

    CHECK_EQ(callHle("scePsmfSetPsmf", 0, data), ERR_ILLEGAL_ADDRESS,
             "null struct");
    CHECK_EQ(callHle("scePsmfSetPsmf", st, 0), ERR_ILLEGAL_ADDRESS, "null data");

    std::vector<uint8_t> bad = h;
    bad[0] = 'X';
    plant(data, bad.data(), bad.size());
    CHECK_EQ(callHle("scePsmfSetPsmf", unknown, data), ERR_INVALID_PSMF,
             "bad magic");

    bad = h;
    std::memset(&bad[4], 0, 4);
    plant(data, bad.data(), bad.size());
    CHECK_EQ(callHle("scePsmfSetPsmf", unknown, data), ERR_BAD_VERSION,
             "zero version");

    bad = h;
    put_be32(bad, 8, 0);
    plant(data, bad.data(), bad.size());
    CHECK_EQ(callHle("scePsmfSetPsmf", unknown, data), ERR_INVALID_VALUE,
             "zero stream offset");
}

static bool read_file_at(const char* path, long offset, std::vector<uint8_t>* out,
                         size_t len) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    out->assign(len, 0);
    bool ok = std::fseek(f, offset, SEEK_SET) == 0;
    size_t got = ok ? std::fread(out->data(), 1, len, f) : 0;
    std::fclose(f);
    out->resize(got);
    return got == len;
}

static void test_real_header() {
    const char* env = std::getenv("PSPRECOMP_TEST_BND");
    const char* path = (env && *env) ? env : kDefaultBnd;
    std::vector<uint8_t> h;
    if (!read_file_at(path, kMovieHeaderOffset, &h, 2048)) {
        std::printf("skipped (%s not readable)\n", path);
        return;
    }

    const uint32_t st = 0x08910000u;
    const uint32_t data = 0x08920000u;
    const uint32_t info = 0x08930000u;
    const uint32_t typeAddr = 0x08930010u;
    const uint32_t chanAddr = 0x08930014u;
    plant(data, h.data(), h.size());

    CHECK_EQ(callHle("scePsmfSetPsmf", st, data), 0, "real SetPsmf");
    const int32_t version = callHle("scePsmfGetPsmfVersion", st);
    const int32_t streams = callHle("scePsmfGetNumberOfStreams", st);
    const int32_t avc =
        callHle("scePsmfGetNumberOfSpecificStreams", st, PSMF_AVC_STREAM);
    const int32_t audio =
        callHle("scePsmfGetNumberOfSpecificStreams", st, PSMF_AUDIO_STREAM);
    const int32_t atrac =
        callHle("scePsmfGetNumberOfSpecificStreams", st, PSMF_ATRAC_STREAM);
    CHECK_EQ(version, (int32_t)PSMF_VERSION_0015, "real version word");
    CHECK_EQ(streams, 2, "real NumberOfStreams");
    CHECK_EQ(avc, 1, "real AVC count");
    CHECK_EQ(atrac, 1, "real ATRAC count");
    CHECK_EQ(audio, 1, "real audio count");

    CHECK_EQ(callHle("scePsmfGetVideoInfo", st, info), 0, "real GetVideoInfo");
    CHECK_EQ(rd32(info), 480u, "real width");
    CHECK_EQ(rd32(info + 4), 272u, "real height");

    CHECK_EQ(callHle("scePsmfSpecifyStream", st, 0), 0, "real SpecifyStream(0)");
    CHECK_EQ(callHle("scePsmfGetCurrentStreamType", st, typeAddr, chanAddr), 0,
             "real GetCurrentStreamType");
    CHECK_EQ(rd32(typeAddr), (uint32_t)PSMF_AVC_STREAM, "real current type");
    CHECK_EQ(rd32(chanAddr), 0u, "real current channel");

    std::printf("real header: version=0x%08X streams=%d avc=%d atrac=%d audio=%d "
                "video=%ux%u\n",
                (unsigned)version, streams, avc, atrac, audio, rd32(info),
                rd32(info + 4));
}

int main() {
    g_ram = static_cast<uint8_t*>(std::calloc(1, PSP_MEM_SIZE));
    if (!g_ram) {
        std::fprintf(stderr, "no memory for rdram\n");
        return 2;
    }
    psp_hle_register_psmf();
    test_registered();
    test_synthetic();
    test_real_header();
    std::free(g_ram);
    std::printf("%d checks, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
