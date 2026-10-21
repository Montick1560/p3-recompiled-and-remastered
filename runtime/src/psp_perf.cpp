#include "psp_perf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

PerfCounters g_perf;

static bool perf_log_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("PSPRECOMP_PERF");
        return e && std::strcmp(e, "1") == 0;
    }();
    return on;
}

static const char* frame_log_path() {
    static const char* p = [] {
        const char* e = std::getenv("PSPRECOMP_FRAMELOG");
        return e && e[0] ? e : nullptr;
    }();
    return p;
}

bool perf_enabled() {
    static const bool on = perf_log_enabled() || frame_log_path() != nullptr;
    return on;
}

PerfSnapshot perf_snapshot() {
    auto ld = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
    return PerfSnapshot{ld(g_perf.presents), ld(g_perf.ge_lists), ld(g_perf.ge_ns),
                        ld(g_perf.prims), ld(g_perf.draw_calls), ld(g_perf.tex_binds),
                        ld(g_perf.tex_misses), ld(g_perf.tex_hash_bytes), ld(g_perf.cpu_wait_ns),
                        ld(g_perf.tex_miss_ns), ld(g_perf.swap_ns), ld(g_perf.pack_decode_ns)};
}

std::string perf_format(const PerfSnapshot& a, const PerfSnapshot& b, double seconds) {
    const double secs = seconds > 0.0 ? seconds : 1.0;
    const uint64_t frames_raw = b.presents - a.presents;
    const double frames = static_cast<double>(std::max<uint64_t>(1, frames_raw));
    auto per_frame = [&](uint64_t x, uint64_t y) { return static_cast<double>(y - x) / frames; };
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "[PERF] fps=%.1f ge_ms/frame=%.1f lists/frame=%.1f prims/frame=%.0f "
                  "draws/frame=%.0f binds/frame=%.0f tex_miss/s=%.0f tex_hash_MB/frame=%.1f "
                  "cpu_wait_ms/s=%.0f",
                  static_cast<double>(frames_raw) / secs,
                  per_frame(a.ge_ns, b.ge_ns) / 1e6,
                  per_frame(a.ge_lists, b.ge_lists),
                  per_frame(a.prims, b.prims),
                  per_frame(a.draw_calls, b.draw_calls),
                  per_frame(a.tex_binds, b.tex_binds),
                  static_cast<double>(b.tex_misses - a.tex_misses) / secs,
                  per_frame(a.tex_hash_bytes, b.tex_hash_bytes) / static_cast<double>(1 << 20),
                  static_cast<double>(b.cpu_wait_ns - a.cpu_wait_ns) / 1e6 / secs);
    return buf;
}

void perf_tick() {
    if (!perf_log_enabled()) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    static PerfSnapshot prev = perf_snapshot();
    const clock::time_point now = clock::now();
    const double secs = std::chrono::duration<double>(now - last).count();
    if (secs < 2.0) return;
    const PerfSnapshot cur = perf_snapshot();
    std::fprintf(stderr, "%s\n", perf_format(prev, cur, secs).c_str());
    prev = cur;
    last = now;
}

std::string perf_frame_row(const PerfSnapshot& a, const PerfSnapshot& b, double t_ms, double dt_ms, bool mark) {
    auto ms = [](uint64_t x, uint64_t y) { return static_cast<double>(y - x) / 1e6; };
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%llu,%llu,%.2f,%d\n", t_ms, dt_ms,
                  ms(a.ge_ns, b.ge_ns), ms(a.tex_miss_ns, b.tex_miss_ns), ms(a.pack_decode_ns, b.pack_decode_ns),
                  ms(a.swap_ns, b.swap_ns), ms(a.cpu_wait_ns, b.cpu_wait_ns),
                  static_cast<unsigned long long>(b.draw_calls - a.draw_calls),
                  static_cast<unsigned long long>(b.tex_misses - a.tex_misses),
                  static_cast<double>(b.tex_hash_bytes - a.tex_hash_bytes) / static_cast<double>(1 << 20),
                  mark ? 1 : 0);
    return buf;
}

static std::atomic<int> g_marks{0}, g_marks_written{0};

int perf_frame_mark() {
    if (frame_log_path() == nullptr) return 0;
    const int n = g_marks.fetch_add(1, std::memory_order_relaxed) + 1;
    std::fprintf(stderr, "[FRAMELOG] mark %d\n", n);
    return n;
}

void perf_frame_end() {
    const char* path = frame_log_path();
    if (path == nullptr) return;
    using clock = std::chrono::steady_clock;
    static FILE* f = [path] {
        FILE* out = std::fopen(path, "w");
        if (out != nullptr) {
            std::fputs(kPerfFrameHeader, out);
            std::fprintf(stderr, "[FRAMELOG] writing %s (F8 marks a hitch)\n", path);
        } else {
            std::fprintf(stderr, "[FRAMELOG] cannot open %s\n", path);
        }
        return out;
    }();
    if (f == nullptr) return;
    static const clock::time_point start = clock::now();
    static clock::time_point last = start;
    static PerfSnapshot prev = perf_snapshot();
    static int rows = 0;
    const clock::time_point now = clock::now();
    const PerfSnapshot cur = perf_snapshot();
    const int marks = g_marks.load(std::memory_order_relaxed);
    const bool mark = marks != g_marks_written.load(std::memory_order_relaxed);
    if (mark) g_marks_written.store(marks, std::memory_order_relaxed);
    const std::string row = perf_frame_row(prev, cur, std::chrono::duration<double, std::milli>(now - start).count(),
                                           std::chrono::duration<double, std::milli>(now - last).count(), mark);
    std::fputs(row.c_str(), f);
    if (mark || ++rows % 120 == 0) std::fflush(f);  // a crash or a kill keeps all but the last 2 s
    prev = cur;
    last = now;
}
