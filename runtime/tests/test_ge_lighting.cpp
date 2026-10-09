// Unit tests for CPU-side per-vertex GE lighting (psp_ge_lighting.cpp).
// Semantics follow PPSSPP's GPU/Software/Lighting.cpp (see .brief/):
// emissive + ambient*matAmbient, then per enabled light ambient/diffuse/
// specular with attenuation (LKA/LKB/LKC), spot exponent/cutoff (LKS/LKO),
// MATERIALUPDATE bits selecting the vertex color, REVERSENORMAL, and
// alpha = ambientAlpha * materialAlpha / 255, clamped to 0..255.
//
// Standalone executable (test_ge_vertex convention): links only
// psp_ge_lighting.cpp -- no SDL/GL/scheduler deps.

#include "psp_ge_lighting.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        const int a_ = static_cast<int>(actual); \
        const int e_ = static_cast<int>(expected); \
        if (a_ != e_) { \
            std::fprintf(stderr, \
                "FAIL: %s: got %d, expected %d\n", msg, a_, e_); \
            failures++; \
        } \
    } while (0)

// Near-equal for attenuation/pow rounding (tolerance of 1 LSB).
#define ASSERT_NEAR_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        const int a_ = static_cast<int>(actual); \
        const int e_ = static_cast<int>(expected); \
        if (std::abs(a_ - e_) > 1) { \
            std::fprintf(stderr, \
                "FAIL: %s: got %d, expected %d (±1)\n", msg, a_, e_); \
            failures++; \
        } \
    } while (0)

static GeLightingState make_base() {
    GeLightingState s;
    s.ambientColor = 0x004C4C4Cu;  // 0xBBGGRR grey
    s.ambientAlpha = 0xFFu;
    s.materialEmissive = 0x000000u;
    s.materialAmbient = 0xFFFFFFu;
    s.materialDiffuse = 0xFFFFFFu;
    s.materialSpecular = 0xFFFFFFu;
    s.materialAlpha = 0xFFu;
    s.materialSpecularCoef = 16.0f;
    s.materialUpdate = 0u;
    s.lightMode = 0u;
    s.reverseNormal = false;
    return s;
}

static void lit(
    const GeLightingState& s, const float wpos[3],
    const float wn[3], const uint8_t* vc, uint8_t out[4]
) {
    ge_compute_lit_color(s, wpos, wn, vc, out);
}

// Title-menu case: lighting ON, no lights enabled, AMBIENTCOLOR=0x4C4C4C,
// MATERIALAMBIENT=white, colorless vertex -> dim grey, opaque.
static void test_ambient_only_grey() {
    GeLightingState s = make_base();
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 1.0f};
    uint8_t out[4] = {};
    lit(s, wpos, wn, nullptr, out);
    ASSERT_EQ(out[0], 0x4C, "ambient-only R");
    ASSERT_EQ(out[1], 0x4C, "ambient-only G");
    ASSERT_EQ(out[2], 0x4C, "ambient-only B");
    ASSERT_EQ(out[3], 0xFF, "ambient-only A");
}

// Alpha = ambientAlpha * materialAlpha / 255.
static void test_alpha_product() {
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 1.0f};
    {
        GeLightingState s = make_base();
        s.ambientAlpha = 0x80u;
        uint8_t out[4] = {};
        lit(s, wpos, wn, nullptr, out);
        ASSERT_EQ(out[3], 0x80, "alpha: ambient 0x80 * mat 0xFF");
    }
    {
        GeLightingState s = make_base();
        s.materialAlpha = 0x80u;
        uint8_t out[4] = {};
        lit(s, wpos, wn, nullptr, out);
        ASSERT_EQ(out[3], 0x80, "alpha: ambient 0xFF * mat 0x80");
    }
    {
        GeLightingState s = make_base();
        s.ambientAlpha = 0x80u;
        s.materialAlpha = 0x80u;
        uint8_t out[4] = {};
        lit(s, wpos, wn, nullptr, out);
        ASSERT_EQ(out[3], 0x40, "alpha: 0x80 * 0x80 -> 0x40");
    }
}

// Emissive adds straight on top of the ambient term.
static void test_emissive_add() {
    GeLightingState s = make_base();
    s.materialEmissive = 0x101010u;  // 0xBBGGRR: +0x10 per channel
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 1.0f};
    uint8_t out[4] = {};
    lit(s, wpos, wn, nullptr, out);
    ASSERT_EQ(out[0], 0x5C, "emissive R: 0x4C + 0x10");
    ASSERT_EQ(out[1], 0x5C, "emissive G: 0x4C + 0x10");
    ASSERT_EQ(out[2], 0x5C, "emissive B: 0x4C + 0x10");
}

