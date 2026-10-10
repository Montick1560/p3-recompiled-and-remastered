#pragma once
// Mouse wheel -> D-pad (pure). Each wheel notch becomes one timed UP/DOWN
// press followed by a release, so the game (which samples sceCtrl once per
// frame and reacts to new presses) counts every notch. The event loop owns it
// and publishes buttons(now) through an atomic.
#include <cstdint>
#include <deque>

constexpr int64_t kWheelPressMs = 70;   // >= 2 samples at 30 fps
constexpr int64_t kWheelGapMs = 70;     // release between notches
constexpr int kWheelMaxQueued = 4;      // a fast spin scrolls at most this far

class WheelPulses {
public:
    // notches > 0: wheel up (D-pad UP); < 0: wheel down (D-pad DOWN).
    void add(int notches, int64_t now_ms);
    // PSP button mask to OR into the pad at time now_ms (monotonic ms).
    uint32_t buttons(int64_t now_ms);
    int pending() const { return static_cast<int>(queue_.size()) + (active_ ? 1 : 0); }

private:
    struct Pulse { uint32_t mask; int64_t at; };
    std::deque<Pulse> queue_;
    bool active_ = false;
    uint32_t active_mask_ = 0;
    int64_t active_start_ = 0;
    int64_t next_free_ = 0;  // earliest start of the next press
};
