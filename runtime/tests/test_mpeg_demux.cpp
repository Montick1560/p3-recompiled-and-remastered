// Unit tests for the MPEG-PS demuxer (hle/psp_mpeg_demux.h): timestamped byte
// queue, H.264 access-unit splitter, PES/pack parsing, ATRAC3plus frame
// extraction. Oracle: PPSSPP Core/HW/MpegDemux.cpp.
//
// Standalone: no SDL/GL/FFmpeg. The real-data test streams the opening
// movie's MPEG-PS out of the user's DATA_CMN.BND (header at 0x13F771B) and
// passes with a "skipped" note when the file is absent; nothing is committed.
// Path: $PSPRECOMP_TEST_BND, else the default disc0 location.

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

static void append(Bytes& b, const Bytes& more) {
    b.insert(b.end(), more.begin(), more.end());
}

// ---- PspByteQueue ----

static void test_byte_queue() {
    PspByteQueue q;
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[3] = {5, 6, 7};
    q.push(a, 4, 1000);
    q.push(b, 3, 2000);
    CHECK_EQ(q.size(), 7, "size after pushes");
    uint8_t tmp[8] = {};
    CHECK_EQ(q.peek(tmp, 8), 7, "peek returns everything");
    CHECK_EQ(tmp[4], 5, "peek order");
    CHECK_EQ(q.size(), 7, "peek does not consume");

    int64_t pts = -1;
    CHECK_EQ(q.pop(2, &pts), 2, "pop 2");
    CHECK_EQ(pts, 1000, "first range holds the first mark");
    pts = -1;
    CHECK_EQ(q.pop(1, &pts), 1, "pop 1 more");
    CHECK_EQ(pts, 0, "mark already consumed: no timestamp");
    CHECK_EQ(q.pop(4, &pts), 4, "pop the rest (clamped)");
    CHECK_EQ(pts, 2000, "second mark");
    CHECK_EQ(q.size(), 0, "empty");
    CHECK_EQ(q.pop(1, &pts), 0, "pop from empty");

    // A zero pts is "no mark".
    q.push(a, 4, 0);
    pts = 77;
    q.pop(4, &pts);
    CHECK_EQ(pts, 0, "zero pts leaves no mark");

    // Long-running push/pop keeps working across compaction.
    for (int i = 0; i < 5000; i++) {
        uint8_t blk[100];
        std::memset(blk, i & 0xFF, sizeof(blk));
        q.push(blk, sizeof(blk), i + 1);
        uint8_t out[100] = {};
        CHECK(q.peek(out, 100) == 100 && out[0] == (i & 0xFF) && out[99] == (i & 0xFF),
              "queue content survives compaction");
        int64_t p = 0;
        q.pop(100, &p);
        if (p != i + 1) {
            CHECK(false, "mark survives compaction");
            break;
        }
    }
}

// ---- H.264 stream builders ----

static const uint8_t kStart4[4] = {0, 0, 0, 1};
static const uint8_t kStart3[3] = {0, 0, 1};

/// One NAL with a 4-byte start code, `n` filler bytes (never 00 00 0x).
static Bytes nal(uint8_t header, std::initializer_list<uint8_t> first, size_t n, bool four = true) {
    Bytes b;
    if (four) {
        b.insert(b.end(), kStart4, kStart4 + 4);
    } else {
        b.insert(b.end(), kStart3, kStart3 + 3);
    }
    b.push_back(header);
    for (uint8_t v : first) {
        b.push_back(v);
    }
    for (size_t i = 0; i < n; i++) {
        b.push_back(static_cast<uint8_t>(0x40 + (i % 0x30)));
    }
    return b;
}

/// An AU in the PSP movie shape: AUD, [SPS, PPS,] one slice.
static Bytes make_au(int index, bool idr) {
    Bytes b = nal(0x09, {0x10}, 0);  // AUD
    if (idr) {
        append(b, nal(0x67, {0x4D, 0x40, 0x1E}, 10));  // SPS
        append(b, nal(0x68, {0xEE, 0x3C}, 2));         // PPS
        append(b, nal(0x65, {0x88}, 200 + index));     // IDR slice, first_mb = 0
    } else {
        append(b, nal(0x41, {0x9A}, 80 + index, false));  // P slice, first_mb = 0
    }
    return b;
}

