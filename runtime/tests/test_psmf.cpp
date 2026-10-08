// Unit tests for the PSMF header parser (hle/psp_psmf.h). Oracle: PPSSPP
// Core/HLE/scePsmf.cpp (Psmf / PsmfStream) and sceMpeg.cpp (AnalyzeMpeg).
//
// Standalone executable: no SDL/GL/scheduler. The real-data test reads the
// opening movie's PSMF header out of the user's DATA_CMN.BND at test time
// (nothing is committed) and passes with a "skipped" note when the file is
// absent. Path: $PSPRECOMP_TEST_BND, else the default disc0 location.

#include "hle/psp_psmf.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static int tests_run = 0;

#define CHECK(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            failures++; \
        } \
    } while (0)

#define CHECK_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        const long long a_ = static_cast<long long>(actual); \
        const long long e_ = static_cast<long long>(expected); \
        if (a_ != e_) { \
            std::fprintf(stderr, "FAIL: %s: got %lld, expected %lld (line %d)\n", \
                msg, a_, e_, __LINE__); \
            failures++; \
        } \
    } while (0)

static const char* kDefaultBnd =
    "C:/Users/torso/Documents/deco/disc0/PSP_GAME/USRDIR/DATA_CMN.BND";
static constexpr long kMovieHeaderOffset = 0x13F771B;  // "PSMF0015"

static void put_be32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = static_cast<uint8_t>(v >> 24);
    b[off + 1] = static_cast<uint8_t>(v >> 16);
    b[off + 2] = static_cast<uint8_t>(v >> 8);
    b[off + 3] = static_cast<uint8_t>(v);
}

static void put_ts(std::vector<uint8_t>& b, size_t off, uint64_t ts) {
    for (int i = 0; i < 6; i++) {
        b[off + i] = static_cast<uint8_t>(ts >> (8 * (5 - i)));
    }
}

/// A well-formed 2048-byte PSMF 0015 header: one AVC (480x272) + one ATRAC
/// stream, 10-entry EP map.
static std::vector<uint8_t> make_header() {
    std::vector<uint8_t> h(2048, 0);
    std::memcpy(h.data(), "PSMF0015", 8);
    put_be32(h, 8, 2048);      // stream offset
    put_be32(h, 12, 0x40000);  // stream size
    put_ts(h, 0x54, 90000);
    put_ts(h, 0x5A, 900000);
    h[0x80] = 0;
    h[0x81] = 2;  // two streams
    uint8_t* v = &h[0x82];
    v[0] = 0xE0;  // video
    v[1] = 0x00;
    put_be32(h, 0x82 + 4, 0x100);  // EP map offset
    put_be32(h, 0x82 + 8, 10);     // EP entries
    v[12] = 30;                    // 480
    v[13] = 17;                    // 272
    uint8_t* a = &h[0x82 + 16];
    a[0] = 0xBD;  // audio
    a[1] = 0x00;  // private stream id: ATRAC, channel 0
    a[14] = 2;
    a[15] = 2;
    for (int i = 0; i < 10; i++) {
        uint8_t* e = &h[0x100 + i * 10];
        e[0] = static_cast<uint8_t>(i);
        e[1] = 1;
        put_be32(h, 0x100 + i * 10 + 2, 90000 + i * 30030);
        put_be32(h, 0x100 + i * 10 + 6, i * 0x1000);
    }
    return h;
}

static void test_synthetic_valid() {
    std::vector<uint8_t> h = make_header();
    PsmfHeader p;
    CHECK(psmf_parse(h.data(), h.size(), &p) == PsmfStatus::Ok, "valid header parses");
    CHECK_EQ(p.magic, PSMF_MAGIC, "magic");
    CHECK_EQ(p.version, PSMF_VERSION_0015, "version word");
    CHECK_EQ(psmf_mpeg_version_index(p.version), 3, "0015 is mpeg version index 3");
    CHECK_EQ(p.streamOffset, 2048, "stream offset");
    CHECK_EQ(p.streamSize, 0x40000, "stream size");
    CHECK_EQ(p.firstTimestamp, 90000, "first timestamp");
    CHECK_EQ(p.lastTimestamp, 900000, "last timestamp");
    CHECK_EQ(p.numStreams, 2, "stored stream count");
    CHECK_EQ(p.streams.size(), 2, "parsed streams");
    CHECK_EQ(p.countStreams(PSMF_AVC_STREAM), 1, "one AVC stream");
    CHECK_EQ(p.countStreams(PSMF_ATRAC_STREAM), 1, "one ATRAC stream");
    CHECK_EQ(p.countStreams(PSMF_AUDIO_STREAM), 1, "AUDIO matches ATRAC");
    CHECK_EQ(p.countStreams(PSMF_PCM_STREAM), 0, "no PCM stream");
    CHECK_EQ(p.videoWidth, 480, "video width");
    CHECK_EQ(p.videoHeight, 272, "video height");
    CHECK_EQ(p.streams[0].videoWidth, 480, "stream 0 width");
    CHECK_EQ(p.streams[0].channel, 0, "video channel");
    CHECK_EQ(p.streams[1].audioChannels, 2, "audio channels");
    CHECK_EQ(p.streams[1].audioFrequency, 2, "audio frequency code");
    CHECK_EQ(p.streams[1].videoWidth, -1, "audio stream is not video");
    CHECK_EQ(p.epMap.size(), 10, "EP map entries");
    CHECK_EQ(p.epMap[3].pts, 90000 + 3 * 30030, "EP pts");
    CHECK_EQ(p.epMap[3].offset, 0x3000, "EP offset");
}

