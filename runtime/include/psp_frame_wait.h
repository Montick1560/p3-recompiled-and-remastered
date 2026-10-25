#pragma once
// A script wait written as N frames counts one frame per script run. When the
// script runs `runs_per_frame` times per original frame (a game unlocked from
// 30 to 60 fps runs it every vblank), the wait ends `runs_per_frame` times
// early. Given the frames-left value a wait's first run leaves (N - 1: the
// first run counts itself), returns the value to store instead so the wait
// lasts N * runs_per_frame runs.
//
// The run that ends a wait goes on with the script, so it also decides on
// which frame the script next looks at the pad. With the pad update paced to
// 30 Hz, presses are only in the pad words on the pad-update frames. A wait
// that starts on the frame in between (`on_pad_frame` false) lasts one run
// less, so it ends on a pad-update frame and every later wait of that script
// starts on one too. Scripts both time with one-frame waits (Patapon 3's
// mission countdown TON, CHIN, KAN, Ya is a loop of them, so they must still
// last two runs) and poll with them ("check; wait 1; repeat": the prologue drum
// tutorial's hit checks, which on the in-between frames never saw a PON).
// A negative count, or one run per frame, is returned unchanged. Pure.
#include <cstdint>

inline int32_t psp_frame_wait_left(int32_t left_after_first_run, int32_t runs_per_frame, bool on_pad_frame) {
    if (left_after_first_run < 0 || runs_per_frame <= 1) return left_after_first_run;
    const int32_t left = (left_after_first_run + 1) * runs_per_frame - 1;
    return on_pad_frame ? left : left - 1;
}
