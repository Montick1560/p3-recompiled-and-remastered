// psp_fpu_apply_fcr31: FCR31.FS flushes denormal float results to zero (the
// DxD settings menu divides a tiny value and must get exactly 0, as on a PSP).

#include "psp_fpu.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static uint32_t bits_of_div(float a, float b) {
    volatile float x = a, y = b;
    volatile float r = x / y;
    float v = r;
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return u;
}

int main() {
    // FLT_MIN / 2^23 = 2^-149: the smallest denormal (bits 1) without FTZ.
    float a;
    const uint32_t abits = 0x00800000u;  // FLT_MIN, 1.18e-38
    std::memcpy(&a, &abits, 4);
    psp_fpu_apply_fcr31(0x00000E00u);
    CHECK(bits_of_div(a, 8388608.0f) != 0, "FS clear: denormal result is kept");
    psp_fpu_apply_fcr31(0x01000000u);
    CHECK(bits_of_div(a, 8388608.0f) == 0, "FS set: denormal result flushes to +0");
    CHECK(bits_of_div(1.0f, 4.0f) == 0x3E800000u, "FS set: normal results are unchanged");
    psp_fpu_apply_fcr31(0);
    CHECK(bits_of_div(a, 8388608.0f) != 0, "FS cleared again: denormals come back");
    if (failures == 0) std::printf("test_fpu: all passed\n");
    return failures == 0 ? 0 : 1;
}
