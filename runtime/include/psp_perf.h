#pragma once
// PSPRECOMP_PERF=1: per-frame cost counters, logged by the main loop every
// 2 s as one "[PERF] ..." line. PSPRECOMP_FRAMELOG=<file>: the same counters
// as one CSV row per presented frame (hitches hide in 2 s averages); F8 marks
// the current frame. Every increment is guarded by perf_enabled(), so the off
// path costs one predictable branch.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

struct PerfCounters {
    std::atomic<uint64_t> presents{0}, ge_lists{0}, ge_ns{0}, prims{0}, draw_calls{0},
        tex_binds{0}, tex_misses{0}, tex_hash_bytes{0}, cpu_wait_ns{0}, tex_miss_ns{0}, swap_ns{0},
        pack_decode_ns{0};
};
extern PerfCounters g_perf;

struct PerfSnapshot {
    uint64_t presents, ge_lists, ge_ns, prims, draw_calls, tex_binds, tex_misses,
        tex_hash_bytes, cpu_wait_ns, tex_miss_ns, swap_ns, pack_decode_ns;
};

bool perf_enabled();
PerfSnapshot perf_snapshot();
// Pure: the counter deltas b - a over `seconds`, per frame (presents) or per second.
std::string perf_format(const PerfSnapshot& a, const PerfSnapshot& b, double seconds);
// Main loop: log a [PERF] line when 2 s have passed since the last one.
void perf_tick();

// PSPRECOMP_FRAMELOG columns: time since start and since the previous present (ms), then the frame's
// GE, texture-miss (decode + upload), HD-pack PNG decode (worker thread), swap and CPU-lock wait ms,
// draws, texture misses, hashed MB, and 1 on a frame the player marked (F8).
constexpr const char* kPerfFrameHeader =
    "t_ms,dt_ms,ge_ms,tex_miss_ms,pack_decode_ms,swap_ms,cpu_wait_ms,draws,tex_misses,tex_hash_mb,mark\n";
// Pure: the row of the frame between snapshots a and b.
std::string perf_frame_row(const PerfSnapshot& a, const PerfSnapshot& b, double t_ms, double dt_ms, bool mark);
// After each present: appends the frame's row when PSPRECOMP_FRAMELOG is set.
void perf_frame_end();
// F8: marks the next row; returns the mark's number (0 when the frame log is off).
int perf_frame_mark();

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
