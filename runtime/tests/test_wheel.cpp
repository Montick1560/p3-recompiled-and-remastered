// psp_wheel: mouse-wheel notches -> timed D-pad UP/DOWN presses (pure).
#include "psp_wheel.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static constexpr uint32_t UP = 0x0010, DOWN = 0x0040;

static void one_notch_is_one_press() {
    WheelPulses w;
    CHECK(w.buttons(0) == 0, "idle: nothing pressed");
    w.add(1, 1000);
    CHECK(w.buttons(1000) == UP, "wheel up -> UP held");
    CHECK(w.buttons(1000 + kWheelPressMs - 1) == UP, "still held during the press");
    CHECK(w.buttons(1000 + kWheelPressMs) == 0, "released after the press time");
    CHECK(w.buttons(5000) == 0, "stays released");
}

static void notches_are_separate_presses() {
    WheelPulses w;
    w.add(-2, 0);  // two notches down
    CHECK(w.buttons(0) == DOWN, "first DOWN press");
    CHECK(w.buttons(kWheelPressMs) == 0, "gap between presses (game sees a release)");
    const int64_t second = kWheelPressMs + kWheelGapMs;
    CHECK(w.buttons(second) == DOWN, "second DOWN press");
    CHECK(w.buttons(second + kWheelPressMs) == 0, "done");
}

static void direction_change_and_cap() {
    WheelPulses w;
    w.add(1, 0);
    w.add(-1, 10);  // reversed while the first press runs: queued after it
    CHECK(w.buttons(10) == UP, "current press finishes first");
    CHECK(w.buttons(kWheelPressMs + kWheelGapMs) == DOWN, "then the queued DOWN");
    WheelPulses c;
    c.add(50, 0);  // a fast spin must not scroll for seconds
    CHECK(c.pending() <= kWheelMaxQueued, "queue is capped");
}

int main() {
    one_notch_is_one_press();
    notches_are_separate_presses();
    direction_change_and_cap();
    if (failures == 0) std::printf("test_wheel: all passed\n");
    return failures == 0 ? 0 : 1;
}
