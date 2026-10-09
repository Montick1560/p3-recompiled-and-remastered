#include "hle/psp_media_engine.h"
#include "hle/psp_mpeg_demux.h"
#include "hle/psp_psmf.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

using Bytes = std::vector<uint8_t>;

static void test_convert_line() {
    CHECK_EQ(psp_video_bytes_per_pixel(PSP_VIDEO_BGR5650), 2, "5650 bpp");
    CHECK_EQ(psp_video_bytes_per_pixel(PSP_VIDEO_ABGR5551), 2, "5551 bpp");
    CHECK_EQ(psp_video_bytes_per_pixel(PSP_VIDEO_ABGR4444), 2, "4444 bpp");
    CHECK_EQ(psp_video_bytes_per_pixel(PSP_VIDEO_ABGR8888), 4, "8888 bpp");
    CHECK_EQ(psp_video_bytes_per_pixel(4), 0, "unknown bpp");
    CHECK_EQ(psp_video_bytes_per_pixel(-1), 0, "negative bpp");

    const uint8_t rgba[8] = {0xFF, 0x80, 0x01, 0x7F, 0x00, 0xFF, 0x80, 0xFF};
    uint8_t dst[16];

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 1, PSP_VIDEO_ABGR8888);
    CHECK(dst[0] == 0xFF && dst[1] == 0x80 && dst[2] == 0x01 && dst[3] == 0x00, "8888 bytes");
    CHECK(dst[4] == 0xAB, "8888 does not write past width");

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 1, PSP_VIDEO_BGR5650);
    CHECK(dst[0] == 0x1F && dst[1] == 0x04, "5650 = 0x041F");
    CHECK(dst[2] == 0xAB, "5650 does not write past width");

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 1, PSP_VIDEO_ABGR5551);
    CHECK(dst[0] == 0x1F && dst[1] == 0x02, "5551 = 0x021F");

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 1, PSP_VIDEO_ABGR4444);
    CHECK(dst[0] == 0x8F && dst[1] == 0x00, "4444 = 0x008F");

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 2, PSP_VIDEO_ABGR8888);
    CHECK(dst[4] == 0x00 && dst[5] == 0xFF && dst[6] == 0x80 && dst[7] == 0x00, "8888 second pixel");

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 2, PSP_VIDEO_BGR5650);
    CHECK(dst[2] == 0xE0 && dst[3] == 0x87, "5650 second pixel 0x87E0");

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 2, PSP_VIDEO_ABGR5551);
    CHECK(dst[2] == 0xE0 && dst[3] == 0x43, "5551 second pixel 0x43E0");

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 2, PSP_VIDEO_ABGR4444);
    CHECK(dst[2] == 0xF0 && dst[3] == 0x08, "4444 second pixel 0x08F0");

    std::memset(dst, 0xAB, sizeof dst);
    psp_video_convert_line(dst, rgba, 1, 99);
    CHECK(dst[0] == 0xAB && dst[1] == 0xAB && dst[2] == 0xAB && dst[3] == 0xAB, "unknown mode writes nothing");
    psp_video_convert_line(dst, rgba, 1, -1);
    CHECK(dst[0] == 0xAB, "negative mode writes nothing");
    psp_video_convert_line(dst, rgba, 0, PSP_VIDEO_ABGR8888);
    CHECK(dst[0] == 0xAB, "zero width writes nothing");
}