// One directional light: diffuse = LDC * matDiffuse * max(N.L, 0).
static void test_directional_diffuse() {
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float front[3] = {0.0f, 0.0f, 1.0f};
    const float back[3] = {0.0f, 0.0f, -1.0f};
    // Facing the light: full diffuse on top of zero ambient.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0x000000u;
        s.materialDiffuse = 0x808080u;
        s.lights[0].enabled = true;
        s.lights[0].computation = 0u;  // diffuse only
        s.lights[0].type = 0u;         // directional
        s.lights[0].pos[0] = 0.0f;
        s.lights[0].pos[1] = 0.0f;
        s.lights[0].pos[2] = 1.0f;
        s.lights[0].diffuse = 0xFFFFFFu;
        uint8_t out[4] = {};
        lit(s, wpos, front, nullptr, out);
        ASSERT_EQ(out[0], 0x80, "dir diffuse R facing");
        ASSERT_EQ(out[1], 0x80, "dir diffuse G facing");
        ASSERT_EQ(out[2], 0x80, "dir diffuse B facing");
    }
    // Back-facing: N.L < 0 contributes nothing.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0x000000u;
        s.materialDiffuse = 0x808080u;
        s.lights[0].enabled = true;
        s.lights[0].computation = 0u;
        s.lights[0].type = 0u;
        s.lights[0].pos[0] = 0.0f;
        s.lights[0].pos[1] = 0.0f;
        s.lights[0].pos[2] = 1.0f;
        s.lights[0].diffuse = 0xFFFFFFu;
        uint8_t out[4] = {};
        lit(s, wpos, back, nullptr, out);
        ASSERT_EQ(out[0], 0x00, "dir diffuse R back-facing");
        ASSERT_EQ(out[1], 0x00, "dir diffuse G back-facing");
        ASSERT_EQ(out[2], 0x00, "dir diffuse B back-facing");
    }
    // REVERSENORMAL flips the normal: the back-facing vertex lights up.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0x000000u;
        s.reverseNormal = true;
        s.materialDiffuse = 0x808080u;
        s.lights[0].enabled = true;
        s.lights[0].computation = 0u;
        s.lights[0].type = 0u;
        s.lights[0].pos[0] = 0.0f;
        s.lights[0].pos[1] = 0.0f;
        s.lights[0].pos[2] = 1.0f;
        s.lights[0].diffuse = 0xFFFFFFu;
        uint8_t out[4] = {};
        lit(s, wpos, back, nullptr, out);
        ASSERT_EQ(out[0], 0x80, "dir diffuse R reversed normal");
    }
}

// Point light: att = 1 / (a + b*d + c*d^2), clamped to [0,1].
// Light at (0,0,10), vertex at origin -> d = 10.
static void test_point_attenuation() {
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 1.0f};
    // Constant term 1 -> att = 1 -> full white diffuse.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0x000000u;
        s.lights[0].enabled = true;
        s.lights[0].computation = 0u;
        s.lights[0].type = 1u;  // point
        s.lights[0].pos[0] = 0.0f;
        s.lights[0].pos[1] = 0.0f;
        s.lights[0].pos[2] = 10.0f;
        s.lights[0].att[0] = 1.0f;
        s.lights[0].att[1] = 0.0f;
        s.lights[0].att[2] = 0.0f;
        s.lights[0].diffuse = 0xFFFFFFu;
        uint8_t out[4] = {};
        lit(s, wpos, wn, nullptr, out);
        ASSERT_EQ(out[0], 0xFF, "point att=1 R");
    }
    // Constant term 2 -> att = 1/2 -> half diffuse.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0x000000u;
        s.lights[0].enabled = true;
        s.lights[0].computation = 0u;
        s.lights[0].type = 1u;  // point
        s.lights[0].pos[0] = 0.0f;
        s.lights[0].pos[1] = 0.0f;
        s.lights[0].pos[2] = 10.0f;
        s.lights[0].att[0] = 2.0f;
        s.lights[0].att[1] = 0.0f;
        s.lights[0].att[2] = 0.0f;
        s.lights[0].diffuse = 0xFFFFFFu;
        uint8_t out[4] = {};
        lit(s, wpos, wn, nullptr, out);
        ASSERT_NEAR_EQ(out[0], 0x80, "point att=1/2 R");
        ASSERT_NEAR_EQ(out[1], 0x80, "point att=1/2 G");
        ASSERT_NEAR_EQ(out[2], 0x80, "point att=1/2 B");
    }
}

// MATERIALUPDATE bit 0: ambient uses the vertex color instead of the register.
static void test_material_update_ambient() {
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 1.0f};
    const uint8_t vc[4] = {0x80u, 0x40u, 0x20u, 0xFFu};
    // Update bit set: white ambient * vertex color = vertex color.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0xFFFFFFu;
        s.materialAmbient = 0x000000u;
        s.materialUpdate = 1u;
        uint8_t out[4] = {};
        lit(s, wpos, wn, vc, out);
        ASSERT_EQ(out[0], 0x80, "matupdate ambient R from vertex");
        ASSERT_EQ(out[1], 0x40, "matupdate ambient G from vertex");
        ASSERT_EQ(out[2], 0x20, "matupdate ambient B from vertex");
    }
    // Update bit clear: black material register wins, vertex ignored.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0xFFFFFFu;
        s.materialAmbient = 0x000000u;
        s.materialUpdate = 0u;
        uint8_t out[4] = {};
        lit(s, wpos, wn, vc, out);
        ASSERT_EQ(out[0], 0x00, "no-update ambient R from register");
        ASSERT_EQ(out[1], 0x00, "no-update ambient G from register");
        ASSERT_EQ(out[2], 0x00, "no-update ambient B from register");
    }
}

