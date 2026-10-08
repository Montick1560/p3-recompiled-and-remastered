// Unit tests for the sceAudio output mixer (psp_audio_mixer.cpp): per-channel
// FIFOs of stereo s16, PSP volumes (0x8000 = unity), mono -> both sides,
// saturation, underrun silence. Standalone: no SDL.

#include "psp_audio_mixer.h"

#include <cstdio>
#include <vector>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        if ((actual) != (expected)) { \
            std::fprintf(stderr, "FAIL: %s: got %d, expected %d\n", msg, \
                static_cast<int>(actual), static_cast<int>(expected)); \
            failures++; \
        } \
    } while (0)

static void test_unity_stereo_passthrough() {
    PspAudioMixer m;
    const int16_t in[4] = {1000, -2000, 3000, -4000};  // 2 stereo frames
    m.push(0, in, 2, true, 0x8000, 0x8000);
    ASSERT_EQ(m.queued(0), 2, "two frames queued");
    int16_t out[4] = {};
    m.mix(out, 2);
    ASSERT_EQ(out[0], 1000, "L0"); ASSERT_EQ(out[1], -2000, "R0");
    ASSERT_EQ(out[2], 3000, "L1"); ASSERT_EQ(out[3], -4000, "R1");
    ASSERT_EQ(m.queued(0), 0, "drained");
}

static void test_half_volume_and_pan() {
    PspAudioMixer m;
    const int16_t in[2] = {1000, 1000};
    m.push(1, in, 1, true, 0x4000, 0x0000);
    int16_t out[2] = {};
    m.mix(out, 1);
    ASSERT_EQ(out[0], 500, "left at half volume");
    ASSERT_EQ(out[1], 0, "right muted");
}

static void test_mono_goes_to_both_sides() {
    PspAudioMixer m;
    const int16_t in[2] = {1234, -1234};  // 2 mono frames
    m.push(2, in, 2, false, 0x8000, 0x8000);
    int16_t out[4] = {};
    m.mix(out, 2);
    ASSERT_EQ(out[0], 1234, "mono L0"); ASSERT_EQ(out[1], 1234, "mono R0");
    ASSERT_EQ(out[2], -1234, "mono L1"); ASSERT_EQ(out[3], -1234, "mono R1");
}

static void test_channels_sum_and_saturate() {
    PspAudioMixer m;
    const int16_t a[2] = {30000, -30000};
    m.push(0, a, 1, true, 0x8000, 0x8000);
    m.push(PspAudioMixer::kOutput2, a, 1, true, 0x8000, 0x8000);
    int16_t out[2] = {};
    m.mix(out, 1);
    ASSERT_EQ(out[0], 32767, "positive saturation");
    ASSERT_EQ(out[1], -32768, "negative saturation");
}

static void test_underrun_is_silence() {
    PspAudioMixer m;
    const int16_t in[2] = {500, 500};
    m.push(0, in, 1, true, 0x8000, 0x8000);
    int16_t out[6] = {1, 1, 1, 1, 1, 1};
    m.mix(out, 3);
    ASSERT_EQ(out[0], 500, "queued frame plays");
    ASSERT_EQ(out[2], 0, "missing frame is silent L");
    ASSERT_EQ(out[5], 0, "missing frame is silent R");
}

int main() {
    test_unity_stereo_passthrough();
    test_half_volume_and_pan();
    test_mono_goes_to_both_sides();
    test_channels_sum_and_saturate();
    test_underrun_is_silence();
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
