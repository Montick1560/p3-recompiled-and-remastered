#pragma once
// A script wait written as N frames counts one frame per script run. When the
// script runs `runs_per_frame` times per original frame (a game unlocked from
// 30 to 60 fps runs it every vblank), the wait ends `runs_per_frame` times
// early. Given the frames-left value a wait's first run leaves (N - 1: the
// first run counts itself), returns the value to store instead so the wait
// lasts N * runs_per_frame runs. A one-frame wait (N = 1, left 0) is left as
// is: scripts use it to poll every frame ("check; wait 1; repeat"), and
// stretched to two runs the poll skipped every other vblank and missed flags
// that last one vblank at the new rate (Patapon 3's prologue drum tutorial never
// saw a PON). A negative count, or one run per frame, is returned unchanged. Pure.
#include <cstdint>

inline int32_t psp_frame_wait_left(int32_t left_after_first_run, int32_t runs_per_frame) {
    if (left_after_first_run <= 0 || runs_per_frame <= 1) return left_after_first_run;
    return (left_after_first_run + 1) * runs_per_frame - 1;
}
