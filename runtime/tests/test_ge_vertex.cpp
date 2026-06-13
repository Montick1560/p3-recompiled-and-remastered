// Unit tests for ge_transform_vertices through-mode UV normalization
// (dothack-L6c fix). PSP through-mode (sprite/2D) texture coordinates
// arrive in TEXEL units; GL samples in normalized [0,1] coords, so the
// runtime must divide each textured through-mode UV by the texture
// dimensions. Oracle: PPSSPP GPU/Common/SoftwareTransformCommon.cpp
// (uscale /= curTextureWidth; vscale /= curTextureHeight).
//
// Standalone executable (test_vfpu convention): links psp_ge_vertex.cpp
// and calls the real ge_transform_vertices — no SDL/GL/scheduler deps.

#include "psp_ge_vertex.h"
#include "psp_ge.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
static int tests_run = 0;

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

// Build a through-mode GeState with identity tex scale/offset and a
// 128x128 base texture (log2 = 7 -> tex_size[0] = 7 | (7<<8) = 0x0707).
static GeState make_through_state(bool texture_enable) {
    GeState s;
    s.reset();
    s.vertex_type = 0x00800000u;  // bit 23 = through-mode
    s.tex_scale_u = 1.0f;
    s.tex_scale_v = 1.0f;
    s.tex_offset_u = 0.0f;
    s.tex_offset_v = 0.0f;
    s.tex_size[0] = 7u | (7u << 8);  // 128 x 128
    s.texture_enable = texture_enable;
    return s;
}

static DecodedVertex make_vertex(float u, float v) {
    DecodedVertex vtx;
    vtx.pos[0] = 100.0f;
    vtx.pos[1] = 100.0f;
    vtx.pos[2] = 0.0f;
    vtx.uv[0] = u;
    vtx.uv[1] = v;
    vtx.has_uv = true;
    vtx.has_color = false;
    vtx.has_normal = false;
    return vtx;
}

// Texturing enabled: texel-unit UVs are normalized by texW/texH.
static void test_through_textured_normalizes() {
    GeState s = make_through_state(true);
    std::vector<DecodedVertex> verts = {make_vertex(64.0f, 128.0f)};
    ge_transform_vertices(verts, s);
    ASSERT_NEAR(verts[0].uv[0], 0.5f,
        "through textured u: 64/128 -> 0.5");
    ASSERT_NEAR(verts[0].uv[1], 1.0f,
        "through textured v: 128/128 -> 1.0");
}

// Texturing disabled: a non-textured through quad must NOT be divided.
static void test_through_untextured_unchanged() {
    GeState s = make_through_state(false);
    std::vector<DecodedVertex> verts = {make_vertex(64.0f, 128.0f)};
    ge_transform_vertices(verts, s);
    ASSERT_NEAR(verts[0].uv[0], 64.0f,
        "through untextured u: scale/offset only, no divide");
    ASSERT_NEAR(verts[0].uv[1], 128.0f,
        "through untextured v: scale/offset only, no divide");
}

// Normalization is applied AFTER tex_scale/offset (matches the measured
// poke that divided the post-scale UV).
static void test_through_scale_offset_then_normalize() {
    GeState s = make_through_state(true);
    s.tex_scale_u = 2.0f;
    s.tex_offset_u = 0.0f;
    std::vector<DecodedVertex> verts = {make_vertex(32.0f, 64.0f)};
    ge_transform_vertices(verts, s);
    // u: 32*2 = 64, then /128 = 0.5
    ASSERT_NEAR(verts[0].uv[0], 0.5f,
        "through textured u: (32*2)/128 -> 0.5");
    ASSERT_NEAR(verts[0].uv[1], 0.5f,
        "through textured v: 64/128 -> 0.5");
}

int main() {
    test_through_textured_normalizes();
    test_through_untextured_unchanged();
    test_through_scale_offset_then_normalize();

    if (failures == 0) {
        std::printf("test_ge_vertex: %d/%d PASS\n", tests_run, tests_run);
        return 0;
    }
    std::fprintf(stderr, "test_ge_vertex: %d/%d FAIL\n",
        tests_run - failures, tests_run);
    return 1;
}
