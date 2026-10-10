#pragma once
// PSPRECOMP_PERF=1: per-frame cost counters, logged by the main loop every
// 2 s as one "[PERF] ..." line. Every increment is guarded by perf_enabled(),
// so the off path costs one predictable branch.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

struct PerfCounters {
    std::atomic<uint64_t> presents{0}, ge_lists{0}, ge_ns{0}, prims{0}, draw_calls{0},
        tex_binds{0}, tex_misses{0}, tex_hash_bytes{0}, cpu_wait_ns{0};
};
extern PerfCounters g_perf;

struct PerfSnapshot {
    uint64_t presents, ge_lists, ge_ns, prims, draw_calls, tex_binds, tex_misses,
        tex_hash_bytes, cpu_wait_ns;
};

bool perf_enabled();
PerfSnapshot perf_snapshot();
// Pure: the counter deltas b - a over `seconds`, per frame (presents) or per second.
std::string perf_format(const PerfSnapshot& a, const PerfSnapshot& b, double seconds);
// Main loop: log a [PERF] line when 2 s have passed since the last one.
void perf_tick();

// Adds the scope's wall time to a counter (only when PSPRECOMP_PERF is on).
class PerfScope {
public:
    explicit PerfScope(std::atomic<uint64_t>& ns) : ns_(ns), on_(perf_enabled()) {
        if (on_) start_ = std::chrono::steady_clock::now();
    }
    ~PerfScope() {
        if (on_) {
            ns_.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start_).count()),
                          std::memory_order_relaxed);
        }
    }
    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;

private:
    std::atomic<uint64_t>& ns_;
    bool on_;
    std::chrono::steady_clock::time_point start_;
};