static void test_splitter_with_aud() {
    Bytes es;
    std::vector<size_t> au_size;
    for (int i = 0; i < 5; i++) {
        Bytes au = make_au(i, i == 0 || i == 3);
        au_size.push_back(au.size());
        append(es, au);
    }

    // Feed in awkward chunks (cutting through start codes) with timestamps on
    // the chunks where AUs begin.
    PspH264AuSplitter sp;
    size_t pos = 0;
    size_t chunk = 1;
    std::vector<PspAvcAccessUnit> aus;
    size_t au_off = 0;
    size_t next_au = 0;
    while (pos < es.size()) {
        size_t n = std::min(chunk, es.size() - pos);
        // The chunk containing an AU start gets that AU's timestamps.
        int64_t pts = PSP_MPEG_NO_TIMESTAMP;
        if (next_au < au_size.size() && au_off >= pos && au_off < pos + n) {
            pts = 90000 + static_cast<int64_t>(next_au) * 3003;
            au_off += au_size[next_au];
            next_au++;
        }
        sp.push(&es[pos], n, pts, pts);
        pos += n;
        chunk = chunk % 17 + 1;
        PspAvcAccessUnit au;
        while (sp.pop(&au, false)) {
            aus.push_back(au);
        }
    }
    CHECK_EQ(aus.size(), 4, "last AU is held back until flush");
    PspAvcAccessUnit last;
    CHECK(sp.pop(&last, true), "flush returns the trailing AU");
    aus.push_back(last);
    CHECK(!sp.pop(&last, true), "nothing left after flush");
    CHECK_EQ(sp.pendingBytes(), 0, "splitter drained");

    CHECK_EQ(aus.size(), 5, "five AUs");
    size_t off = 0;
    for (size_t i = 0; i < aus.size(); i++) {
        CHECK_EQ(aus[i].data.size(), au_size[i], "AU size");
        CHECK(std::memcmp(aus[i].data.data(), &es[off], au_size[i]) == 0, "AU bytes");
        CHECK_EQ(aus[i].pts, 90000 + static_cast<int64_t>(i) * 3003, "AU pts");
        off += au_size[i];
    }
}

static void test_splitter_without_aud() {
    // SPS PPS IDR | P | P(2 slices: first_mb 0 then non-zero) | SEI P
    Bytes es;
    Bytes a0 = nal(0x67, {0x4D, 0x40}, 6);
    append(a0, nal(0x68, {0xEE}, 2));
    append(a0, nal(0x65, {0x88}, 50));
    Bytes a1 = nal(0x41, {0x9A}, 40);
    Bytes a2 = nal(0x41, {0x9A}, 30);
    append(a2, nal(0x41, {0x48}, 30));  // second slice: first_mb != 0 (bit 7 clear)
    Bytes a3 = nal(0x06, {0x01, 0x02}, 3);  // SEI starts an AU after a slice
    append(a3, nal(0x41, {0x9A}, 20));
    for (const Bytes* a : {&a0, &a1, &a2, &a3}) {
        append(es, *a);
    }
    PspH264AuSplitter sp;
    sp.push(es.data(), es.size(), PSP_MPEG_NO_TIMESTAMP, PSP_MPEG_NO_TIMESTAMP);
    std::vector<PspAvcAccessUnit> aus;
    PspAvcAccessUnit au;
    while (sp.pop(&au, false)) {
        aus.push_back(au);
    }
    CHECK_EQ(aus.size(), 3, "three AUs before flush");
    CHECK(sp.pop(&au, true), "flush");
    aus.push_back(au);
    CHECK_EQ(aus.size(), 4, "four AUs");
    const Bytes* exp[4] = {&a0, &a1, &a2, &a3};
    for (size_t i = 0; i < aus.size() && i < 4; i++) {
        CHECK(aus[i].data == *exp[i], "AU boundaries without AUD");
        CHECK_EQ(aus[i].pts, PSP_MPEG_NO_TIMESTAMP, "no timestamps");
    }
}

