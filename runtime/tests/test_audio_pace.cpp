// Unit tests for the HLE pacing helper (psp_time.cpp). The Windows Sleep()
// timer runs at ~64 Hz (15.6 ms); audio grains are 11.6 ms at 512 samples, so
// coarse sleeps slow headless audio pacing ~26% (T0 in
// build/autopilot/RELEVO.md). The landing test fails with a plain sleep_for
// (20 ms stretches to ~31 ms) and passes with psp_sleep_until's high-resolution
// waitable-timer path. Standalone: no SDL, no guest code.

#include "psp_time.h"

#include <chrono>
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

static void test_pace_us() {
    ASSERT_EQ(psp_audio_pace_us(0), 0, "zero samples");
    ASSERT_EQ(psp_audio_pace_us(441), 10000, "441 samples = 10 ms");
    ASSERT_EQ(psp_audio_pace_us(1024), 23219, "1024 samples ~ 23.22 ms");
    ASSERT_EQ(psp_audio_pace_us(44100), 1000000, "one second");
}

static void test_sleep_lands_within_a_millisecond() {
    const auto t0 = std::chrono::steady_clock::now();
    psp_sleep_until(t0 + std::chrono::milliseconds(20));
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    tests_run++;
    // Never early; and not stretched to two 15.6 ms ticks (31 ms) either.
    if (us < 20000 || us > 27000) {
        std::fprintf(stderr, "FAIL: 20 ms wait landed at %lld us (want 20000..27000)\n",
                     static_cast<long long>(us));
        failures++;
    }
}

static void test_sleep_lands_for_grain() {
    // The movie audio grain: 512 samples = 11.6 ms must land under 25 ms
    // (a coarse sleep floors it to 15.6 or 31.2 ms depending on the tick).
    const auto t0 = std::chrono::steady_clock::now();
    psp_sleep_until(t0 + std::chrono::microseconds(psp_audio_pace_us(512)));
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    tests_run++;
    if (us < 11600 || us > 25000) {
        std::fprintf(stderr, "FAIL: grain wait landed at %lld us (want 11600..25000)\n",
                     static_cast<long long>(us));
        failures++;
    }
}

static void test_pace_clock_absorbs_work() {
    using namespace std::chrono;
    AudioPaceClock clock;
    const auto t0 = steady_clock::time_point{} + seconds(1000);
    const auto d1 = clock.schedule(t0, 441);                    // t0 + 10 ms
    const auto d2 = clock.schedule(t0 + milliseconds(14), 441); // 4 ms of work absorbed
    const auto d3 = clock.schedule(t0 + milliseconds(24), 441); // still on schedule
    const auto d4 = clock.schedule(t0 + milliseconds(100), 441);  // idle: resync
    ASSERT_EQ(duration_cast<microseconds>(d1 - t0).count(), 10000, "first deadline");
    ASSERT_EQ(duration_cast<microseconds>(d2 - t0).count(), 20000, "work absorbed");
    ASSERT_EQ(duration_cast<microseconds>(d3 - t0).count(), 30000, "clock keeps time");
    ASSERT_EQ(duration_cast<microseconds>(d4 - t0).count(), 110000, "idle resync");
}

int main() {
    test_pace_us();
    test_sleep_lands_within_a_millisecond();
    test_sleep_lands_for_grain();
    test_pace_clock_absorbs_work();
    if (failures == 0) {
        std::printf("all %d psp_time tests passed\n", tests_run);
        return 0;
    }
    std::fprintf(stderr, "%d of %d psp_time tests failed\n", failures, tests_run);
    return 1;
}
