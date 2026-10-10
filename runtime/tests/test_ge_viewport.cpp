// Unit tests for ge_compute_viewport_depth: the PSP viewport scale/offset +
// depth-range registers -> GL viewport + depth range mapping (issue #23).
// Oracle: PPSSPP GPU/Common/GPUStateUtils.cpp ConvertViewportAndScissor
// (1:1, non-supersampled, non-accurate-depth FBO case).
//
// Standalone executable (test_vfpu convention): links psp_ge_viewport.cpp
// only -- pure float/int math, no SDL/GL/scheduler deps.

#include "psp_ge_draw.h"

#include <cmath>
#include <cstdio>
#include <cstdint>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        const long a_ = static_cast<long>(actual); \
        const long e_ = static_cast<long>(expected); \
        if (a_ != e_) { \
            std::fprintf(stderr, \
                "FAIL: %s: got %ld, expected %ld\n", msg, a_, e_); \
            failures++; \
        } \
    } while (0)

#define ASSERT_NEAR(actual, expected, msg) \
    do { \
        tests_run++; \
        const float a_ = static_cast<float>(actual); \
        const float e_ = static_cast<float>(expected); \
        if (std::fabs(a_ - e_) > 1e-4f) { \
            std::fprintf(stderr, \
                "FAIL: %s: got %g, expected %g\n", msg, a_, e_); \
            failures++; \
        } \
    } while (0)

static constexpr int FBH = 272;  // PSP-native FBO height

// PSP defaults (psp_ge.cpp ge_init): a full-screen 480x272 viewport. The
// offsets that center it: off_x = vpXCenter - |vpXScale| = 2048 - 240 = 1808
// (raw 1808*16 = 28928); off_y = 2048 - 136 = 1912 (raw 1912*16 = 30592).
// This MUST reduce to the previously-hardcoded glViewport(0,0,480,272).
static void test_fullscreen_default_matches_hardcoded() {
    GeViewportDepth v = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f,
        32767.5f, 32767.5f,
        1808u * 16u, 1912u * 16u, FBH);
    ASSERT_EQ(v.x, 0, "fullscreen x == 0");
    ASSERT_EQ(v.y, 0, "fullscreen gl-y == 0 (272 - 0 - 272)");
    ASSERT_EQ(v.w, 480, "fullscreen w == 480 (2*240)");
    ASSERT_EQ(v.h, 272, "fullscreen h == 272 (2*136)");
}

// Default full-range depth: center 32767.5, scale 32767.5 ->
// [0, 65535]/65535 = [0, 1].
static void test_fullscreen_depth_range() {
    GeViewportDepth v = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f,
        32767.5f, 32767.5f, 0u, 0u, FBH);
    ASSERT_NEAR(v.near_z, 0.0f, "depth near == 0.0");
    ASSERT_NEAR(v.far_z, 1.0f, "depth far == 1.0");
}

// A negative Y-scale (PSP convention) must not flip the rectangle: width and
// height use |scale|, so the rect dims match the positive case.
static void test_negative_yscale_uses_abs() {
    GeViewportDepth pos = ge_compute_viewport_depth(
        240.0f, 136.0f, 2048.0f, 2048.0f, 1.0f, 1.0f,
        1808u * 16u, 1912u * 16u, FBH);
    GeViewportDepth neg = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f, 1.0f, 1.0f,
        1808u * 16u, 1912u * 16u, FBH);
    ASSERT_EQ(neg.h, pos.h, "|+136| == |-136| height");
    ASSERT_EQ(neg.w, pos.w, "width unaffected by y-scale sign");
    ASSERT_EQ(neg.h, 272, "height == 272");
}

