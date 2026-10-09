#pragma once

#include <glad/glad.h>
#include <cstdint>

struct GeBlendSetup {
    GLenum src;          // GL source factor (RGB and alpha use the same one)
    GLenum dst;          // GL destination factor
    GLenum equation;     // GL blend equation
    bool   set_constant; // true -> glBlendColor(constant[0..2], 1)
    float  constant[3];  // fix_b as 0..1 floats (R,G,B)
    int    src1_rgb;     // what the shader writes to frag_color1.rgb: 0 = fix_a color, 1 = clamp(2*a), 2 = clamp(2*(1-a))
    int    src1_a;       // what the shader writes to frag_color1.a:   0 = 1.0,         1 = clamp(2*a), 2 = clamp(2*(1-a))
    float  fix_a[3];     // fix_a as 0..1 floats (R,G,B)
};

inline GeBlendSetup ge_blend_setup(uint32_t blend_mode, uint32_t fix_a, uint32_t fix_b) {
    GeBlendSetup b{};
    b.set_constant = false;
    b.src1_rgb = 0;
    b.src1_a = 0;
    b.fix_a[0] = static_cast<float>(fix_a & 0xFFu) / 255.0f;
    b.fix_a[1] = static_cast<float>((fix_a >> 8) & 0xFFu) / 255.0f;
    b.fix_a[2] = static_cast<float>((fix_a >> 16) & 0xFFu) / 255.0f;
    b.constant[0] = static_cast<float>(fix_b & 0xFFu) / 255.0f;
    b.constant[1] = static_cast<float>((fix_b >> 8) & 0xFFu) / 255.0f;
    b.constant[2] = static_cast<float>((fix_b >> 16) & 0xFFu) / 255.0f;

    int s = static_cast<int>(blend_mode & 0xFu);
    int d = static_cast<int>((blend_mode >> 4) & 0xFu);
    int e = static_cast<int>((blend_mode >> 8) & 0x7u);
    if (s > 10) s = 10;
    if (d > 10) d = 10;

    switch (s) {
    case 0: b.src = GL_DST_COLOR; break;
    case 1: b.src = GL_ONE_MINUS_DST_COLOR; break;
    case 2: b.src = GL_SRC_ALPHA; break;
    case 3: b.src = GL_ONE_MINUS_SRC_ALPHA; break;
    case 4: b.src = GL_DST_ALPHA; break;
    case 5: b.src = GL_ONE_MINUS_DST_ALPHA; break;
    case 6: b.src = GL_SRC1_COLOR; b.src1_rgb = 1; break;
    case 7: b.src = GL_SRC1_COLOR; b.src1_rgb = 2; break;
    case 8: b.src = GL_DST_ALPHA; break;
    case 9: b.src = GL_ONE_MINUS_DST_ALPHA; break;
    case 10:
        if (fix_a == 0x000000u) {
            b.src = GL_ZERO;
        } else if (fix_a == 0xFFFFFFu) {
            b.src = GL_ONE;
        } else {
            b.src = GL_SRC1_COLOR;
            b.src1_rgb = 0;
        }
        break;
    default:
        b.src = GL_SRC_ALPHA;
        break;
    }

    switch (d) {
    case 0: b.dst = GL_SRC_COLOR; break;
    case 1: b.dst = GL_ONE_MINUS_SRC_COLOR; break;
    case 2: b.dst = GL_SRC_ALPHA; break;
    case 3: b.dst = GL_ONE_MINUS_SRC_ALPHA; break;
    case 4: b.dst = GL_DST_ALPHA; break;
    case 5: b.dst = GL_ONE_MINUS_DST_ALPHA; break;
    case 6: b.dst = GL_SRC1_ALPHA; b.src1_a = 1; break;
    case 7: b.dst = GL_SRC1_ALPHA; b.src1_a = 2; break;
    case 8: b.dst = GL_DST_ALPHA; break;
    case 9: b.dst = GL_ONE_MINUS_DST_ALPHA; break;
    case 10:
        if (fix_b == 0x000000u) {
            b.dst = GL_ZERO;
        } else if (fix_b == 0xFFFFFFu) {
            b.dst = GL_ONE;
        } else {
            b.dst = GL_CONSTANT_COLOR;
            b.set_constant = true;
        }
        break;
    default:
        b.dst = GL_ONE_MINUS_SRC_ALPHA;
        break;
    }

    switch (e) {
    case 0: b.equation = GL_FUNC_ADD; break;
    case 1: b.equation = GL_FUNC_SUBTRACT; break;
    case 2: b.equation = GL_FUNC_REVERSE_SUBTRACT; break;
    case 3: b.equation = GL_MIN; break;
    case 4: b.equation = GL_MAX; break;
    default: b.equation = GL_FUNC_ADD; break;
    }

    return b;
}