static void test_pcm_private_stream_is_pcm() {
    std::vector<uint8_t> h = make_header();
    h[0x82 + 16 + 1] = 0x10;  // private id with a high nibble => PCM
    PsmfHeader p;
    CHECK(psmf_parse(h.data(), h.size(), &p) == PsmfStatus::Ok, "parses");
    CHECK_EQ(p.countStreams(PSMF_PCM_STREAM), 1, "PCM stream counted");
    CHECK_EQ(p.countStreams(PSMF_ATRAC_STREAM), 0, "not ATRAC");
    CHECK_EQ(p.countStreams(PSMF_AUDIO_STREAM), 1, "AUDIO matches PCM");
}

static void test_error_statuses() {
    PsmfHeader p;
    std::vector<uint8_t> h = make_header();

    CHECK(psmf_parse(nullptr, 0, &p) == PsmfStatus::TooShort, "null/empty is too short");
    CHECK(psmf_parse(h.data(), 8, &p) == PsmfStatus::TooShort, "8 bytes is too short");

    std::vector<uint8_t> bad = h;
    bad[0] = 'X';
    CHECK(psmf_parse(bad.data(), bad.size(), &p) == PsmfStatus::BadMagic, "bad magic");

    bad = h;
    std::memset(&bad[4], 0, 4);
    CHECK(psmf_parse(bad.data(), bad.size(), &p) == PsmfStatus::BadVersion, "zero version");

    bad = h;
    put_be32(bad, 8, 0);
    CHECK(psmf_parse(bad.data(), bad.size(), &p) == PsmfStatus::BadStreamOffset, "zero stream offset");
    CHECK_EQ(p.streams.size(), 0, "no streams parsed on error");

    bad = h;
    bad[4] = '9';
    PsmfHeader q;
    CHECK(psmf_parse(bad.data(), bad.size(), &q) == PsmfStatus::Ok, "unknown non-zero version still parses");
    CHECK_EQ(psmf_mpeg_version_index(q.version), -1, "unknown version has no mpeg index");
}

static void test_truncated_and_hostile_tables() {
    PsmfHeader p;
    std::vector<uint8_t> h = make_header();

    // Header cut mid stream table: only complete entries are used.
    CHECK(psmf_parse(h.data(), 0x82 + 16 + 5, &p) == PsmfStatus::Ok, "cut mid-table parses");
    CHECK_EQ(p.numStreams, 2, "stored count is preserved");
    CHECK_EQ(p.streams.size(), 1, "only the complete entry is parsed");

    // Fixed fields only (no stream count): no streams, no crash.
    CHECK(psmf_parse(h.data(), 0x20, &p) == PsmfStatus::Ok, "fixed fields only");
    CHECK_EQ(p.streams.size(), 0, "no streams");
    CHECK_EQ(p.firstTimestamp, 0, "missing timestamps read as zero");

    // Stream count of 65535 in a 2 KiB header.
    std::vector<uint8_t> big = h;
    big[0x80] = 0xFF;
    big[0x81] = 0xFF;
    CHECK(psmf_parse(big.data(), big.size(), &p) == PsmfStatus::Ok, "huge stream count parses");
    CHECK_EQ(p.numStreams, 65535, "stored count is preserved");
    CHECK(p.streams.size() <= (2048 - 0x82) / 16, "streams clipped to the buffer");

    // EP map claiming 4 billion entries / pointing past the buffer.
    std::vector<uint8_t> ep = h;
    put_be32(ep, 0x82 + 4, 0xFFFFFFF0U);
    put_be32(ep, 0x82 + 8, 0xFFFFFFFFU);
    CHECK(psmf_parse(ep.data(), ep.size(), &p) == PsmfStatus::Ok, "hostile EP map parses");
    CHECK_EQ(p.epMap.size(), 0, "EP map outside the buffer is dropped");

    ep = h;
    put_be32(ep, 0x82 + 4, 0x7F0);
    put_be32(ep, 0x82 + 8, 0x1000000);
    CHECK(psmf_parse(ep.data(), ep.size(), &p) == PsmfStatus::Ok, "EP map overrunning the end parses");
    CHECK_EQ(p.epMap.size(), (2048 - 0x7F0) / PSMF_EP_ENTRY_SIZE, "EP map clipped to the buffer");
}

