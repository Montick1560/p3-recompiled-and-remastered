// Unit tests for ge_transform_vertices through-mode UV normalization
// (dothack-L6c fix). PSP through-mode (sprite/2D) texture coordinates
// arrive in TEXEL units; GL samples in normalized [0,1] coords, so the
// runtime must divide each textured through-mode UV by the texture
// dimensions. Oracle: PPSSPP GPU/Common/SoftwareTransformCommon.cpp
// (uscale /= curTextureWidth; vscale /= curTextureHeight).
//
// Also covers the clip-space output contract: transform mode emits clip
// (x,y,z,w) WITHOUT the CPU perspective divide (GL performs it, giving
// near-plane clipping and perspective-correct UV interpolation), through
// mode emits NDC with w = 1, and ge_expand_rectangle divides rectangle
// corners by clip w (dropping rectangles with a w <= 0 corner).
//
// Standalone executable (test_vfpu convention): links psp_ge_vertex.cpp
// and calls the real ge_transform_vertices — no SDL/GL/scheduler deps.

#include "psp_ge_vertex.h"
#include "psp_ge.h"
#include "psp_ge_mask.h"
#include "psp_ge_blend.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

static DecodedVertex make_pos_vertex(float x, float y, float z) {
    DecodedVertex v;
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
    v.uv[0] = 0.0f; v.uv[1] = 0.0f;
    v.has_uv = false; v.has_color = false; v.has_normal = false;
    return v;
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

// ---- Clip-space output (near-plane clipping + perspective-correct UVs) ----
//
// Transform mode must emit clip coordinates (pos = clip.xyz, w = clip.w)
// WITHOUT the CPU perspective divide: GL's own divide provides near-plane
// clipping (vertices with w <= 0 no longer flip to the opposite screen
// side and drag "fan" spikes across the frame) and perspective-correct
// UV interpolation.

// Transform-mode state: identity world & view, minimal perspective proj
// (col-major 4x4): clip = (x, y, z, -z). z < 0 is in front (w > 0);
// z > 0 is behind the camera (w < 0).
static GeState make_perspective_state() {
    GeState s;
    s.reset();
    s.vertex_type = 0x000183u;  // transform mode (bit 23 clear)
    s.world_matrix[0] = 1; s.world_matrix[4] = 1; s.world_matrix[8] = 1;
    s.view_matrix[0] = 1; s.view_matrix[4] = 1; s.view_matrix[8] = 1;
    s.proj_matrix[0] = 1.0f;
    s.proj_matrix[5] = 1.0f;
    s.proj_matrix[10] = 1.0f;
    s.proj_matrix[11] = -1.0f;  // clip.w = -z
    return s;
}

// A vertex in front of the camera keeps clip xyz in pos and clip w in w
// (no divide).
static void test_transform_front_keeps_clip() {
    GeState s = make_perspective_state();
    // z = -2 -> clip = (1, 2, -2, 2)
    std::vector<DecodedVertex> verts = {make_pos_vertex(1.0f, 2.0f, -2.0f)};
    ge_transform_vertices(verts, s);
    ASSERT_NEAR(verts[0].pos[0], 1.0f,
        "front: pos.x == clip.x (no divide)");
    ASSERT_NEAR(verts[0].pos[1], 2.0f,
        "front: pos.y == clip.y (no divide)");
    ASSERT_NEAR(verts[0].pos[2], -2.0f,
        "front: pos.z == clip.z (no divide)");
    ASSERT_NEAR(verts[0].w, 2.0f,
        "front: w == clip.w (no divide)");
}

// A vertex behind the camera keeps w < 0: the old CPU divide flipped it
// to the opposite screen side; GL now clips it at the near plane.
static void test_transform_behind_keeps_negative_w() {
    GeState s = make_perspective_state();
    // z = 3 -> clip = (1, 2, 3, -3)
    std::vector<DecodedVertex> verts = {make_pos_vertex(1.0f, 2.0f, 3.0f)};
    ge_transform_vertices(verts, s);
    ASSERT_NEAR(verts[0].pos[0], 1.0f,
        "behind: pos.x not divided/flipped");
    ASSERT_NEAR(verts[0].pos[1], 2.0f,
        "behind: pos.y not divided/flipped");
    ASSERT_NEAR(verts[0].pos[2], 3.0f,
        "behind: pos.z not divided/flipped");
    ASSERT_NEAR(verts[0].w, -3.0f,
        "behind: w < 0 reaches GL (near-clipped)");
}

// Through mode still maps to NDC and emits w == 1.
static void test_through_w_is_one() {
    GeState s = make_through_state(false);
    std::vector<DecodedVertex> verts = {make_vertex(64.0f, 128.0f)};
    ge_transform_vertices(verts, s);
    // make_vertex pos = (100, 100, 0)
    ASSERT_NEAR(verts[0].pos[0], 100.0f / 240.0f - 1.0f,
        "through: NDC x");
    ASSERT_NEAR(verts[0].pos[1], 1.0f - 100.0f / 136.0f,
        "through: NDC y");
    ASSERT_NEAR(verts[0].pos[2], 0.0f,
        "through: NDC z");
    ASSERT_NEAR(verts[0].w, 1.0f,
        "through: w == 1");
}

// ---- Collapsed-MVP degeneracy detection (Patapon-restore fix) ----
//
// The guest uploads matrices that are non-zero and non-singular but compose
// into an MVP that maps a whole primitive onto a sub-pixel cluster (#27
// dataflow family). ge_transform_vertices must detect this generically and
// route to the installed degenerate fallback. We install a sentinel fallback
// that stamps a marker so the test can tell which path ran.

static bool g_fallback_fired = false;
static void sentinel_fallback(const float wpos[3], float out[3]) {
    g_fallback_fired = true;
    // Map to a recognizable NDC so we can also assert the values.
    out[0] = wpos[0] / 240.0f - 1.0f;
    out[1] = wpos[1] / 136.0f + 1.0f;
    out[2] = 0.0f;
}

// Build a transform-mode state with the exact broken Patapon title matrices
// (col-major) recorded in .planning/research/patapon-visible-output.md.
static GeState make_broken_title_state() {
    GeState s;
    s.reset();
    s.vertex_type = 0x000183u;  // transform mode (bit 23 clear)
    const float world[12] = {
        1, 0, 0,   0, 0, 1,   0, -1, 0,   240, -136, 0};
    const float view[12] = {
        -0.006f, 0, 0,   0, -0.006f, 0,   0, 0, 1,   1.502f, -0.851f, 1.0f};
    const float proj[16] = {
        0.007f, 0, 0, 0,   0, 0.012f, 0, 0,
        0, 0, -0.002f, 0,  0, 0, -1.0f, 1.0f};
    for (int i = 0; i < 12; i++) { s.world_matrix[i] = world[i];
                                   s.view_matrix[i] = view[i]; }
    for (int i = 0; i < 16; i++) s.proj_matrix[i] = proj[i];
    return s;
}

// The broken title matrices must be detected as collapsed and routed to the
// fallback (which stamps the marker).
static void test_collapsed_mvp_routes_to_fallback() {
    g_fallback_fired = false;
    ge_vertex_set_degenerate_fallback(sentinel_fallback);
    GeState s = make_broken_title_state();
    // A title glyph quad spanning a non-trivial model extent.
    std::vector<DecodedVertex> verts = {
        make_pos_vertex(0.0f, 0.0f, 0.0f),
        make_pos_vertex(40.0f, 0.0f, 0.0f),
        make_pos_vertex(40.0f, 24.0f, 0.0f),
        make_pos_vertex(0.0f, 24.0f, 0.0f),
    };
    ge_transform_vertices(verts, s);
    tests_run++;
    if (!g_fallback_fired) {
        std::fprintf(stderr,
            "FAIL: collapsed MVP did not route to fallback\n");
        failures++;
    }
    ge_vertex_set_degenerate_fallback(nullptr);
}

// A healthy, full-screen ortho MVP must NOT be flagged as collapsed: the
// fallback must stay silent and the real transform path must run.
static void test_healthy_mvp_no_fallback() {
    g_fallback_fired = false;
    ge_vertex_set_degenerate_fallback(sentinel_fallback);
    GeState s;
    s.reset();
    s.vertex_type = 0x000183u;
    // Identity world & view; ortho proj mapping x in [0,480]/y in [0,272]
    // to NDC [-1,1] (col-major 4x4). m0 = 2/480, m5 = 2/272.
    s.world_matrix[0] = 1; s.world_matrix[4] = 1; s.world_matrix[8] = 1;
    s.view_matrix[0] = 1; s.view_matrix[4] = 1; s.view_matrix[8] = 1;
    s.proj_matrix[0]  = 2.0f / 480.0f;
    s.proj_matrix[5]  = 2.0f / 272.0f;
    s.proj_matrix[10] = -1.0f;
    s.proj_matrix[12] = -1.0f;  // x offset
    s.proj_matrix[13] = -1.0f;  // y offset
    s.proj_matrix[15] = 1.0f;
    std::vector<DecodedVertex> verts = {
        make_pos_vertex(0.0f, 0.0f, 0.0f),
        make_pos_vertex(480.0f, 0.0f, 0.0f),
        make_pos_vertex(480.0f, 272.0f, 0.0f),
        make_pos_vertex(0.0f, 272.0f, 0.0f),
    };
    ge_transform_vertices(verts, s);
    tests_run++;
    if (g_fallback_fired) {
        std::fprintf(stderr,
            "FAIL: healthy MVP wrongly routed to degenerate fallback\n");
        failures++;
    }
    ge_vertex_set_degenerate_fallback(nullptr);
}

// A legitimately tiny prim (small model extent) under the SAME broken matrices
// must NOT trigger the collapse detector: its small NDC image is consistent
// with its small input, so we cannot prove the MVP is broken from it. This
// guards against over-triggering on genuinely small geometry.
static void test_tiny_prim_no_overtrigger() {
    g_fallback_fired = false;
    ge_vertex_set_degenerate_fallback(sentinel_fallback);
    GeState s = make_broken_title_state();
    // Sub-unit model spread: below MODEL_SPREAD_MIN.
    std::vector<DecodedVertex> verts = {
        make_pos_vertex(0.0f, 0.0f, 0.0f),
        make_pos_vertex(0.2f, 0.2f, 0.0f),
    };
    ge_transform_vertices(verts, s);
    tests_run++;
    if (g_fallback_fired) {
        std::fprintf(stderr,
            "FAIL: tiny prim over-triggered the collapse detector\n");
        failures++;
    }
    ge_vertex_set_degenerate_fallback(nullptr);
}

// ---- Rectangle expansion (clip-space corners) ----
//
// ge_expand_rectangle divides both corners by their clip w and emits the
// 6 vertices (two triangles) of the screen-aligned quad with w = 1,
// preserving the historical corner mixing (z from v0 except the pure-v1
// corner; uv components cross-matched the same way).

// Both corners in front (w > 0): positions are divided by w.
static void test_rect_expand_divides_by_w() {
    DecodedVertex v0 = make_pos_vertex(-2.0f, -2.0f, -2.0f);
    DecodedVertex v1 = make_pos_vertex(2.0f, 2.0f, -4.0f);
    v0.w = 2.0f;
    v1.w = 4.0f;
    // Divided corners: a = (-1, -1, -1), b = (0.5, 0.5, -1)
    DecodedVertex out[6];
    tests_run++;
    if (!ge_expand_rectangle(v0, v1, out)) {
        std::fprintf(stderr,
            "FAIL: rect expand dropped a w > 0 rectangle\n");
        failures++;
        return;
    }
    // Triangle 1: a, c1 = (b.x, a.y, a.z), b
    ASSERT_NEAR(out[0].pos[0], -1.0f, "rect: out0 x = -2/2");
    ASSERT_NEAR(out[0].pos[1], -1.0f, "rect: out0 y = -2/2");
    ASSERT_NEAR(out[0].pos[2], -1.0f, "rect: out0 z = -2/2");
    ASSERT_NEAR(out[1].pos[0], 0.5f, "rect: out1 x = 2/4");
    ASSERT_NEAR(out[1].pos[1], -1.0f, "rect: out1 y = v0.y/w0");
    ASSERT_NEAR(out[1].pos[2], -1.0f, "rect: out1 z = v0.z/w0");
    ASSERT_NEAR(out[2].pos[0], 0.5f, "rect: out2 x = 2/4");
    ASSERT_NEAR(out[2].pos[1], 0.5f, "rect: out2 y = 2/4");
    ASSERT_NEAR(out[2].pos[2], -1.0f, "rect: out2 z = -4/4");
    // Triangle 2: a, b, c3 = (a.x, b.y, a.z)
    ASSERT_NEAR(out[3].pos[0], -1.0f, "rect: out3 == out0 x");
    ASSERT_NEAR(out[3].pos[1], -1.0f, "rect: out3 == out0 y");
    ASSERT_NEAR(out[4].pos[0], 0.5f, "rect: out4 == out2 x");
    ASSERT_NEAR(out[4].pos[1], 0.5f, "rect: out4 == out2 y");
    ASSERT_NEAR(out[5].pos[0], -1.0f, "rect: out5 x = v0.x/w0");
    ASSERT_NEAR(out[5].pos[1], 0.5f, "rect: out5 y = v1.y/w1");
    ASSERT_NEAR(out[5].pos[2], -1.0f, "rect: out5 z = v0.z/w0");
    for (int k = 0; k < 6; k++)
        ASSERT_NEAR(out[k].w, 1.0f, "rect: expanded vertex w == 1");
}

// A corner with w <= 0 (behind / on the eye plane) drops the rectangle.
static void test_rect_expand_drops_nonpositive_w() {
    DecodedVertex v0 = make_pos_vertex(-2.0f, -2.0f, -2.0f);
    DecodedVertex v1 = make_pos_vertex(2.0f, 2.0f, -4.0f);
    DecodedVertex out[6];

    tests_run++;
    v0.w = -1.0f;  // first corner behind the eye
    v1.w = 4.0f;
    if (ge_expand_rectangle(v0, v1, out)) {
        std::fprintf(stderr,
            "FAIL: rect expand kept a w < 0 corner\n");
        failures++;
    }

    tests_run++;
    v0.w = 2.0f;
    v1.w = 0.0f;   // second corner on the eye plane
    if (ge_expand_rectangle(v0, v1, out)) {
        std::fprintf(stderr,
            "FAIL: rect expand kept a w == 0 corner\n");
        failures++;
    }
}

// Colorless vertices take the material ambient RGB + MATERIALALPHA
// (PPSSPP gstate.getMaterialAmbientRGBA()), not opaque white: the Patapon 3
// dialog boxes are filled with colorless quads tinted cream this way.
static void test_colorless_vertex_uses_material_ambient() {
    const size_t kRam = 0x08000000u;
    uint8_t* ram = static_cast<uint8_t*>(std::calloc(kRam, 1));
    const uint32_t vaddr = 0x08800000u;
    const float pos[3] = {10.0f, 20.0f, 0.0f};
    std::memcpy(ram + (vaddr & 0x07FFFFFFu), pos, sizeof(pos));
    GeState s;
    s.reset();
    s.vertex_type = 0x00800180u;  // through, float position, no color/uv
    s.vertex_addr = vaddr;
    s.material_ambient = 0x00EBF5FFu;  // B=0xEB G=0xF5 R=0xFF
    s.material_alpha = 0x80u;
    std::vector<DecodedVertex> out;
    ge_decode_vertices(ram, s, 0 /* points */, 1, out);
    ASSERT_NEAR(out.size(), 1, "one vertex decoded");
    if (!out.empty()) {
        ASSERT_NEAR(out[0].color[0], 0xFF, "R from material ambient");
        ASSERT_NEAR(out[0].color[1], 0xF5, "G from material ambient");
        ASSERT_NEAR(out[0].color[2], 0xEB, "B from material ambient");
        ASSERT_NEAR(out[0].color[3], 0x80, "A from MATERIALALPHA");
    }
    std::free(ram);
}

// Vertex layout (PPSSPP VertexDecoder::SetVertexType): components in the order
// weights, texcoord, color, normal, position; each aligned to its own size;
// the total rounded up to the largest alignment (color 8888 counts as 4).
static void test_vertex_stride_alignment() {
    ASSERT_NEAR(ge_vertex_stride(0x0080011Cu), 12, "col8888 + pos16 (Patapon 3 dialog box)");
    ASSERT_NEAR(ge_vertex_stride(0x00000081u), 5, "tc8 + pos8");
    ASSERT_NEAR(ge_vertex_stride(0x00000112u), 12, "tc16 + col565 + pos16");
    ASSERT_NEAR(ge_vertex_stride(0x00004380u), 16, "2 u8 weights + pos float");
    ASSERT_NEAR(ge_vertex_stride(0x0000009Cu), 8, "col8888 + pos8");
    ASSERT_NEAR(ge_vertex_stride(0x000000A0u), 6, "nrm8 + pos8");
    ASSERT_NEAR(ge_vertex_stride(0x0000019Fu), 24, "tcfloat + col8888 + posfloat");
    ASSERT_NEAR(ge_vertex_stride(0x00000183u), 20, "tcfloat + posfloat");
}

// The decoder reads every vertex at the aligned stride: the second vertex of
// a col8888 + pos16 strip starts 12 bytes in, not 10.
static void test_decode_uses_aligned_stride() {
    const size_t kRam = 0x08000000u;
    uint8_t* ram = static_cast<uint8_t*>(std::calloc(kRam, 1));
    const uint32_t vaddr = 0x08800000u;
    const uint8_t verts[24] = {
        0xFF, 0xED, 0xD2, 0xFF, 0x97, 0x00, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xED, 0xD2, 0xFF, 0x97, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(ram + (vaddr & 0x07FFFFFFu), verts, sizeof(verts));
    GeState s;
    s.reset();
    s.vertex_type = 0x0080011Cu;
    s.vertex_addr = vaddr;
    std::vector<DecodedVertex> out;
    ge_decode_vertices(ram, s, 4, 2, out);
    ASSERT_NEAR(out.size(), 2, "two vertices");
    if (out.size() == 2) {
        ASSERT_NEAR(out[1].pos[0], 151.0f, "v1 x");
        ASSERT_NEAR(out[1].pos[1], 40.0f, "v1 y");
        ASSERT_NEAR(out[1].color[1], 0xED, "v1 green");
    }
    std::free(ram);
}

// GE VADDR/IADDR/JUMP targets are relative (PPSSPP getRelativeAddress):
// offsetAddr (OFFSETADDR / ORIGIN) + ((BASE & 0x0F0000) << 8 | data).
static void test_relative_address_adds_offset() {
    ASSERT_NEAR(ge_relative_address(0x00090000u, 0, 0x0042A0u), 0x090042A0u,
                "base supplies bits 24-27");
    ASSERT_NEAR(ge_relative_address(0, 0x094C5A00u, 0xA0u), 0x094C5AA0u,
                "offset (ORIGIN) is added");
    ASSERT_NEAR(ge_relative_address(0x00F90000u, 0, 0x1u), 0x09000001u,
                "only BASE bits 16-19 extend the address");
    ASSERT_NEAR(ge_relative_address(0x00080000u, 0x08000000u, 0x10u),
                0x00000010u, "sum wraps to 28 bits");
}

// PPSSPP vertex range culling (VertexShaderGenerator.cpp): a transformed
// vertex whose screen position (after the viewport) leaves [0, 4096) in x or
// y, or whose w < -1, kills its primitives -- unless it is z-clipped
// (z < -w), in which case clipping, not culling, applies.
static void test_range_culling() {
    GeState s{};
    s.viewport_x_scale = 240.0f; s.viewport_y_scale = -136.0f;
    s.viewport_x_center = 2048.0f; s.viewport_y_center = 2048.0f;
    const float on_screen[4] = {0.5f, -0.5f, 0.2f, 1.0f};
    const float far_right[4] = {9.0f, 0.0f, 0.2f, 1.0f};    // x = 2048 + 2160 > 4096
    const float far_up[4] = {0.0f, -16.0f, 0.2f, 1.0f};     // y = 2048 + 2176 > 4096
    const float z_clipped[4] = {9.0f, 0.0f, -2.0f, 1.0f};   // z < -w: clipped, not culled
    const float w_behind[4] = {0.0f, 0.0f, 5.0f, -2.0f};    // w < -1, z >= -w
    tests_run++; if (ge_vertex_range_culled(s, on_screen)) { failures++; std::fprintf(stderr, "FAIL: on-screen vertex culled\n"); }
    tests_run++; if (!ge_vertex_range_culled(s, far_right)) { failures++; std::fprintf(stderr, "FAIL: x >= 4096 not culled\n"); }
    tests_run++; if (!ge_vertex_range_culled(s, far_up)) { failures++; std::fprintf(stderr, "FAIL: y >= 4096 not culled\n"); }
    tests_run++; if (ge_vertex_range_culled(s, z_clipped)) { failures++; std::fprintf(stderr, "FAIL: z-clipped vertex culled\n"); }
    tests_run++; if (!ge_vertex_range_culled(s, w_behind)) { failures++; std::fprintf(stderr, "FAIL: w < -1 not culled\n"); }
}

// GE pixel masks (MASKRGB/MASKALPHA, 1 = keep) -> GL channel write enables,
// PPSSPP ConvertMaskState: 0x00 writes, 0xFF keeps, partial >= 128 writes.
static void test_color_write_mask() {
    bool w[4];
    ge_color_write_mask(0x000000, 0x00, w);
    tests_run++; if (!(w[0] && w[1] && w[2] && w[3])) { failures++; std::fprintf(stderr, "FAIL: no mask writes all" "\n"); }
    ge_color_write_mask(0xFFFFFF, 0x00, w);
    tests_run++; if (w[0] || w[1] || w[2] || !w[3]) { failures++; std::fprintf(stderr, "FAIL: RGB masked, alpha written" "\n"); }
    ge_color_write_mask(0x00FF00, 0xFF, w);
    tests_run++; if (!w[0] || w[1] || !w[2] || w[3]) { failures++; std::fprintf(stderr, "FAIL: G and A masked" "\n"); }
    ge_color_write_mask(0x0000F0, 0x00, w);  // R partially masked: 0x0F writable < 128
    tests_run++; if (w[0] || !w[1]) { failures++; std::fprintf(stderr, "FAIL: partial mask heuristic" "\n"); }
}

static void test_blend_setup() {
    GeBlendSetup b = ge_blend_setup(0x0032u, 0, 0);
    tests_run++; if (b.src != GL_SRC_ALPHA) { failures++; std::fprintf(stderr, "FAIL: 0x0032 src\n"); }
    tests_run++; if (b.dst != GL_ONE_MINUS_SRC_ALPHA) { failures++; std::fprintf(stderr, "FAIL: 0x0032 dst\n"); }
    tests_run++; if (b.equation != GL_FUNC_ADD) { failures++; std::fprintf(stderr, "FAIL: 0x0032 equation\n"); }
    tests_run++; if (b.set_constant) { failures++; std::fprintf(stderr, "FAIL: 0x0032 set_constant\n"); }

    b = ge_blend_setup(0x00A2u, 0, 0xFFFFFFu);
    tests_run++; if (b.dst != GL_ONE) { failures++; std::fprintf(stderr, "FAIL: FIXB white dst\n"); }
    tests_run++; if (b.set_constant) { failures++; std::fprintf(stderr, "FAIL: FIXB white set_constant\n"); }

    b = ge_blend_setup(0x00A2u, 0, 0x804020u);
    tests_run++; if (b.dst != GL_CONSTANT_COLOR) { failures++; std::fprintf(stderr, "FAIL: FIXB const dst\n"); }
    tests_run++; if (!b.set_constant) { failures++; std::fprintf(stderr, "FAIL: FIXB const set_constant\n"); }
    tests_run++; if (!(std::fabs(b.constant[0] - (0x20 / 255.0f)) < 1e-6f)) { failures++; std::fprintf(stderr, "FAIL: FIXB const R\n"); }
    tests_run++; if (!(std::fabs(b.constant[1] - (0x40 / 255.0f)) < 1e-6f)) { failures++; std::fprintf(stderr, "FAIL: FIXB const G\n"); }
    tests_run++; if (!(std::fabs(b.constant[2] - (0x80 / 255.0f)) < 1e-6f)) { failures++; std::fprintf(stderr, "FAIL: FIXB const B\n"); }

    b = ge_blend_setup(0x000Au, 0x102030u, 0);
    tests_run++; if (b.src != GL_SRC1_COLOR) { failures++; std::fprintf(stderr, "FAIL: FIXA src\n"); }
    tests_run++; if (b.src1_rgb != 0) { failures++; std::fprintf(stderr, "FAIL: FIXA src1_rgb\n"); }
    tests_run++; if (!(std::fabs(b.fix_a[0] - (0x30 / 255.0f)) < 1e-6f)) { failures++; std::fprintf(stderr, "FAIL: FIXA R\n"); }
    tests_run++; if (!(std::fabs(b.fix_a[1] - (0x20 / 255.0f)) < 1e-6f)) { failures++; std::fprintf(stderr, "FAIL: FIXA G\n"); }
    tests_run++; if (!(std::fabs(b.fix_a[2] - (0x10 / 255.0f)) < 1e-6f)) { failures++; std::fprintf(stderr, "FAIL: FIXA B\n"); }

    b = ge_blend_setup(0x000Au, 0x000000u, 0);
    tests_run++; if (b.src != GL_ZERO) { failures++; std::fprintf(stderr, "FAIL: FIXA zero src\n"); }
    b = ge_blend_setup(0x000Fu, 0xFFFFFFu, 0);
    tests_run++; if (b.src != GL_ONE) { failures++; std::fprintf(stderr, "FAIL: s=15 FIXA white src\n"); }

    b = ge_blend_setup(0x0076u, 0, 0);
    tests_run++; if (b.src != GL_SRC1_COLOR) { failures++; std::fprintf(stderr, "FAIL: double src factor\n"); }
    tests_run++; if (b.src1_rgb != 1) { failures++; std::fprintf(stderr, "FAIL: double src1_rgb\n"); }
    tests_run++; if (b.dst != GL_SRC1_ALPHA) { failures++; std::fprintf(stderr, "FAIL: double dst factor\n"); }
    tests_run++; if (b.src1_a != 2) { failures++; std::fprintf(stderr, "FAIL: double src1_a\n"); }

    b = ge_blend_setup(0x0232u, 0, 0);
    tests_run++; if (b.equation != GL_FUNC_REVERSE_SUBTRACT) { failures++; std::fprintf(stderr, "FAIL: reverse subtract\n"); }
    b = ge_blend_setup(0x0332u, 0, 0);
    tests_run++; if (b.equation != GL_MIN) { failures++; std::fprintf(stderr, "FAIL: min equation\n"); }
    b = ge_blend_setup(0x0532u, 0, 0);
    tests_run++; if (b.equation != GL_FUNC_ADD) { failures++; std::fprintf(stderr, "FAIL: absdiff equation\n"); }
}

int main() {
    test_color_write_mask();
    test_blend_setup();
    test_range_culling();
    test_through_textured_normalizes();
    test_through_untextured_unchanged();
    test_through_scale_offset_then_normalize();
    test_transform_front_keeps_clip();
    test_transform_behind_keeps_negative_w();
    test_through_w_is_one();
    test_collapsed_mvp_routes_to_fallback();
    test_healthy_mvp_no_fallback();
    test_tiny_prim_no_overtrigger();
    test_rect_expand_divides_by_w();
    test_rect_expand_drops_nonpositive_w();
    test_colorless_vertex_uses_material_ambient();
    test_vertex_stride_alignment();
    test_decode_uses_aligned_stride();
    test_relative_address_adds_offset();

    if (failures == 0) {
        std::printf("test_ge_vertex: %d/%d PASS\n", tests_run, tests_run);
        return 0;
    }
    std::fprintf(stderr, "test_ge_vertex: %d/%d FAIL\n",
        tests_run - failures, tests_run);
    return 1;
}
