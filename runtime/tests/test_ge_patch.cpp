// Unit tests for GE BEZIER patch tessellation (psp_ge_patch.cpp). Patapon 3
// draws its weather wind gusts as 14x4-control-point bezier surfaces (4
// cubic patches along u, 1 along v, PATCHDIVISION 16x16, vertex colour
// alpha fading the ends); the runtime ignored GE_CMD_BEZIER, so the wind was
// never drawn. Oracle: PPSSPP GPU/Common/SplineCommon.cpp (Bernstein basis,
// num_patches = (count - 1) / 3, every vertex attribute interpolated with the
// same weights).

#include "psp_ge_patch.h"
#include "psp_ge_vertex.h"
#include "psp_ge_constants.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
static int tests_run = 0;

#define CHECK(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s\n", msg); \
            failures++; \
        } \
    } while (0)

#define CHECK_NEAR(a, b, msg) CHECK(std::fabs((a) - (b)) < 1e-3f, msg)

static DecodedVertex cp(float x, float y, uint8_t alpha) {
    DecodedVertex v{};
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = 0.0f;
    v.color[0] = v.color[1] = v.color[2] = 255; v.color[3] = alpha;
    v.has_color = true;
    return v;
}

// A flat 4x4 grid on the plane: the bezier surface interpolates its corner
// control points exactly and stays on the plane.
static void test_single_patch_corners_and_count() {
    std::vector<DecodedVertex> ctrl;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) ctrl.push_back(cp(i * 10.0f, j * 10.0f, 255));
    std::vector<DecodedVertex> out;
    int prim = -1;
    CHECK(ge_tessellate_bezier(ctrl, 4, 4, 2, 2, 0, false, prim, out), "single patch tessellates");
    CHECK(prim == GE_PRIM_TRIANGLES, "patch primitive 0 draws triangles");
    CHECK(out.size() == 2u * 2u * 6u, "2x2 divisions -> 8 triangles");
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (const DecodedVertex& v : out) {
        minx = std::fmin(minx, v.pos[0]); maxx = std::fmax(maxx, v.pos[0]);
        miny = std::fmin(miny, v.pos[1]); maxy = std::fmax(maxy, v.pos[1]);
    }
    CHECK_NEAR(minx, 0.0f, "surface reaches the u=0 edge");
    CHECK_NEAR(maxx, 30.0f, "surface reaches the u=1 edge");
    CHECK_NEAR(maxy, 30.0f, "surface reaches the v=1 edge");
}

// Patapon 3's wind: 14 x 4 control points -> (14-1)/3 = 4 patches along u
// (the 14th column is unused), colour alpha interpolated with the same
// Bernstein weights (centre of a patch whose control alphas are 0,0,255,255
// along u -> 128).
static void test_wind_strip_patches_and_alpha() {
    std::vector<DecodedVertex> ctrl;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 14; i++) ctrl.push_back(cp(i * 1.0f, j * 1.0f, (i % 3 == 0 || i % 3 == 1) && i < 2 ? 0 : 255));
    std::vector<DecodedVertex> out;
    int prim = -1;
    CHECK(ge_tessellate_bezier(ctrl, 14, 4, 16, 16, 0, false, prim, out), "wind strip tessellates");
    CHECK(out.size() == 4u * 16u * 16u * 6u, "4 patches x 16x16 divisions");
    float maxx = -1e9f;
    for (const DecodedVertex& v : out) maxx = std::fmax(maxx, v.pos[0]);
    CHECK_NEAR(maxx, 12.0f, "last patch ends on control column 12");
    // First vertex of the first patch is control point 0 (alpha 0).
    CHECK(out[0].color[3] == 0, "u=0,v=0 keeps the control alpha");
    CHECK(out[0].has_color, "colour carried through");
}

static void test_points_and_bad_counts() {
    std::vector<DecodedVertex> ctrl(16, cp(0, 0, 255));
    std::vector<DecodedVertex> out;
    int prim = -1;
    CHECK(ge_tessellate_bezier(ctrl, 4, 4, 3, 3, 2, false, prim, out), "points patch");
    CHECK(prim == GE_PRIM_POINTS && out.size() == 16u, "patch primitive 2: (3+1)^2 points");
    CHECK(!ge_tessellate_bezier(ctrl, 3, 4, 3, 3, 0, false, prim, out), "fewer than 4 control points along u");
    CHECK(!ge_tessellate_bezier(ctrl, 4, 5, 3, 3, 0, false, prim, out), "more control points than supplied");
}

int main() {
    test_single_patch_corners_and_count();
    test_wind_strip_patches_and_alpha();
    test_points_and_bad_counts();
    if (failures == 0) {
        std::printf("test_ge_patch: %d/%d PASS\n", tests_run, tests_run);
        return 0;
    }
    std::fprintf(stderr, "test_ge_patch: %d/%d FAIL\n", tests_run - failures, tests_run);
    return 1;
}