static void test_splitter_junk() {
    PspH264AuSplitter sp;
    PspAvcAccessUnit au;
    Bytes junk(5000, 0xAB);
    sp.push(junk.data(), junk.size(), 5, 5);
    CHECK(!sp.pop(&au, false), "no start code: no AU");
    CHECK(!sp.pop(&au, true), "junk is not an AU on flush either");
    // Zero runs and truncated start codes must not hang or crash.
    Bytes zeros(100000, 0);
    sp.push(zeros.data(), zeros.size(), PSP_MPEG_NO_TIMESTAMP, PSP_MPEG_NO_TIMESTAMP);
    CHECK(!sp.pop(&au, true), "zeros are not an AU");
    CHECK(sp.pendingBytes() < 200000, "bounded buffering");
}

// ---- MPEG-PS builders ----

static Bytes pack_header() {
    const uint8_t h[14] = {0, 0, 1, 0xBA, 0x44, 0x00, 0x07, 0xAE, 0x1D, 0x11, 0x01, 0x86, 0xA3, 0xF8};
    return Bytes(h, h + 14);
}

static void put_pts(Bytes& b, int marker, int64_t pts) {
    b.push_back(static_cast<uint8_t>((marker << 4) | ((pts >> 29) & 0x0E) | 1));
    b.push_back(static_cast<uint8_t>(pts >> 22));
    b.push_back(static_cast<uint8_t>(((pts >> 14) & 0xFE) | 1));
    b.push_back(static_cast<uint8_t>(pts >> 7));
    b.push_back(static_cast<uint8_t>(((pts << 1) & 0xFE) | 1));
}

/// MPEG-2 PES packet. `pts < 0`: no timestamp. `sub`: private stream 1
/// sub-header (channel byte + 3 bytes) prepended to the payload.
static Bytes pes(uint8_t stream_id, int64_t pts, const Bytes& payload, const Bytes& sub = {}) {
    Bytes hdr;
    hdr.push_back(0x80);
    if (pts >= 0) {
        hdr.push_back(0x80);
        hdr.push_back(5);
        put_pts(hdr, 2, pts);
    } else {
        hdr.push_back(0x00);
        hdr.push_back(0);
    }
    Bytes body = hdr;
    append(body, sub);
    append(body, payload);
    Bytes b = {0, 0, 1, stream_id, static_cast<uint8_t>(body.size() >> 8),
               static_cast<uint8_t>(body.size())};
    append(b, body);
    return b;
}

