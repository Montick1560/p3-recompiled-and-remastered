// Unit tests for the present rectangle (aspect fit) and window scale parsing.

#include "psp_present.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static bool eq(PresentRect r, int x, int y, int w, int h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

int main() {
    CHECK(eq(present_fit_rect(480, 272), 0, 0, 480, 272), "native size fills the window");
    CHECK(eq(present_fit_rect(960, 544), 0, 0, 960, 544), "2x fills the window");
    CHECK(eq(present_fit_rect(1920, 1080), 7, 0, 1905, 1080), "16:9 gets side bars");
    CHECK(eq(present_fit_rect(800, 800), 0, 173, 800, 453), "square gets top/bottom bars");
    CHECK(eq(present_fit_rect(0, 272), 0, 0, 0, 0), "zero width");
    CHECK(eq(present_fit_rect(480, -1), 0, 0, 0, 0), "negative height");

    CHECK(present_window_scale(nullptr) == 2, "unset scale -> 2");
    CHECK(present_window_scale("") == 2, "empty scale -> 2");
    CHECK(present_window_scale("1") == 1, "scale 1");
    CHECK(present_window_scale("3") == 3, "scale 3");
    CHECK(present_window_scale("8") == 8, "scale 8");
    CHECK(present_window_scale("0") == 2, "scale 0 -> 2");
    CHECK(present_window_scale("9") == 2, "scale 9 -> 2");
    CHECK(present_window_scale("abc") == 2, "garbage -> 2");

    CHECK(present_render_scale(nullptr) == 1, "unset render scale -> 1");
    CHECK(present_render_scale("") == 1, "empty render scale -> 1");
    CHECK(present_render_scale("1") == 1, "render scale 1");
    CHECK(present_render_scale("4") == 4, "render scale 4");
    CHECK(present_render_scale("8") == 8, "render scale 8");
    CHECK(present_render_scale("0") == 1, "render scale 0 -> 1");
    CHECK(present_render_scale("9") == 1, "render scale 9 -> 1");
    CHECK(present_render_scale("-2") == 1, "negative render scale -> 1");
    CHECK(present_render_scale("abc") == 1, "garbage render scale -> 1");

    if (failures == 0) std::printf("test_present: all passed\n");
    return failures == 0 ? 0 : 1;
}
