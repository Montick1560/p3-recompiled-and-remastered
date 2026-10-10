#include "psp_perf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

PerfCounters g_perf;

bool perf_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("PSPRECOMP_PERF");
        return e && std::strcmp(e, "1") == 0;
    }();
    return on;
}

PerfSnapshot perf_snapshot() {
    auto ld = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
    return PerfSnapshot{ld(g_perf.presents), ld(g_perf.ge_lists), ld(g_perf.ge_ns),
                        ld(g_perf.prims), ld(g_perf.draw_calls), ld(g_perf.tex_binds),
                        ld(g_perf.tex_misses), ld(g_perf.tex_hash_bytes), ld(g_perf.cpu_wait_ns)};
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
    if (!perf_enabled()) return;
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
