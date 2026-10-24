#pragma once
// Runs a once-per-frame guest routine at most once per advance of a counter
// that another routine moves. When a patch set makes a 30 fps game render at
// 60 and paces its pad update to 30 Hz (psp_vblank_pacer.h), a routine that
// reads the pad's held-frame counters every frame sees each value on two
// frames; gating it on the update count makes it see each value once, as at
// 30 fps. When the counter moves on every call (30 fps), every call runs. Pure.
#include <cstdint>

struct OncePerCount {
    bool started = false;
    uint64_t last = 0;

    bool due(uint64_t count) {
        if (started && count == last) return false;
        started = true;
        last = count;
        return true;
    }
};
