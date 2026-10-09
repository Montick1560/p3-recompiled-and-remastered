// Unit tests for the 59.94 Hz vblank clock (psp_vblank_clock.h): frame
// index from elapsed time and the wait until the next vblank boundary; and
// the SysClock -> seconds/microseconds split (psp_sysclock.h).

#include "psp_sysclock.h"
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
    // sceKernelSysClock2USec (PPSSPP sceKernelTime.cpp): first out = clock /
    // 1e6, second out = clock % 1e6, from the 64-bit microsecond clock.
    {
        uint32_t hi = 0, lo = 0;
        psp_sysclock_split(0x00000002540BE3FFULL, hi, lo);  // 9999999999 us
        ASSERT_EQ(hi, 9999, "sysclock seconds");
        ASSERT_EQ(lo, 999999, "sysclock remainder us");
        psp_sysclock_split(1000000ULL << 32, hi, lo);  // high word in use
        ASSERT_EQ(hi, 4294967296ULL % (1ULL << 32), "seconds keep the low 32 bits");
        ASSERT_EQ(lo, 0, "exact multiple of 1e6");
    }
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