// A half-height viewport in the bottom half of the PSP screen (top-left
// origin) must land in the GL bottom-left origin correctly:
//   top = 136, h = 136 -> gl_y = 272 - 136 - 136 = 0 (bottom of FBO).
static void test_y_flip_bottom_half() {
    // Want left=0,w=480 (full width), top=136,h=136.
    // h=136 -> |yscale|=68. top = vpYCenter - off_y - 68 = 136.
    // Pick vpYCenter=2048 -> off_y = 2048 - 68 - 136 = 1844 (raw *16).
    GeViewportDepth v = ge_compute_viewport_depth(
        240.0f, -68.0f, 2048.0f, 2048.0f, 1.0f, 1.0f,
        1808u * 16u, 1844u * 16u, FBH);
    ASSERT_EQ(v.h, 136, "half height == 136");
    ASSERT_EQ(v.y, 0, "bottom-half PSP -> gl-y 0 (272-136-136)");
}

// OFFSETX/Y are 1/16-subpixel; only the low 16 bits are used. A raw word
// 1808*16 = 28928 must yield off_x = 1808.0 exactly, and high bits ignored.
static void test_offset_subpixel_and_mask() {
    // off raw with garbage high bits set; low 16 = 28928 -> 1808.0.
    uint32_t raw_x = 28928u | 0xFFFF0000u;
    GeViewportDepth v = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f, 1.0f, 1.0f,
        raw_x, 1912u * 16u, FBH);
    ASSERT_EQ(v.x, 0, "off_x (low16 28928 -> 1808) gives left 0");
}

// Non-default depth window (e.g. minimized Z range) clamps into [0,1] and
// maps center+-scale through /65535.
static void test_depth_clamped_subrange() {
    // center 16384, scale 8192 -> near (16384-8192)/65535, far (16384+8192)/65535
    GeViewportDepth v = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f,
        8192.0f, 16384.0f, 0u, 0u, FBH);
    ASSERT_NEAR(v.near_z, 8192.0f / 65535.0f, "near == 8192/65535");
    ASSERT_NEAR(v.far_z, 24576.0f / 65535.0f, "far == 24576/65535");
}

// Reversed-Z (issue #23 Fix-2 precondition). A negative ZSCALE (Patapon)
// makes near = (center - scale)/65535 LARGER than far = (center + scale)/65535,
// i.e. glDepthRange(near>far). The draw path keys its glClearDepth(far) and
// reversed depth-func mapping off exactly this near>far ordering, so assert it
// holds for a representative Patapon-style ZSCALE<0 / full ZCENTER.
static void test_reversed_z_range() {
    GeViewportDepth v = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f,
        -32767.5f, 32767.5f, 0u, 0u, FBH);
    ASSERT_NEAR(v.near_z, 1.0f, "reversed near == 1.0 (center-scale)");
    ASSERT_NEAR(v.far_z, 0.0f, "reversed far == 0.0 (center+scale)");
    tests_run++;
    if (!(v.near_z > v.far_z)) {
        std::fprintf(stderr,
            "FAIL: reversed-Z must yield near>far (got near=%g far=%g)\n",
            v.near_z, v.far_z);
        failures++;
    }
}

// Render scale (M3b): outputs are FBO pixels = PSP pixels * scale.
static void test_fullscreen_scale2() {
    GeViewportDepth v = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f, 32767.5f, 32767.5f,
        1808u * 16u, 1912u * 16u, FBH, 2);
    ASSERT_EQ(v.x, 0, "scale2 fullscreen x");
    ASSERT_EQ(v.y, 0, "scale2 fullscreen y");
    ASSERT_EQ(v.w, 960, "scale2 fullscreen w");
    ASSERT_EQ(v.h, 544, "scale2 fullscreen h");
    ASSERT_NEAR(v.near_z, 0.0f, "scale does not touch depth (near)");
    ASSERT_NEAR(v.far_z, 1.0f, "scale does not touch depth (far)");
}

// Centred half-size viewport: PSP rect x 120..359, y 68..203 (top-left
// origin) -> GL (bottom-left) x=120 y=68 w=240 h=136; scale 3 triples all.
static void test_half_viewport_scale3() {
    GeViewportDepth v = ge_compute_viewport_depth(
        120.0f, -68.0f, 2048.0f, 2048.0f, 32767.5f, 32767.5f,
        1808u * 16u, 1912u * 16u, FBH, 3);
    ASSERT_EQ(v.x, 360, "scale3 half x");
    ASSERT_EQ(v.y, 204, "scale3 half y");
    ASSERT_EQ(v.w, 720, "scale3 half w");
    ASSERT_EQ(v.h, 408, "scale3 half h");
}

