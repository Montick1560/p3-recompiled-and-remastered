// test_ge_fog.cpp -- GE fog helpers against PPSSPP (ShaderUniforms.cpp:255 UpdateFogCoef,
// VertexShaderGenerator.cpp:904 fog depth, FragmentShaderGenerator.cpp:866 mix, ShaderId.cpp:326).
#include "psp_ge_fog.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void float24_decodes_top_bits() {
    CHECK(near(ge_float24(0x3F8000), 1.0f));     // 0x3F800000
    CHECK(near(ge_float24(0xC00000), -2.0f));    // 0xC0000000
    CHECK(near(ge_float24(0x000000), 0.0f));
    CHECK(near(ge_float24(0xFF3F8000), 1.0f));   // command byte ignored
}
static void fog_coef_nonfinite_clamped() {
    CHECK(near(ge_fog_coef(0x7F8000), 65535.0f));    // +inf
    CHECK(near(ge_fog_coef(0xFF8000), -65535.0f));   // -inf
    CHECK(std::isfinite(ge_fog_coef(0x7FC000)));     // NaN -> finite
    CHECK(near(ge_fog_coef(0x3F8000), 1.0f));        // finite passes through
}
static void fog_inactive_in_through_mode() {
    CHECK(ge_fog_active(true, false));
    CHECK(!ge_fog_active(true, true));
    CHECK(!ge_fog_active(false, false));
}
static void fog_factor_matches_ppsspp() {
    CHECK(near(ge_fog_factor(-10.0f, 20.0f, 0.1f), 1.0f));   // (-10+20)*0.1
    CHECK(near(ge_fog_factor(-10.0f, 10.0f, 0.5f), 0.0f));   // at the fog end: full fog
    CHECK(ge_fog_factor(-50.0f, 10.0f, 0.5f) < 0.0f);         // past it: clamps to full fog
}
static void fog_mix_white() {
    float white[3]; ge_fog_color_rgb(0xFFFFFF, white);
    float rgb[3] = {0.2f, 0.4f, 0.6f};
    ge_fog_mix(white, -3.0f, rgb);                  // full fog
    CHECK(near(rgb[0], 1.0f) && near(rgb[1], 1.0f) && near(rgb[2], 1.0f));
    float keep[3] = {0.2f, 0.4f, 0.6f};
    ge_fog_mix(white, 2.0f, keep);                  // no fog
    CHECK(near(keep[0], 0.2f) && near(keep[1], 0.4f) && near(keep[2], 0.6f));
    float half[3] = {0.0f, 0.0f, 0.0f};
    ge_fog_mix(white, 0.25f, half);                 // 75 % fog colour
    CHECK(near(half[0], 0.75f));
}
static void fog_color_byte_order() {
    float c[3]; ge_fog_color_rgb(0x0000FF, c);      // R in byte 0
    CHECK(near(c[0], 1.0f) && near(c[1], 0.0f) && near(c[2], 0.0f));
    ge_fog_color_rgb(0xFF0000, c);                  // B in byte 2
    CHECK(near(c[0], 0.0f) && near(c[2], 1.0f));
}

int main() {
    float24_decodes_top_bits();
    fog_coef_nonfinite_clamped();
    fog_inactive_in_through_mode();
    fog_factor_matches_ppsspp();
    fog_mix_white();
    fog_color_byte_order();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_ge_fog: all passed\n");
    return 0;
}