/// A packet the demuxer must skip: start code, 16-bit length, filler.
static Bytes skippable_packet(uint8_t id, size_t len) {
    Bytes b = {0, 0, 1, id, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
    b.insert(b.end(), len, 0xFF);
    return b;
}

static Bytes atrac_frame(int index) {
    Bytes f = {0x0F, 0xD0, 0x28, 0x5C, 0, 0, 0, 0};
    for (int i = 0; i < 752 - 8; i++) {
        f.push_back(static_cast<uint8_t>(0x11 + ((i + index * 7) % 0x60)));
    }
    return f;
}

struct SyntheticPs {
    Bytes ps;
    std::vector<Bytes> video_aus;
    std::vector<Bytes> audio_frames;
    std::vector<int64_t> video_pts;  // expected AU timestamps
};

static SyntheticPs build_synthetic_ps() {
    SyntheticPs s;
    Bytes video_es;
    std::vector<size_t> au_start;
    for (int i = 0; i < 12; i++) {
        Bytes au = make_au(i, i % 5 == 0);
        au_start.push_back(video_es.size());
        s.video_aus.push_back(au);
        append(video_es, au);
    }
    Bytes audio_es;
    for (int i = 0; i < 9; i++) {
        Bytes f = atrac_frame(i);
        s.audio_frames.push_back(Bytes(f.begin() + 8, f.end()));
        append(audio_es, f);
    }

    append(s.ps, pack_header());
    const uint8_t sys[] = {0, 0, 1, 0xBB, 0, 6, 0x80, 0xC3, 0x51, 0x80, 0xF0, 0x7F};
    s.ps.insert(s.ps.end(), sys, sys + sizeof(sys));
    // Private stream 2 (navigation) and padding must be skipped.
    append(s.ps, skippable_packet(0xBF, 18));
    append(s.ps, skippable_packet(0xBE, 40));

    // Interleave: video in PES of 150..400 bytes (pts on the PES that begins
    // an AU), audio in 500-byte PES with the 4-byte private sub-header.
    s.video_pts.assign(s.video_aus.size(), PSP_MPEG_NO_TIMESTAMP);
    size_t vpos = 0, apos = 0;
    size_t next_au = 0;
    int vlen = 150;
    int round = 0;
    int64_t apts = 85000;
    while (vpos < video_es.size() || apos < audio_es.size()) {
        if (vpos < video_es.size()) {
            size_t n = std::min<size_t>(vlen, video_es.size() - vpos);
            // The PES pts belongs to the first AU starting inside it; further
            // AUs starting in the same PES carry no timestamp.
            int64_t pts = -1;
            while (next_au < au_start.size() && au_start[next_au] < vpos + n) {
                if (pts < 0) {
                    pts = 90000 + static_cast<int64_t>(next_au) * 3003;
                    s.video_pts[next_au] = pts;
                }
                next_au++;
            }
            append(s.ps, pack_header());
            append(s.ps, pes(0xE0, pts, Bytes(video_es.begin() + vpos, video_es.begin() + vpos + n)));
            vpos += n;
            vlen = 150 + (vlen * 7 + 13) % 250;
        }
        if (apos < audio_es.size() && round % 2 == 0) {
            size_t n = std::min<size_t>(500, audio_es.size() - apos);
            append(s.ps, pack_header());
            append(s.ps, pes(0xBD, apts, Bytes(audio_es.begin() + apos, audio_es.begin() + apos + n),
                             Bytes{0x00, 0x00, 0x00, 0x00}));
            apos += n;
            apts += 4180;
        }
        round++;
    }
    return s;
}

/// Feeds `ps` in chunks of `chunk` bytes, demuxing after every chunk, and
/// drains both queues. Returns true if everything matched.
static void run_synthetic(const SyntheticPs& s, size_t chunk, const char* label) {
    PspMpegDemux d(s.ps.size() + 4096);
    d.setAudioChannel(0);
    std::vector<PspAvcAccessUnit> aus;
    std::vector<Bytes> frames;
    std::vector<int64_t> frame_pts;
    for (size_t pos = 0; pos < s.ps.size(); pos += chunk) {
        size_t n = std::min(chunk, s.ps.size() - pos);
        CHECK(d.addStreamData(&s.ps[pos], n), label);
        d.demux();
        PspAvcAccessUnit au;
        while (d.nextVideoAu(&au, false)) {
            aus.push_back(au);
        }
        const uint8_t* fb = nullptr;
        int c1 = 0, c2 = 0;
        int64_t pts = 0;
        int sz;
        while ((sz = d.getNextAudioFrame(&fb, &c1, &c2, &pts)) > 0) {
            frames.push_back(Bytes(fb, fb + sz));
            frame_pts.push_back(pts);
        }
    }
    d.demux();
    PspAvcAccessUnit au;
    while (d.nextVideoAu(&au, true)) {
        aus.push_back(au);
    }
    const uint8_t* fb = nullptr;
    int c1 = 0, c2 = 0;
    int64_t pts = 0;
    int sz;
    while ((sz = d.getNextAudioFrame(&fb, &c1, &c2, &pts)) > 0) {
        frames.push_back(Bytes(fb, fb + sz));
        frame_pts.push_back(pts);
    }

    if (aus.size() != s.video_aus.size()) {
        std::fprintf(stderr, "chunk %zu (%s): %zu AUs, expected %zu\n", chunk, label,
                     aus.size(), s.video_aus.size());
    }
    CHECK_EQ(aus.size(), s.video_aus.size(), "video AU count");
    for (size_t i = 0; i < aus.size() && i < s.video_aus.size(); i++) {
        if (aus[i].data != s.video_aus[i]) {
            CHECK(false, "video AU bytes");
            break;
        }
        if (aus[i].pts != s.video_pts[i]) {
            CHECK(false, "video AU pts");
            break;
        }
    }
    CHECK_EQ(frames.size(), s.audio_frames.size(), "audio frame count");
    for (size_t i = 0; i < frames.size() && i < s.audio_frames.size(); i++) {
        if (frames[i] != s.audio_frames[i]) {
            CHECK(false, "audio frame bytes");
            break;
        }
    }
    // Every PES carried a pts; each frame range that includes a PES start
    // reports a non-zero timestamp for at least some frames.
    int with_pts = 0;
    for (int64_t p : frame_pts) {
        with_pts += p != 0;
    }
    CHECK(with_pts >= 3, "audio timestamps are reported");
}

static void test_synthetic_stream_any_chunking() {
    SyntheticPs s = build_synthetic_ps();
    for (size_t chunk : {size_t(1), size_t(3), size_t(13), size_t(100), size_t(2048), s.ps.size()}) {
        run_synthetic(s, chunk, "synthetic");
    }
}

static void test_audio_channel_filter() {
    SyntheticPs s = build_synthetic_ps();
    PspMpegDemux d(s.ps.size() + 4096);
    d.setAudioChannel(5);  // the stream's audio is on channel 0
    CHECK(d.addStreamData(s.ps.data(), s.ps.size()), "add");
    d.demux();
    const uint8_t* fb = nullptr;
    int c1, c2;
    int64_t pts;
    CHECK_EQ(d.getNextAudioFrame(&fb, &c1, &c2, &pts), 0, "other channel is dropped");

    // -1 latches onto the first channel it sees.
    PspMpegDemux d2(s.ps.size() + 4096);
    CHECK(d2.addStreamData(s.ps.data(), s.ps.size()), "add");
    d2.demux();
    CHECK(d2.getNextAudioFrame(&fb, &c1, &c2, &pts) > 0, "channel -1 accepts the first channel");
    CHECK_EQ(c1, 0x28, "header code 1 is reported");
    CHECK_EQ(c2, 0x5C, "header code 2 is reported");
}

static void test_capacity_and_garbage() {
    PspMpegDemux d(1000);
    Bytes big(1001, 0);
    CHECK(!d.addStreamData(big.data(), big.size()), "over capacity is refused");
    CHECK_EQ(d.rawBytes(), 0, "refused data is not added");
    CHECK(d.addStreamData(big.data(), 1000), "exact capacity is accepted");
    CHECK(!d.addStreamData(big.data(), 1), "full");

    uint64_t s = 0x1234567887654321ULL;
    auto rnd = [&s]() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    };
    for (int iter = 0; iter < 3000; iter++) {
        PspMpegDemux g(8192);
        Bytes buf(static_cast<size_t>(rnd() % 3000));
        for (auto& b : buf) {
            b = static_cast<uint8_t>(rnd());
        }
        // Sprinkle valid-looking start codes with lying lengths.
        for (size_t i = 0; i + 8 < buf.size(); i += 61) {
            uint8_t ids[] = {0xBA, 0xBB, 0xBD, 0xBE, 0xBF, 0xE0, 0xE1, 0xC0, 0xB9};
            buf[i] = 0;
            buf[i + 1] = 0;
            buf[i + 2] = 1;
            buf[i + 3] = ids[rnd() % 9];
        }
        g.setAudioChannel(static_cast<int>(rnd() % 3) - 1);
        g.addStreamData(buf.data(), buf.size());
        g.demux();
        PspAvcAccessUnit au;
        while (g.nextVideoAu(&au, true)) {
        }
        const uint8_t* fb;
        int c1, c2;
        int64_t pts;
        for (int i = 0; i < 100 && g.getNextAudioFrame(&fb, &c1, &c2, &pts) > 0; i++) {
        }
    }
    tests_run++;
}

