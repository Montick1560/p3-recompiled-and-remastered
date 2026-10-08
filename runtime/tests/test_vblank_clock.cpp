// Unit tests for the 59.94 Hz vblank clock (psp_vblank_clock.h): frame
// index from elapsed time and the wait until the next vblank boundary.

#include "psp_vblank_clock.h"

#include <cstdio>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        if ((actual) != (expected)) { \
            std::fprintf(stderr, "FAIL: %s: got %lld, expected %lld\n", msg, \
                static_cast<long long>(actual), static_cast<long long>(expected)); \
            failures++; \
        } \
    } while (0)

int main() {
    ASSERT_EQ(psp_vblank_index(0), 0, "start is frame 0");
    ASSERT_EQ(psp_vblank_index(16683), 0, "just before the first boundary");
    ASSERT_EQ(psp_vblank_index(16684), 1, "just after the first boundary");
    ASSERT_EQ(psp_vblank_index(1000999), 59, "just before the 60th boundary");
    ASSERT_EQ(psp_vblank_index(1001000), 60, "60th boundary at exactly 1.001 s");
    ASSERT_EQ(psp_vblank_index(3600LL * 1000000), 215784, "an hour, no drift");

    // Waiting always reaches the *next* boundary, never a fixed period.
    ASSERT_EQ(psp_us_until_next_vblank(0), 16684, "from frame start");
    ASSERT_EQ(psp_us_until_next_vblank(10000), 6684, "mid-frame work shortens the wait");
    ASSERT_EQ(psp_us_until_next_vblank(16683), 1, "1 us before the boundary");
    ASSERT_EQ(psp_vblank_index(16683 + psp_us_until_next_vblank(16683)), 1,
              "landing on the next frame");

    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
