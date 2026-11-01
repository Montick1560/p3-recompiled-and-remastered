#pragma once
// The last half step of a move halved for 60 fps. A guest move that adds a fixed step on every update
// (an army march) runs, at 30 fps, on every other vblank from its first one, so a move lasting D vblanks
// takes ceil(D / 2) steps: its last step covers the vblank after the move ended. At 60 fps with the
// step halved it takes D half steps, half a step short when D is odd (march: 107 vblanks, 54 steps at
// 30 fps vs 53.5 at 60). Feed the move's delta of every update; on the first update that does not move
// after a run of an odd number of equal half steps it returns that half step (the 30 fps step's second
// half, on the vblank it covered), else 0. A run whose last step differs from the one before (cut short
// by a clamp or a target) gets nothing. Pure.
// shortcut: a pause in the middle of a move (one update without movement) ends the run there; a move
// that pauses and resumes may get its half step early. Fine for the march (it never pauses mid-command).
#include <cmath>

struct HalfStepTail {
    int steps = 0;
    float last = 0.0f, before = 0.0f;
};

inline float psp_half_step_tail(HalfStepTail* t, float delta) {
    if (delta != 0.0f) {
        t->before = t->last;
        t->last = delta;
        t->steps++;
        return 0.0f;
    }
    const bool odd_run = t->steps >= 3 && (t->steps & 1) && std::fabs(t->last - t->before) <= 1e-4f * std::fabs(t->last);
    t->steps = 0;
    return odd_run ? t->last : 0.0f;
}
