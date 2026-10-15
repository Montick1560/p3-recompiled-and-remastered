#pragma once
// Paces a once-per-frame guest routine to at most one run per `period`
// vblanks. A game written for 30 fps counts input in frames; when a patch set
// makes it render at 60, its pad update runs on every second frame so held
// counts and key repeat keep their 30 fps timing. When the game slows down to
// `period` vblanks per frame on its own, every frame runs. Pure; the caller
// passes the current vblank index (psp_vblank_clock.h).
#include <cstdint>

struct VblankPacer {
    bool started = false;
    int64_t last = 0;

    bool due(int64_t vcount, int64_t period) {
        if (started && vcount - last < period) return false;
        started = true;
        last = vcount;
        return true;
    }
};
