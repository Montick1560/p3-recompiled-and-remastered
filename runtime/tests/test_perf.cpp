// psp_perf: PSPRECOMP_PERF per-second counter line formatting (pure).
#include "psp_perf.h"

#include <algorithm>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static void formats_per_frame_values() {
    PerfSnapshot a{}, b{};
    b.presents = 60;
    b.ge_lists = 60;
    b.ge_ns = 60ull * 12'345'678ull;
    b.prims = 60ull * 870;
    b.draw_calls = 60ull * 860;
    b.tex_binds = 60ull * 410;
    b.tex_misses = 4;
    b.tex_hash_bytes = 60ull * 35ull * (1ull << 20);
    b.cpu_wait_ns = 0;
    const std::string s = perf_format(a, b, 2.0);
    std::printf("%s\n", s.c_str());
    CHECK(has(s, "[PERF] "), "prefix");
    CHECK(has(s, "fps=30.0"), "fps");
    CHECK(has(s, "ge_ms/frame=12.3"), "ge ms per frame");
    CHECK(has(s, "prims/frame=870"), "prims per frame");
    CHECK(has(s, "draws/frame=860"), "draws per frame");
    CHECK(has(s, "binds/frame=410"), "binds per frame");
    CHECK(has(s, "tex_miss/s=2"), "misses per second");
    CHECK(has(s, "tex_hash_MB/frame=35.0"), "hashed MB per frame");
    CHECK(has(s, "cpu_wait_ms/s=0"), "cpu wait");
}

static void no_presents_no_division_by_zero() {
    PerfSnapshot a{}, b{};
    b.prims = 100;
    const std::string s = perf_format(a, b, 2.0);
    CHECK(has(s, "fps=0.0"), "fps 0");
    CHECK(has(s, "prims/frame=100"), "per-frame divides by max(1, presents)");
    CHECK(has(s, "ge_ms/frame=0.0"), "ge 0");
}

// PSPRECOMP_FRAMELOG: one CSV row per presented frame, the counter deltas of that frame.
static void frame_row_columns() {
    PerfSnapshot a{}, b{};
    b.presents = 1;
    b.ge_ns = 7'250'000;
    b.draw_calls = 1422;
    b.tex_misses = 3;
    b.tex_miss_ns = 41'500'000;
    b.tex_hash_bytes = 512 * 1024;
    b.swap_ns = 2'000'000;
    b.pack_decode_ns = 30'000'000;
    b.cpu_wait_ns = 1'000'000;
    const std::string row = perf_frame_row(a, b, 12345.6, 58.3, true);
    std::printf("%s%s", kPerfFrameHeader, row.c_str());
    CHECK(row == "12345.6,58.30,7.25,41.50,30.00,2.00,1.00,1422,3,0.50,1\n", "row values in header order");
    const std::string header = kPerfFrameHeader;
    CHECK(std::count(header.begin(), header.end(), ',') == std::count(row.begin(), row.end(), ','), "row matches header");
    const std::string plain = perf_frame_row(a, b, 0.0, 16.7, false);
    CHECK(plain.size() > 3 && plain.compare(plain.size() - 3, 3, ",0\n") == 0, "no mark -> 0");
}

int main() {
    formats_per_frame_values();
    no_presents_no_division_by_zero();
    frame_row_columns();
    if (failures == 0) std::printf("test_perf: all passed\n");
    return failures == 0 ? 0 : 1;
}
