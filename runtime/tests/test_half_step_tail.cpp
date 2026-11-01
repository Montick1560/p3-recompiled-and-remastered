// psp_half_step_tail.h: a 60 Hz run of half steps gets the half step its 30 fps run had.
#include "psp_half_step_tail.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

// Feeds n moving updates of `step`, then one update that does not move; returns that update's extra.
static float run(HalfStepTail* t, int n, float step) {
    for (int i = 0; i < n; i++) CHECK(psp_half_step_tail(t, step) == 0.0f, "no extra while moving");
    return psp_half_step_tail(t, 0.0f);
}

int main() {
    HalfStepTail t;
    CHECK(run(&t, 107, 0.941f) == 0.941f, "odd run (107 half steps, 30 fps had 54) gets one more half step");
    CHECK(run(&t, 106, 0.941f) == 0.0f, "even run already matches 30 fps");
    CHECK(run(&t, 107, -0.47f) == -0.47f, "a backward run gets its half step backward");
    CHECK(psp_half_step_tail(&t, 0.0f) == 0.0f, "standing still adds nothing");
    for (int i = 0; i < 106; i++) psp_half_step_tail(&t, 0.941f);
    CHECK(run(&t, 1, 0.3f) == 0.0f, "a last step cut short (a clamp) gets nothing");
    CHECK(run(&t, 1, 0.941f) == 0.0f, "a single step is not a run");
    if (failures == 0) std::printf("half_step_tail: all tests passed\n");
    return failures ? 1 : 0;
}
