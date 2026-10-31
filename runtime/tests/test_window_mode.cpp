// Unit tests for the display mode rules (psp_window_mode.h): which mode PSPRECOMP_WINDOW_MODE /
// PSPRECOMP_FULLSCREEN select, the borderless rectangle and the F11 / Alt+Enter toggle.
#include "psp_window_mode.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

using M = PspWindowMode;

static void mode_from_env() {
    CHECK(psp_window_mode("borderless", nullptr) == M::Borderless, "borderless");
    CHECK(psp_window_mode("BORDERLESS", "0") == M::Borderless, "case-insensitive, beats fullscreen=0");
    CHECK(psp_window_mode(nullptr, "1") == M::Fullscreen, "legacy fullscreen=1");
    CHECK(psp_window_mode(nullptr, nullptr) == M::Windowed, "nothing set: windowed");
    CHECK(psp_window_mode("bogus", "1") == M::Fullscreen, "bad mode falls back to the legacy switch");
    CHECK(psp_window_mode("windowed", "1") == M::Windowed, "mode beats the legacy switch");
    CHECK(psp_window_mode("fullscreen", nullptr) == M::Fullscreen, "fullscreen");
    CHECK(psp_window_mode("", "0") == M::Windowed, "empty mode, fullscreen=0");
}

static void alone_grows_down() {
    const PspRect r = psp_borderless_rect({0, 0, 1366, 768}, nullptr, 0);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1366 && r.h == 769, "1 px taller, off the bottom edge");
}

static void neighbour_on_the_right_grows_down() {
    const PspRect o[] = {{1366, 0, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1366, 768}, o, 1);
    CHECK(r.w == 1366 && r.h == 769, "side by side: down, never into the right monitor");
}

static void neighbour_below_grows_right() {
    const PspRect o[] = {{0, 1080, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1920, 1080}, o, 1);
    CHECK(r.x == 0 && r.w == 1921 && r.h == 1080, "stacked: right");
}

static void below_and_right_grows_up() {
    const PspRect o[] = {{0, 1080, 1920, 1080}, {1920, 0, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1920, 1080}, o, 2);
    CHECK(r.y == -1 && r.h == 1081 && r.w == 1920, "up");
}

static void surrounded_shrinks() {
    const PspRect o[] = {{0, 1080, 1920, 1080}, {1920, 0, 1920, 1080}, {0, -1080, 1920, 1080}, {-1920, 0, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1920, 1080}, o, 4);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1079, "no free side: 1 px shorter");
}

static void corner_contact_ignored() {
    const PspRect o[] = {{1920, 1080, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1920, 1080}, o, 1);
    CHECK(r.h == 1081 && r.w == 1920, "diagonal monitor does not block the bottom");
}

static void negative_origin() {
    const PspRect o[] = {{0, 0, 1920, 1080}, {-1920, 0, 1920, 1080}};  // self included, skipped
    const PspRect r = psp_borderless_rect({-1920, 0, 1920, 1080}, o, 2);
    CHECK(r.x == -1920 && r.y == 0 && r.w == 1920 && r.h == 1081, "left monitor grows down");
}

static void toggle() {
    CHECK(psp_window_mode_toggle(M::Windowed, M::Borderless) == M::Borderless, "windowed -> configured");
    CHECK(psp_window_mode_toggle(M::Windowed, M::Windowed) == M::Fullscreen, "windowed config -> fullscreen");
    CHECK(psp_window_mode_toggle(M::Windowed, M::Fullscreen) == M::Fullscreen, "windowed -> fullscreen");
    CHECK(psp_window_mode_toggle(M::Borderless, M::Borderless) == M::Windowed, "borderless -> windowed");
    CHECK(psp_window_mode_toggle(M::Fullscreen, M::Borderless) == M::Windowed, "fullscreen -> windowed");
}

static void names() {
    CHECK(psp_window_mode_name(M::Windowed)[0] == 'w', "windowed name");
    CHECK(psp_window_mode_name(M::Borderless)[0] == 'b', "borderless name");
    CHECK(psp_window_mode_name(M::Fullscreen)[0] == 'f', "fullscreen name");
}

int main() {
    mode_from_env();
    alone_grows_down();
    neighbour_on_the_right_grows_down();
    neighbour_below_grows_right();
    below_and_right_grows_up();
    surrounded_shrinks();
    corner_contact_ignored();
    negative_origin();
    toggle();
    names();
    if (failures == 0) std::printf("test_window_mode: all passed\n");
    return failures == 0 ? 0 : 1;
}
