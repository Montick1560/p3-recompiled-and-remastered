#include "psp_wheel.h"

#include <algorithm>

void WheelPulses::add(int notches, int64_t now_ms) {
    const uint32_t mask = notches > 0 ? 0x0010u : 0x0040u;  // UP / DOWN
    for (int n = notches > 0 ? notches : -notches; n > 0 && pending() < kWheelMaxQueued; n--) {
        queue_.push_back({mask, now_ms});
    }
}

uint32_t WheelPulses::buttons(int64_t now_ms) {
    for (;;) {
        if (active_) {
            if (now_ms < active_start_ + kWheelPressMs) return active_mask_;
            next_free_ = active_start_ + kWheelPressMs + kWheelGapMs;
            active_ = false;
        }
        if (queue_.empty()) return 0;
        const int64_t start = std::max(next_free_, queue_.front().at);
        if (now_ms < start) return 0;
        active_ = true;
        active_mask_ = queue_.front().mask;
        active_start_ = start;
        queue_.pop_front();
    }
}
