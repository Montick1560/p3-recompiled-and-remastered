// psp_frame_latch.h: a change to the latched value, made by another thread, reaches the main thread's frame loop at
// its next frame; every other change at once.
#include "psp_frame_latch.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    FrameLatch l{3};  // the movie's "done"
    CHECK(l.view(0x0A1D7000u, 0, 10) == 0, "a new object: its value at once");
    CHECK(l.view(0x0A1D7000u, 1, 10) == 1, "starting (0 -> 1) in the same frame: at once");
    CHECK(l.view(0x0A1D7000u, 2, 11) == 2, "playing: at once");
    CHECK(l.view(0x0A1D7000u, 3, 12) == 2, "done in frame 12: the rest of frame 12 still sees playing");
    CHECK(l.view(0x0A1D7000u, 3, 12) == 2, "a later task of frame 12: still playing");
    CHECK(l.view(0x0A1D7000u, 3, 13) == 3, "frame 13: every task sees done");
    CHECK(l.view(0x0A1D7000u, 3, 14) == 3, "and keeps seeing it");
    CHECK(l.view(0x098E8EA0u, 3, 20) == 3, "another object first seen done: done at once");
    CHECK(l.view(0x098E8EA0u, 0, 21) == 0, "the next movie at the same address: its value at once");
    CHECK(l.view(0x098E8EA0u, 3, 21) == 0, "done in the frame it started: hidden in 21");
    CHECK(l.view(0x098E8EA0u, 3, 22) == 3, "shown in 22");
    if (failures == 0) std::printf("frame_latch: all passed\n");
    return failures == 0 ? 0 : 1;
}