// MATERIALUPDATE bit 1: diffuse uses the vertex color.
static void test_material_update_diffuse() {
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 1.0f};
    const uint8_t vc[4] = {0x80u, 0x80u, 0x80u, 0xFFu};
    // Update bit set: white light * vertex grey = grey.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0x000000u;
        s.materialDiffuse = 0x000000u;
        s.materialUpdate = 2u;
        s.lights[0].enabled = true;
        s.lights[0].computation = 0u;
        s.lights[0].type = 0u;
        s.lights[0].pos[2] = 1.0f;
        s.lights[0].diffuse = 0xFFFFFFu;
        uint8_t out[4] = {};
        lit(s, wpos, wn, vc, out);
        ASSERT_EQ(out[0], 0x80, "matupdate diffuse R from vertex");
    }
    // Update bit clear: black material diffuse -> no diffuse.
    {
        GeLightingState s = make_base();
        s.ambientColor = 0x000000u;
        s.materialDiffuse = 0x000000u;
        s.materialUpdate = 0u;
        s.lights[0].enabled = true;
        s.lights[0].computation = 0u;
        s.lights[0].type = 0u;
        s.lights[0].pos[2] = 1.0f;
        s.lights[0].diffuse = 0xFFFFFFu;
        uint8_t out[4] = {};
        lit(s, wpos, wn, vc, out);
        ASSERT_EQ(out[0], 0x00, "no-update diffuse R from register");
    }
}

// Specular (BOTH computation) folds into primary: N=H=(0,0,1) -> pow(1)=1.
static void test_specular_adds_to_primary() {
    GeLightingState s = make_base();
    s.ambientColor = 0x000000u;
    s.materialDiffuse = 0x000000u;
    s.materialSpecular = 0xFFFFFFu;
    s.materialSpecularCoef = 8.0f;
    s.lights[0].enabled = true;
    s.lights[0].computation = 1u;  // diffuse + specular
    s.lights[0].type = 0u;
    s.lights[0].pos[0] = 0.0f;
    s.lights[0].pos[1] = 0.0f;
    s.lights[0].pos[2] = 1.0f;
    s.lights[0].diffuse = 0x000000u;
    s.lights[0].specular = 0x808080u;
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 1.0f};
    uint8_t out[4] = {};
    lit(s, wpos, wn, nullptr, out);
    ASSERT_EQ(out[0], 0x80, "specular R in primary");
    ASSERT_EQ(out[1], 0x80, "specular G in primary");
    ASSERT_EQ(out[2], 0x80, "specular B in primary");
}

// Zero normal (vtype with no normal): no diffuse, ambient only.
static void test_zero_normal_no_diffuse() {
    GeLightingState s = make_base();
    s.ambientColor = 0x000000u;
    s.lights[0].enabled = true;
    s.lights[0].computation = 0u;
    s.lights[0].type = 0u;
    s.lights[0].pos[2] = 1.0f;
    s.lights[0].diffuse = 0xFFFFFFu;
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 0.0f};
    uint8_t out[4] = {};
    lit(s, wpos, wn, nullptr, out);
    ASSERT_EQ(out[0], 0x00, "zero normal R");
    ASSERT_EQ(out[1], 0x00, "zero normal G");
    ASSERT_EQ(out[2], 0x00, "zero normal B");
}

// Everything above range clamps to 255, nothing below 0.
static void test_clamping() {
    GeLightingState s = make_base();
    s.ambientColor = 0xFFFFFFu;
    s.materialEmissive = 0xFFFFFFu;  // white + white ambient -> way over
    const float wpos[3] = {0.0f, 0.0f, 0.0f};
    const float wn[3] = {0.0f, 0.0f, 1.0f};
    uint8_t out[4] = {};
    lit(s, wpos, wn, nullptr, out);
    ASSERT_EQ(out[0], 0xFF, "clamp R");
    ASSERT_EQ(out[1], 0xFF, "clamp G");
    ASSERT_EQ(out[2], 0xFF, "clamp B");
    ASSERT_EQ(out[3], 0xFF, "clamp A");
}

int main() {
    test_ambient_only_grey();
    test_alpha_product();
    test_emissive_add();
    test_directional_diffuse();
    test_point_attenuation();
    test_material_update_ambient();
    test_material_update_diffuse();
    test_specular_adds_to_primary();
    test_zero_normal_no_diffuse();
    test_clamping();

    if (failures == 0) {
        std::printf("test_ge_lighting: %d/%d PASS\n", tests_run, tests_run);
        return 0;
    }
    std::fprintf(stderr, "test_ge_lighting: %d/%d FAIL\n",
        tests_run - failures, tests_run);
    return 1;
}
