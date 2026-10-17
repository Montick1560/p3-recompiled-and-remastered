// Unit tests for the 59.94 Hz vblank clock (psp_vblank_clock.h): frame
// index from elapsed time and the wait until the next vblank boundary; and
// the SysClock -> seconds/microseconds split (psp_sysclock.h); the loading-screen
// fast-forward budget and clock warp (psp_fast_forward.h).

#include "psp_fast_forward.h"
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

    // Fast-forward (psp_fast_forward.h): skipped waits warp the clock by their wait.
    {
        PspFastForward ff;
        ASSERT_EQ(ff.skip(1000), false, "no request: waits sleep");
        ASSERT_EQ(ff.warp_us(), 0, "no request: no warp");
        ASSERT_EQ(ff.present_due(0), true, "no request: every present");
        ff.request(2);
        ASSERT_EQ(ff.active(), true, "a request activates it");
        ASSERT_EQ(ff.skip(100), true, "first requested wait skips");
        ASSERT_EQ(ff.skip(200), true, "second requested wait skips");
        ASSERT_EQ(ff.warp_us(), 300, "warp = sum of the skipped waits");
        ASSERT_EQ(ff.skip(50), false, "budget spent: the wait sleeps again");
        ASSERT_EQ(ff.warp_us(), 300, "a slept wait does not warp");
        ASSERT_EQ(ff.active(), false, "lapses on its own");
        ff.request(3);
        ff.request(1);
        ASSERT_EQ(ff.skip(1) && ff.skip(1) && ff.skip(1), true, "a smaller request keeps the budget");
        ASSERT_EQ(ff.skip(1), false, "then lapses");
        ff.request(4);
        ASSERT_EQ(ff.present_due(1000000), true, "first present while fast-forwarding");
        ASSERT_EQ(ff.present_due(1010000), false, "10 ms later: skipped");
        ASSERT_EQ(ff.present_due(1016684), true, "one display period later: presented");
        PspFastForward d;
        ASSERT_EQ(d.delay_us(10000), 10000, "not fast-forwarding: delays sleep in full");
        d.request(2);
        ASSERT_EQ(d.delay_us(10000), 1000, "fast-forwarding: a 10 ms poll delay sleeps 1 ms");
        ASSERT_EQ(d.delay_us(300), 300, "fast-forwarding: shorter delays unchanged");
        ASSERT_EQ(d.skip(1) && d.skip(1), true, "delays do not spend the vblank budget");
        PspFastForward one;
        const int64_t t = 10000;
        one.request(1);
        one.skip(psp_us_until_next_vblank(t));
        ASSERT_EQ(psp_vblank_index(t + one.warp_us()) - psp_vblank_index(t), 1,
                  "one skipped wait = one vblank later at the same real time");
    }

    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