static void test_write_without_picture() {
    PspVideoDecoder v;
    CHECK(!v.hasPicture(), "fresh decoder has no picture");
    CHECK_EQ(v.width(), 0, "width before a picture");
    CHECK_EQ(v.height(), 0, "height before a picture");
    uint8_t dst[64];
    std::memset(dst, 0xCD, sizeof dst);
    CHECK_EQ(v.writeImage(dst, sizeof dst, 16, PSP_VIDEO_ABGR8888, 0, 0, 1, 1), 0, "no picture");
    CHECK(dst[0] == 0xCD, "no picture does not write");
    CHECK_EQ(v.writeImage(nullptr, 64, 16, PSP_VIDEO_ABGR8888, 0, 0, 1, 1), 0, "null dst");
    CHECK_EQ(v.writeImage(dst, sizeof dst, 0, PSP_VIDEO_ABGR8888, 0, 0, 1, 1), 0, "frameWidth 0");
    CHECK_EQ(v.writeImage(dst, sizeof dst, -1, PSP_VIDEO_ABGR8888, 0, 0, 1, 1), 0, "frameWidth negative");
    CHECK_EQ(v.writeImage(dst, sizeof dst, 2049, PSP_VIDEO_ABGR8888, 0, 0, 1, 1), 0, "frameWidth > 2048");
    CHECK_EQ(v.writeImage(dst, sizeof dst, 16, 99, 0, 0, 1, 1), 0, "unknown pixel mode");
    CHECK_EQ(v.writeImage(dst, sizeof dst, 16, PSP_VIDEO_ABGR8888, -1, 0, 1, 1), 0, "negative xpos");
    CHECK_EQ(v.writeImage(dst, sizeof dst, 16, PSP_VIDEO_ABGR8888, 0, -1, 1, 1), 0, "negative ypos");
    CHECK_EQ(v.writeImage(dst, sizeof dst, 16, PSP_VIDEO_ABGR8888, 0, 0, -1, 1), 0, "negative w");
    CHECK_EQ(v.writeImage(dst, sizeof dst, 16, PSP_VIDEO_ABGR8888, 0, 0, 1, -1), 0, "negative h");

    uint8_t junk[32];
    std::memset(junk, 0xA5, sizeof junk);
    CHECK(!v.decode(nullptr, 0), "null access unit");
    CHECK(!v.decode(junk, sizeof junk), "garbage access unit");
    CHECK(!v.drain(), "drain of an empty decoder");
    v.reset();
    CHECK(!v.hasPicture(), "reset leaves no picture");

    PspMpegAudioDecoder a;
    int16_t pcm[2048 * 2];
    std::memset(pcm, 0x5A, sizeof pcm);
    CHECK_EQ(a.decode(nullptr, 16, 0, 0, pcm, 2048), -1, "null audio frame");
    bool zero = true;
    for (int i = 0; i < 2048 * 2; i++) {
        if (pcm[i] != 0) {
            zero = false;
        }
    }
    CHECK(zero, "decode error zero-fills 2048 frames");
    CHECK_EQ(a.decode(junk, 16, 0x24, 0, nullptr, 2048), -1, "null audio out");
    CHECK_EQ(a.decode(junk, 0, 0, 0, pcm, 2048), -1, "empty audio frame");
    a.reset();
    int n = a.decode(junk, static_cast<int>(sizeof junk), 0, 1, pcm, 2048);
    CHECK(n == -1 || n == 2048, "garbage audio does not crash");
    n = a.decode(junk, 4, 0, 0, pcm, 3);
    CHECK(n == -1 || (n > 0 && n <= 3), "short capacity does not write past the buffer");
}

static const char* kDefaultBnd =
    "C:/Users/torso/Documents/deco/disc0/PSP_GAME/USRDIR/DATA_CMN.BND";
static constexpr long kMovieHeaderOffset = 0x13F771B;