static void test_random_garbage_never_crashes() {
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    auto rnd = [&s]() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    };
    int statuses[5] = {};
    for (int iter = 0; iter < 20000; iter++) {
        size_t n = static_cast<size_t>(rnd() % 600);
        std::vector<uint8_t> buf(n);
        for (auto& b : buf) {
            b = static_cast<uint8_t>(rnd());
        }
        if (iter % 2 == 0 && n >= 16) {  // reach the table parsing paths
            std::memcpy(buf.data(), "PSMF0015", 8);
            if (buf[8] == 0 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0) {
                buf[11] = 8;
            }
        }
        PsmfHeader p;
        PsmfStatus st = psmf_parse(buf.data(), buf.size(), &p);
        statuses[static_cast<int>(st)]++;
        if (p.streams.size() > n / 16 || p.epMap.size() > n / 10) {
            CHECK(false, "parsed tables must fit in the input");
            break;
        }
    }
    CHECK(statuses[static_cast<int>(PsmfStatus::Ok)] > 0, "fuzz reached the Ok path");
    CHECK(statuses[static_cast<int>(PsmfStatus::BadMagic)] > 0, "fuzz reached BadMagic");
    tests_run++;
}

static bool read_file_at(const char* path, long offset, std::vector<uint8_t>* out, size_t len) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    out->assign(len, 0);
    bool ok = std::fseek(f, offset, SEEK_SET) == 0;
    size_t got = ok ? std::fread(out->data(), 1, len, f) : 0;
    std::fclose(f);
    out->resize(got);
    return got == len;
}

static void test_real_movie_header() {
    const char* env = std::getenv("PSPRECOMP_TEST_BND");
    const char* path = (env && *env) ? env : kDefaultBnd;
    std::vector<uint8_t> h;
    if (!read_file_at(path, kMovieHeaderOffset, &h, 2048)) {
        std::printf("test_real_movie_header: skipped (%s not readable)\n", path);
        return;
    }
    PsmfHeader p;
    CHECK(psmf_parse(h.data(), h.size(), &p) == PsmfStatus::Ok, "real header parses");
    CHECK_EQ(p.version, PSMF_VERSION_0015, "real version is 0015");
    CHECK_EQ(p.streamOffset, 2048, "real stream offset");
    CHECK_EQ(p.streamSize, 4143104, "real stream size");
    CHECK_EQ(p.firstTimestamp, 90000, "real first timestamp");
    CHECK_EQ(p.lastTimestamp, 2678586, "real last timestamp");
    CHECK_EQ(p.numStreams, 2, "real stream count");
    CHECK_EQ(p.countStreams(PSMF_AVC_STREAM), 1, "real AVC stream present");
    CHECK_EQ(p.countStreams(PSMF_ATRAC_STREAM), 1, "real ATRAC stream present");
    CHECK_EQ(p.videoWidth, 480, "real video width");
    CHECK_EQ(p.videoHeight, 272, "real video height");
    CHECK_EQ(p.streams[0].streamId, 0xE0, "real video PES id");
    CHECK_EQ(p.streams[1].streamId, 0xBD, "real audio PES id");
    CHECK_EQ(p.streams[1].audioChannels, 2, "real audio is stereo");
    std::printf("test_real_movie_header: ran against %s (%dx%d, stream 0x%X+0x%X)\n",
        path, p.videoWidth, p.videoHeight, p.streamOffset, p.streamSize);
}

int main() {
    test_synthetic_valid();
    test_pcm_private_stream_is_pcm();
    test_error_statuses();
    test_truncated_and_hostile_tables();
    test_random_garbage_never_crashes();
    test_real_movie_header();
    std::printf("%d checks, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