// A quarter-pixel offset rounds away at scale 1 (left = -0.25 -> 0) but is
// a whole FBO pixel at scale 4 (-1): scale must apply before rounding.
static void test_subpixel_offset_scales_before_rounding() {
    GeViewportDepth s1 = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f, 32767.5f, 32767.5f,
        1808u * 16u + 4u, 1912u * 16u, FBH, 1);
    GeViewportDepth s4 = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f, 32767.5f, 32767.5f,
        1808u * 16u + 4u, 1912u * 16u, FBH, 4);
    ASSERT_EQ(s1.x, 0, "quarter-pixel offset at scale 1");
    ASSERT_EQ(s4.x, -1, "quarter-pixel offset at scale 4");
}

static void test_scissor_never_set_is_disabled() {
    GeScissor s = ge_compute_scissor(0u, 0u, FBH, 2);
    ASSERT_EQ(s.enabled, false, "both registers 0 -> disabled");
}

static void test_scissor_fullscreen_scaled() {
    const uint32_t s2 = 479u | (271u << 10);
    GeScissor a = ge_compute_scissor(0u, s2, FBH, 1);
    ASSERT_EQ(a.enabled, true, "full scissor enabled");
    ASSERT_EQ(a.x, 0, "full x"); ASSERT_EQ(a.y, 0, "full y");
    ASSERT_EQ(a.w, 480, "full w"); ASSERT_EQ(a.h, 272, "full h");
    GeScissor b = ge_compute_scissor(0u, s2, FBH, 2);
    ASSERT_EQ(b.w, 960, "full w scale2"); ASSERT_EQ(b.h, 544, "full h scale2");
}

// PSP inclusive rect (10,20)-(109,69): 100x50, GL y = 272-1-69 = 202.
static void test_scissor_partial_scaled() {
    const uint32_t s1 = 10u | (20u << 10);
    const uint32_t s2 = 109u | (69u << 10);
    GeScissor a = ge_compute_scissor(s1, s2, FBH, 1);
    ASSERT_EQ(a.x, 10, "partial x"); ASSERT_EQ(a.y, 202, "partial y");
    ASSERT_EQ(a.w, 100, "partial w"); ASSERT_EQ(a.h, 50, "partial h");
    GeScissor b = ge_compute_scissor(s1, s2, FBH, 2);
    ASSERT_EQ(b.x, 20, "partial x scale2"); ASSERT_EQ(b.y, 404, "partial y scale2");
    ASSERT_EQ(b.w, 200, "partial w scale2"); ASSERT_EQ(b.h, 100, "partial h scale2");
}

static void test_scissor_inverted_is_empty() {
    GeScissor s = ge_compute_scissor(50u, 10u | (100u << 10), FBH, 2);
    ASSERT_EQ(s.enabled, true, "inverted still enabled");
    ASSERT_EQ(s.w, 0, "x2 < x1 -> zero width");
}

int main() {
    test_fullscreen_default_matches_hardcoded();
    test_fullscreen_depth_range();
    test_negative_yscale_uses_abs();
    test_y_flip_bottom_half();
    test_offset_subpixel_and_mask();
    test_depth_clamped_subrange();
    test_reversed_z_range();
    test_fullscreen_scale2();
    test_half_viewport_scale3();
    test_subpixel_offset_scales_before_rounding();
    test_scissor_never_set_is_disabled();
    test_scissor_fullscreen_scaled();
    test_scissor_partial_scaled();
    test_scissor_inverted_is_empty();

    if (failures == 0) {
        std::printf("test_ge_viewport: %d/%d PASS\n", tests_run, tests_run);
        return 0;
    }
    std::fprintf(stderr, "test_ge_viewport: %d/%d FAIL\n",
        tests_run - failures, tests_run);
    return 1;
}