static bool read_file_at(const char* path, long offset, Bytes* out, size_t len) {
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

static bool row_padding_untouched(const uint8_t* buf, int frameWidth, int picW, int height, int bpp) {
    if (picW >= frameWidth) {
        return true;
    }
    for (int y = 0; y < height; y++) {
        const uint8_t* row = buf + (static_cast<size_t>(y) * static_cast<size_t>(frameWidth) +
                                    static_cast<size_t>(picW)) *
                                       static_cast<size_t>(bpp);
        const size_t n = static_cast<size_t>(frameWidth - picW) * static_cast<size_t>(bpp);
        for (size_t i = 0; i < n; i++) {
            if (row[i] != 0xCD) {
                return false;
            }
        }
    }
    return true;
}

static void test_real_movie() {
    if (!PspVideoDecoder::available()) {
        std::printf("test_real_movie: skipped (FFmpeg not available)\n");
        return;
    }
    const char* env = std::getenv("PSPRECOMP_TEST_BND");
    const char* path = (env && *env) ? env : kDefaultBnd;
    Bytes hdr;
    if (!read_file_at(path, kMovieHeaderOffset, &hdr, 2048)) {
        std::printf("test_real_movie: skipped (%s not readable)\n", path);
        return;
    }
    PsmfHeader ph;
    CHECK(psmf_parse(hdr.data(), hdr.size(), &ph) == PsmfStatus::Ok, "header parses");
    Bytes ps;
    CHECK(read_file_at(path, kMovieHeaderOffset + static_cast<long>(ph.streamOffset), &ps, ph.streamSize),
          "stream readable");
    if (ps.size() != ph.streamSize) {
        return;
    }

    PspMpegDemux d(960 * 2048 + 2048);
    d.setAudioChannel(0);
    d.setVideoStreamId(PSMF_VIDEO_STREAM_ID);
    PspVideoDecoder vid;
    PspMpegAudioDecoder aud;
    std::vector<int16_t> pcm(2048 * 2);
    int pictures = 0;
    int aus = 0;
    int audio_n = 0;
    bool nonzero = false;
    size_t pos = 0;
    while (pos < ps.size() && (aus < 60 || audio_n < 20)) {
        size_t room = d.getRemainSize() / 2048 * 2048;
        size_t n = std::min<size_t>({room, ps.size() - pos, size_t(960) * 2048});
        if (n == 0) {
            CHECK(false, "demuxer stalled: no room and no progress");
            break;
        }
        CHECK(d.addStreamData(&ps[pos], n), "add packets");
        pos += n;
        d.demux();
        PspAvcAccessUnit au;
        while (aus < 60 && d.nextVideoAu(&au, false)) {
            if (vid.decode(au.data.data(), au.data.size())) {
                pictures++;
            }
            aus++;
        }
        const uint8_t* fb = nullptr;
        int c1 = 0;
        int c2 = 0;
        int64_t pts = 0;
        int sz;
        while ((sz = d.getNextAudioFrame(&fb, &c1, &c2, &pts)) > 0) {
            if (audio_n < 20) {
                const int got = aud.decode(fb, sz, c1, c2, pcm.data(), 2048);
                CHECK_EQ(got, 2048, "ATRAC frame yields 2048 stereo frames");
                if (got > 0) {
                    const int lim = got < 2048 ? got : 2048;
                    for (int i = 0; i < lim * 2; i++) {
                        if (pcm[static_cast<size_t>(i)] != 0) {
                            nonzero = true;
                        }
                    }
                }
                audio_n++;
            }
        }
    }
    if (aus < 60) {
        PspAvcAccessUnit au;
        while (aus < 60 && d.nextVideoAu(&au, true)) {
            if (vid.decode(au.data.data(), au.data.size())) {
                pictures++;
            }
            aus++;
        }
    }

    std::printf("test_real_movie: %d pictures / %d AUs, %dx%d\n", pictures, aus, vid.width(),
                vid.height());
    CHECK_EQ(aus, 60, "decoded the first 60 access units");
    CHECK(pictures >= 50, "at least 50 pictures from 60 access units");
    CHECK_EQ(ph.videoWidth, 480, "PSMF video width");
    CHECK_EQ(ph.videoHeight, 272, "PSMF video height");
    CHECK(vid.hasPicture(), "decoder kept a picture");
    CHECK_EQ(vid.width(), ph.videoWidth, "picture width matches the PSMF header");
    CHECK_EQ(vid.height(), ph.videoHeight, "picture height matches the PSMF header");
    CHECK_EQ(audio_n, 20, "decoded 20 ATRAC frames");
    CHECK(nonzero, "at least one audio sample is non-zero");

    if (!vid.hasPicture()) {
        return;
    }

    CHECK_EQ(vid.writeImage(nullptr, 64, 512, PSP_VIDEO_ABGR8888, 0, 0, 32, 32), 0,
             "null dst with a picture");
    uint8_t tiny[8];
    CHECK_EQ(vid.writeImage(tiny, sizeof tiny, 0, PSP_VIDEO_ABGR8888, 0, 0, 32, 32), 0,
             "frameWidth 0 with a picture");
    CHECK_EQ(vid.writeImage(tiny, sizeof tiny, 2049, PSP_VIDEO_ABGR8888, 0, 0, 32, 32), 0,
             "frameWidth > 2048 with a picture");
    CHECK_EQ(vid.writeImage(tiny, sizeof tiny, 32, 99, 0, 0, 32, 32), 0, "unknown mode with a picture");
    CHECK_EQ(vid.writeImage(tiny, sizeof tiny, 32, PSP_VIDEO_BGR5650, -4, 0, 32, 32), 0,
             "negative region with a picture");

    Bytes buf8888(static_cast<size_t>(512) * 4 * 272, 0xCD);
    const int n8888 = vid.writeImage(buf8888.data(), buf8888.size(), 512, PSP_VIDEO_ABGR8888, 0, 0,
                                     ph.videoWidth, ph.videoHeight);
    CHECK_EQ(n8888, 512 * 4 * 272, "8888 writeImage size");
    CHECK(row_padding_untouched(buf8888.data(), 512, ph.videoWidth, 272, 4),
          "8888 bytes past the picture stay 0xCD");
    bool alpha_ok = true;
    bool varied = false;
    uint32_t first = 0;
    bool have_first = false;
    if (ph.videoWidth > 0 && ph.videoHeight > 0) {
        for (int y = 0; y < ph.videoHeight; y++) {
            for (int x = 0; x < ph.videoWidth; x++) {
                const uint8_t* p = buf8888.data() + (static_cast<size_t>(y) * 512 + static_cast<size_t>(x)) * 4;
                if (p[3] != 0) {
                    alpha_ok = false;
                }
                const uint32_t px = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                    (static_cast<uint32_t>(p[2]) << 16);
                if (!have_first) {
                    first = px;
                    have_first = true;
                } else if (px != first) {
                    varied = true;
                }
            }
        }
    }
    CHECK(alpha_ok, "8888 alpha bytes of the picture are 0");
    CHECK(varied, "8888 picture is not a single colour");

    Bytes buf5650(static_cast<size_t>(512) * 2 * 272, 0xCD);
    const int n5650 = vid.writeImage(buf5650.data(), buf5650.size(), 512, PSP_VIDEO_BGR5650, 0, 0,
                                     ph.videoWidth, ph.videoHeight);
    CHECK_EQ(n5650, 512 * 2 * 272, "5650 writeImage size");
    CHECK(row_padding_untouched(buf5650.data(), 512, ph.videoWidth, 272, 2),
          "5650 bytes past the picture stay 0xCD");
    bool varied16 = false;
    uint16_t first16 = 0;
    bool have16 = false;
    if (ph.videoWidth > 0 && ph.videoHeight > 0) {
        for (int y = 0; y < ph.videoHeight; y++) {
            for (int x = 0; x < ph.videoWidth; x++) {
                const uint8_t* p = buf5650.data() + (static_cast<size_t>(y) * 512 + static_cast<size_t>(x)) * 2;
                const uint16_t px = static_cast<uint16_t>(p[0] | (p[1] << 8));
                if (!have16) {
                    first16 = px;
                    have16 = true;
                } else if (px != first16) {
                    varied16 = true;
                }
            }
        }
    }
    CHECK(varied16, "5650 picture is not a single colour");

    Bytes region(static_cast<size_t>(32) * 2 * 32, 0xCD);
    const int nreg = vid.writeImage(region.data(), region.size(), 32, PSP_VIDEO_BGR5650, 16, 16, 32, 32);
    CHECK_EQ(nreg, 32 * 2 * 32, "region writeImage size");
}

int main() {
    test_convert_line();
    test_write_without_picture();
    test_real_movie();
    std::printf("%d checks, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
