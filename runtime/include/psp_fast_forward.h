#pragma once
// Display fast-forward for loading screens. A game module that knows a loading screen is up
// asks for the next few vblank waits to be skipped. A skipped wait does not sleep until the
// vblank: the display clock jumps there at once (warp), so a loader that advances one step per
// frame runs as fast as the host draws instead of at 59.94 Hz. The budget lapses by itself a
// few vblanks after the requests stop. While it is active, presents are limited to one per
// real display period so a vsynced swap does not pace the skipped frames, and thread delays
// are cut to 1 ms so a loader thread polling its queue keeps up. Thread-safe.
#include <atomic>
#include <cstdint>

class PspFastForward {
public:
    /// Let the next `vblanks` waits skip (keeps the larger of the current and the new budget).
    void request(int vblanks) {
        int cur = budget_.load(std::memory_order_relaxed);
        while (cur < vblanks && !budget_.compare_exchange_weak(cur, vblanks)) {
        }
    }

    /// A vblank wait that would sleep `wait_us`: true = do not sleep; the clock warps by it.
    bool skip(int64_t wait_us) {
        int cur = budget_.load(std::memory_order_relaxed);
        do {
            if (cur <= 0) return false;
        } while (!budget_.compare_exchange_weak(cur, cur - 1));
        warp_.fetch_add(wait_us, std::memory_order_relaxed);
        return true;
    }

    /// Total microseconds the display clock has been moved forward.
    int64_t warp_us() const { return warp_.load(std::memory_order_relaxed); }

    /// Present now? Always while not fast-forwarding; otherwise once per 16684 us of real time
    /// (`real_us`: the host clock, not warped).
    bool present_due(int64_t real_us) {
        if (!active()) return true;
        const int64_t last = last_present_.load(std::memory_order_relaxed);
        if (last != kNever && real_us - last < kPeriodUs) return false;
        last_present_.store(real_us, std::memory_order_relaxed);
        return true;
    }

    bool active() const { return budget_.load(std::memory_order_relaxed) > 0; }

    /// A thread delay of `usec` (sceKernelDelayThread): in full normally, at most 1 ms while
    /// fast-forwarding, so a loader thread polling its queue every 10 ms keeps up.
    int64_t delay_us(int64_t usec) const {
        return (active() && usec > kMaxDelayUs) ? kMaxDelayUs : usec;
    }

private:
    static constexpr int64_t kNever = INT64_MIN;
    static constexpr int64_t kPeriodUs = 16684;  // one 59.94 Hz frame
    static constexpr int64_t kMaxDelayUs = 1000;
    std::atomic<int> budget_{0};
    std::atomic<int64_t> warp_{0};
    std::atomic<int64_t> last_present_{kNever};
};
