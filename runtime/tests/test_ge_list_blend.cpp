// psp_ge_list_blend.h: blending two recorded GE command lists of the same shape.
#include "psp_ge_list_blend.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static uint32_t f24(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return bits >> 8;
}
static float from24(uint32_t cmd) {
    const uint32_t bits = (cmd & 0x00FFFFFFu) << 8;
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}
static uint32_t cmd(uint32_t op, uint32_t data) { return op << 24 | (data & 0x00FFFFFFu); }

int main() {
    {   // world matrix data lerped, alpha lerped, texture offset and everything else kept from a
        const uint32_t a[] = {cmd(0x4A, f24(0.125f)), cmd(0x58, 0x40), cmd(0x3A, 0), cmd(0x3B, f24(1.0f)),
                              cmd(0x3B, f24(-2.0f)), cmd(0x10, 0x090000), cmd(0x0A, 0x123400), cmd(0x0B, 0)};
        const uint32_t b[] = {cmd(0x4A, f24(0.25f)), cmd(0x58, 0xC0), cmd(0x3A, 0), cmd(0x3B, f24(3.0f)),
                              cmd(0x3B, f24(2.0f)), cmd(0x10, 0x090000), cmd(0x0A, 0x123400), cmd(0x0B, 0)};
        uint32_t out[8] = {};
        const size_t n = psp_ge_blend_lists(a, b, 8, 0.5f, out);
        CHECK(n == 8, "copies up to and including RET");
        CHECK(out[0] == a[0], "texture offset kept from a (atlas cells are not blended)");
        CHECK(out[1] == cmd(0x58, 0x80), "material alpha lerped");
        CHECK(out[2] == a[2], "world matrix number kept");
        CHECK(std::fabs(from24(out[3]) - 2.0f) < 1e-3f && std::fabs(from24(out[4]) - 0.0f) < 1e-3f,
              "world matrix data lerped");
        CHECK(out[5] == a[5] && out[6] == a[6] && out[7] == a[7], "base, call, ret copied");
    }
    {   // t = 0 gives a bit-exact
        const uint32_t a[] = {cmd(0x58, 0x10), cmd(0x3B, f24(0.3f)), cmd(0x0B, 0)};
        const uint32_t b[] = {cmd(0x58, 0xF0), cmd(0x3B, f24(0.9f)), cmd(0x0B, 0)};
        uint32_t out[3] = {};
        CHECK(psp_ge_blend_lists(a, b, 3, 0.0f, out) == 3 && std::memcmp(out, a, sizeof a) == 0, "t=0 is a");
    }
    {   // bone matrix data is lerped like world data
        const uint32_t a[] = {cmd(0x2B, f24(0.0f)), cmd(0x0B, 0)};
        const uint32_t b[] = {cmd(0x2B, f24(4.0f)), cmd(0x0B, 0)};
        uint32_t out[2] = {};
        CHECK(psp_ge_blend_lists(a, b, 2, 0.25f, out) == 2 && std::fabs(from24(out[0]) - 1.0f) < 1e-3f,
              "bone matrix data lerped");
    }
    {   // different shapes (an opcode differs) are refused
        const uint32_t a[] = {cmd(0x3B, f24(1.0f)), cmd(0x0B, 0)};
        const uint32_t b[] = {cmd(0x58, 0x10), cmd(0x0B, 0)};
        uint32_t out[2] = {};
        CHECK(psp_ge_blend_lists(a, b, 2, 0.5f, out) == 0, "opcode mismatch refused");
    }
    {   // no RET within max_words is refused (a list that jumps or ends elsewhere)
        const uint32_t a[] = {cmd(0x3B, f24(1.0f)), cmd(0x3B, f24(1.0f))};
        uint32_t out[2] = {};
        CHECK(psp_ge_blend_lists(a, a, 2, 0.5f, out) == 0, "missing RET refused");
    }
    {   // lists that change control flow are refused: JUMP, CALL is fine only after BASE, ORIGIN, END
        const uint32_t j[] = {cmd(0x08, 0x000100), cmd(0x0B, 0)};
        const uint32_t o[] = {cmd(0x14, 0), cmd(0x0B, 0)};
        uint32_t out[2] = {};
        CHECK(psp_ge_blend_lists(j, j, 2, 0.5f, out) == 0, "JUMP refused");
        CHECK(psp_ge_blend_lists(o, o, 2, 0.5f, out) == 0, "ORIGIN refused");
    }
    // A whole world matrix (0x3A + 12 x 0x3B): basis vectors x, y, z (3 words each), then translation.
    auto matrix = [](uint32_t* l, const float m[12]) {
        l[0] = cmd(0x3A, 0);
        for (int i = 0; i < 12; i++) l[1 + i] = cmd(0x3B, f24(m[i]));
        l[13] = cmd(0x0B, 0);
    };
    {   // an axis that flips (180 degrees, the Wooyari Hero Mode fire wheel from one key to the next) would
        // lerp through zero: the list is not blended
        const float ma[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 5, 6, 7};
        const float mb[12] = {1, 0, 0, 0, -1, 0, 0, 0, 1, 5, 6, 7};
        uint32_t a[14], b[14], out[14] = {};
        matrix(a, ma);
        matrix(b, mb);
        CHECK(psp_ge_blend_lists(a, b, 14, 0.5f, out) == 0, "flipped axis refused");
    }
    {   // a 90 degree turn about z keeps the axes' length (a plain lerp draws the model at 71 %)
        const float ma[12] = {2, 0, 0, 0, 2, 0, 0, 0, 2, 1, 1, 1};
        const float mb[12] = {0, 2, 0, -2, 0, 0, 0, 0, 2, 3, 1, 1};
        uint32_t a[14], b[14], out[14] = {};
        matrix(a, ma);
        matrix(b, mb);
        CHECK(psp_ge_blend_lists(a, b, 14, 0.5f, out) == 14, "turned matrix blended");
        const float x0 = from24(out[1]), x1 = from24(out[2]), y0 = from24(out[4]), y1 = from24(out[5]);
        CHECK(std::fabs(std::sqrt(x0 * x0 + x1 * x1) - 2.0f) < 1e-3f && std::fabs(x0 - x1) < 1e-3f,
              "x axis halfway round at full length");
        CHECK(std::fabs(std::sqrt(y0 * y0 + y1 * y1) - 2.0f) < 1e-3f && std::fabs(y0 + y1) < 1e-3f,
              "y axis halfway round at full length");
        CHECK(std::fabs(from24(out[10]) - 2.0f) < 1e-3f, "translation lerped");
    }
    {   // a scale change still lerps the length (2 -> 4 is 3 halfway)
        const float ma[12] = {2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0};
        const float mb[12] = {4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0};
        uint32_t a[14], b[14], out[14] = {};
        matrix(a, ma);
        matrix(b, mb);
        CHECK(psp_ge_blend_lists(a, b, 14, 0.5f, out) == 14 && std::fabs(from24(out[1]) - 3.0f) < 1e-3f,
              "scale lerped");
    }
    if (failures == 0) std::printf("ge_list_blend: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
