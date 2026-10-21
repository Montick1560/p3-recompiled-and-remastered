// psp_anim_blend.h: blending two evaluated poses (smooth animation at 60 fps).
#include "psp_anim_blend.h"

#include <cmath>
#include <cstring>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)
static bool near(float x, float y) { return std::fabs(x - y) < 1e-5f; }

int main() {
    {   // lerp and keep
        float a[3] = {0.0f, 10.0f, 5.0f};
        const float b[3] = {2.0f, 20.0f, 7.0f};
        const uint8_t k[3] = {kBlendLerp, kBlendLerp, kBlendKeep};
        psp_blend_pose(a, b, k, 3, 0.25f);
        CHECK(near(a[0], 0.5f) && near(a[1], 12.5f), "lerp by t");
        CHECK(near(a[2], 5.0f), "keep leaves a");
    }
    {   // t = 0 is exactly a, t = 1 is exactly b
        float a[2] = {1.0f, -3.0f};
        const float b[2] = {4.0f, 9.0f};
        const uint8_t k[2] = {kBlendLerp, kBlendLerp};
        psp_blend_pose(a, b, k, 2, 0.0f);
        CHECK(a[0] == 1.0f && a[1] == -3.0f, "t=0 keeps a bit-exact");
        psp_blend_pose(a, b, k, 2, 1.0f);
        CHECK(near(a[0], 4.0f) && near(a[1], 9.0f), "t=1 reaches b");
    }
    {   // quaternion: identity to 90 deg about z, halfway = 45 deg, unit length
        const float s = std::sqrt(0.5f);
        float a[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float b[4] = {0.0f, 0.0f, s, s};
        const uint8_t k[4] = {kBlendQuat, kBlendKeep, kBlendKeep, kBlendKeep};
        psp_blend_pose(a, b, k, 4, 0.5f);
        const float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
        CHECK(near(len, 1.0f), "nlerp result is unit length");
        CHECK(near(a[2], std::sin(0.3926991f)) && near(a[3], std::cos(0.3926991f)), "halfway is 45 deg");
    }
    {   // quaternion sign flip: b = -a (same rotation) must stay a, not collapse to zero
        float a[4] = {0.1f, 0.2f, 0.3f, 0.9273618f};
        const float b[4] = {-0.1f, -0.2f, -0.3f, -0.9273618f};
        const uint8_t k[4] = {kBlendQuat, kBlendKeep, kBlendKeep, kBlendKeep};
        psp_blend_pose(a, b, k, 4, 0.5f);
        CHECK(near(a[0], 0.1f) && near(a[3], 0.9273618f), "short arc across a sign flip");
    }
    {   // alpha byte: top byte of a packed word lerped (rounded), low 24 bits kept from a
        auto word = [](uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; };
        auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        float a[1] = {word(0xAFFFFFFFu)};
        const float b[1] = {word(0x9F123456u)};
        const uint8_t k[1] = {kBlendAlphaByte};
        psp_blend_pose(a, b, k, 1, 0.5f);
        CHECK(bits(a[0]) == 0xA7FFFFFFu, "alpha 0xAF -> 0x9F halfway is 0xA7, low bits from a");
        float c[1] = {word(0x00FFFFFFu)};
        const float d[1] = {word(0xFFFFFFFFu)};
        psp_blend_pose(c, d, k, 1, 0.25f);
        CHECK(bits(c[0]) == 0x40FFFFFFu, "alpha 0 -> 255 at a quarter rounds to 0x40");
        float e[1] = {word(0x80FFFFFFu)};
        const float f[1] = {word(0x80FFFFFFu)};
        psp_blend_pose(e, f, k, 1, 0.7f);
        CHECK(bits(e[0]) == 0x80FFFFFFu, "equal alpha stays bit-exact");
    }
    {   // UV offset: lerp, but a wrap (jump of more than half a texture) holds a
        float a[2] = {0.2f, 0.9f};
        const float b[2] = {0.3f, 0.05f};
        const uint8_t k[2] = {kBlendUv, kBlendUv};
        psp_blend_pose(a, b, k, 2, 0.5f);
        CHECK(near(a[0], 0.25f), "uv offset lerps");
        CHECK(near(a[1], 0.9f), "uv offset wrap holds a");
    }
    // next frame selection
    CHECK(psp_anim_next_frame(3, 0.0f, 10.0f, false) == 4, "inside: whole + 1");
    CHECK(psp_anim_next_frame(10, 0.0f, 10.0f, false) == -1, "one-shot at its end: no blend");
    CHECK(psp_anim_next_frame(9, 0.0f, 9.5f, false) == -1, "one-shot: next beyond end");
    CHECK(psp_anim_next_frame(10, 2.0f, 10.0f, true) == 2, "loop wraps to its start");
    CHECK(psp_anim_next_frame(9, 2.0f, 10.0f, true) == 10, "loop: last frame still inside");
    if (failures == 0) std::printf("anim_blend: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