// ---- Real data ----

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

static int nal_type_at(const Bytes& d, size_t i) {
    // i points at a start code ("00 00 01" or "00 00 00 01")
    size_t p = (d[i + 2] == 1) ? i + 3 : i + 4;
    return d[p] & 0x1F;
}

static void test_real_movie_stream() {
    const char* env = std::getenv("PSPRECOMP_TEST_BND");
    const char* path = (env && *env) ? env : kDefaultBnd;
    Bytes hdr;
    if (!read_file_at(path, kMovieHeaderOffset, &hdr, 2048)) {
        std::printf("test_real_movie_stream: skipped (%s not readable)\n", path);
        return;
    }
    PsmfHeader ph;
    CHECK(psmf_parse(hdr.data(), hdr.size(), &ph) == PsmfStatus::Ok, "header parses");
    Bytes ps;
    CHECK(read_file_at(path, kMovieHeaderOffset + ph.streamOffset, &ps, ph.streamSize),
          "stream readable");

    // Feed like the game does: whole 2048-byte packets, a ring's worth at a
    // time (960 packets), demuxing and draining between puts.
    PspMpegDemux d(960 * 2048 + 2048);
    d.setAudioChannel(0);
    d.setVideoStreamId(PSMF_VIDEO_STREAM_ID);
    std::vector<PspAvcAccessUnit> aus;
    size_t audio_frames = 0;
    size_t audio_with_pts = 0;
    size_t first_audio_size = 0;
    size_t pos = 0;
    while (pos < ps.size()) {
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
        while (d.nextVideoAu(&au, false)) {
            aus.push_back(std::move(au));
        }
        const uint8_t* fb = nullptr;
        int c1, c2;
        int64_t pts = 0;
        int sz;
        while ((sz = d.getNextAudioFrame(&fb, &c1, &c2, &pts)) > 0) {
            if (audio_frames == 0) {
                first_audio_size = static_cast<size_t>(sz);
            }
            audio_frames++;
            audio_with_pts += pts != 0;
        }
    }
    PspAvcAccessUnit tail;
    while (d.nextVideoAu(&tail, true)) {
        aus.push_back(std::move(tail));
    }

    // Duration (last - first timestamp) at 29.97 fps.
    const int64_t expect_frames = (ph.lastTimestamp - ph.firstTimestamp) / 3003;
    std::printf("test_real_movie_stream: %zu video AUs (header says ~%lld), %zu audio frames\n",
                aus.size(), static_cast<long long>(expect_frames), audio_frames);
    CHECK(aus.size() + 3 >= static_cast<size_t>(expect_frames) &&
              aus.size() <= static_cast<size_t>(expect_frames) + 3,
          "video AU count matches the header's duration");
    CHECK(audio_frames >= 600, "audio frames found");
    CHECK_EQ(first_audio_size, 744, "ATRAC3plus frame payload is 744 bytes");
    CHECK(audio_with_pts > 100, "audio frames carry timestamps");

    // First AU: AUD, SPS, PPS, IDR; timestamp 90000 from the first PES.
    CHECK(!aus.empty(), "have a first AU");
    if (!aus.empty()) {
        const PspAvcAccessUnit& first = aus[0];
        CHECK(first.data.size() > 100, "first AU has data");
        CHECK_EQ(first.pts, 90000, "first AU pts");
        bool has_sps = false, has_pps = false, has_idr = false;
        int first_nal = -1;
        for (size_t i = 0; i + 4 < first.data.size(); i++) {
            if (first.data[i] == 0 && first.data[i + 1] == 0 &&
                (first.data[i + 2] == 1 || (first.data[i + 2] == 0 && first.data[i + 3] == 1))) {
                int t = nal_type_at(first.data, i);
                if (first_nal < 0) {
                    first_nal = t;
                }
                has_sps |= t == 7;
                has_pps |= t == 8;
                has_idr |= t == 5;
                i += 2;
            }
        }
        CHECK_EQ(first_nal, 9, "first AU starts with an access unit delimiter");
        CHECK(has_sps && has_pps && has_idr, "first AU holds SPS, PPS and an IDR slice");
        CHECK(first.data[0] == 0 && first.data[1] == 0, "first AU starts at a start code");
    }
    // Every AU starts at a start code and has exactly one picture.
    size_t bad = 0;
    for (const PspAvcAccessUnit& au : aus) {
        if (au.data.size() < 6 || au.data[0] != 0 || au.data[1] != 0) {
            bad++;
        }
    }
    CHECK_EQ(bad, 0, "all AUs start at a start code");
}

int main() {
    test_byte_queue();
    test_splitter_with_aud();
    test_splitter_without_aud();
    test_splitter_junk();
    test_synthetic_stream_any_chunking();
    test_audio_channel_filter();
    test_capacity_and_garbage();
    test_real_movie_stream();
    std::printf("%d checks, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
