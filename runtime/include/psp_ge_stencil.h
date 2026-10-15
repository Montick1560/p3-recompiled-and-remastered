#pragma once
// GE stencil registers -> host stencil state. A port of PPSSPP's
// ConvertStencilFuncState / ConvertStencilFunc5551 (tools/ppsspp-src
// GPU/Common/GPUStateUtils.cpp) and of its clear-mode stencil setup
// (GPU/GLES/StateMappingGLES.cpp, GPU/Common/SoftwareTransformCommon.cpp).
//
// On the PSP the stencil is the alpha channel of the color buffer (8888: 8
// bits, 4444: 4, 5551: 1, 565: none). The host keeps it in a real stencil
// buffer; 5551 stores any set bit as 0xFF, so its tests are rewritten for a
// buffer that only holds 0x00 or 0xFF. Pure (no GL): GE codes in, GE codes
// out; the draw path maps them to GL (tests/test_ge_stencil.cpp).
#include <cstdint>

#include "psp_ge_constants.h"

struct GeStencilInput {
    bool test_enable = false;   // GE_CMD_STENCILTESTENABLE
    uint32_t stencil_test = 0;  // GE_CMD_STENCILTEST: func | ref << 8 | mask << 16
    uint32_t stencil_op = 0;    // GE_CMD_STENCILOP: sfail | zfail << 8 | zpass << 16
    uint32_t mask_rgb = 0;      // GE_CMD_MASKRGB (set bits are not written)
    uint32_t mask_alpha = 0;    // GE_CMD_MASKALPHA: the stencil write mask, inverted
    bool depth_test = false;
    bool depth_write = false;
    uint32_t fb_format = GE_FORMAT_8888;  // GE_CMD_FRAMEBUFPIXFORMAT
};

struct GeStencilState {
    bool enabled = false;
    uint8_t func = GE_COMP_ALWAYS;  // GeCompFunc: passes when (ref & mask) func (buffer & mask)
    uint8_t ref = 0;
    uint8_t test_mask = 0xFF;
    uint8_t sfail = GE_STENCILOP_KEEP;  // GeStencilOp
    uint8_t zfail = GE_STENCILOP_KEEP;
    uint8_t zpass = GE_STENCILOP_KEEP;
    uint8_t write_mask = 0;  // also valid when disabled: clears use it
};

namespace ge_stencil_detail {

inline uint8_t mask_5551(uint8_t write_mask) {
    return write_mask >= 0x80 ? 0xFF : 0x00;
}

// ConvertStencilFunc5551: any non-zero value in the buffer counts as 0xFF.
inline void func_5551(GeStencilState& s) {
    const bool uses_ref = s.sfail == GE_STENCILOP_REPLACE || s.zfail == GE_STENCILOP_REPLACE
                          || s.zpass == GE_STENCILOP_REPLACE;
    const uint8_t masked_ref = s.ref & s.test_mask;
    const uint8_t used_ref = (s.ref & 0x80) != 0 ? 0xFF : 0x00;
    const bool ref_is_full = masked_ref == (0xFF & s.test_mask) && s.test_mask != 0;

    // Only rewrite when the ref is unused, or REPLACE would write the same bit.
    auto rewrite_func = [&](uint8_t func, uint8_t ref) {
        if (!uses_ref || used_ref == ref) {
            s.func = func;
            s.ref = ref;
            s.test_mask = 0xFF;
        }
    };
    auto rewrite_ref = [&](bool always) {
        s.func = always ? GE_COMP_ALWAYS : GE_COMP_NEVER;
        s.ref = uses_ref ? used_ref : 0xFF;
        s.test_mask = 0xFF;
    };

    switch (s.func) {
    case GE_COMP_NEVER:
    case GE_COMP_ALWAYS:
        rewrite_ref(s.func == GE_COMP_ALWAYS);
        break;
    case GE_COMP_EQUAL:
        if (masked_ref == 0) rewrite_func(GE_COMP_EQUAL, 0);
        else if (ref_is_full) rewrite_func(GE_COMP_NOTEQUAL, 0);
        else rewrite_ref(false);
        break;
    case GE_COMP_NOTEQUAL:
        if (masked_ref == 0) rewrite_func(GE_COMP_NOTEQUAL, 0);
        else if (ref_is_full) rewrite_func(GE_COMP_EQUAL, 0);
        else rewrite_ref(true);
        break;
    case GE_COMP_LESS:
        if (ref_is_full) rewrite_ref(false);
        else rewrite_func(GE_COMP_NOTEQUAL, 0);
        break;
    case GE_COMP_LEQUAL:
        if (masked_ref == 0) rewrite_ref(true);
        else rewrite_func(GE_COMP_NOTEQUAL, 0);
        break;
    case GE_COMP_GREATER:
        if (masked_ref > 0) rewrite_func(GE_COMP_EQUAL, 0);
        else rewrite_ref(false);
        break;
    case GE_COMP_GEQUAL:
        if (ref_is_full) rewrite_ref(true);
        else rewrite_func(GE_COMP_EQUAL, 0);
        break;
    }

    auto rewrite_ops = [&](uint8_t from, uint8_t to) {
        if (s.sfail == from) s.sfail = to;
        if (s.zfail == from) s.zfail = to;
        if (s.zpass == from) s.zpass = to;
    };
    // One bit: decrement always zeroes.
    rewrite_ops(GE_STENCILOP_DECR, GE_STENCILOP_ZERO);
    if (s.func == GE_COMP_NOTEQUAL && s.ref == 0 && s.test_mask != 0) {
        rewrite_ops(GE_STENCILOP_INVERT, GE_STENCILOP_ZERO);  // != 0 can only become 0
    }
    if (s.func == GE_COMP_EQUAL && s.ref == 0 && s.test_mask != 0) {
        rewrite_ops(GE_STENCILOP_INCR, GE_STENCILOP_INVERT);  // == 0 increments to set
    }
    if (!uses_ref && s.ref == 0xFF) {
        rewrite_ops(GE_STENCILOP_INCR, GE_STENCILOP_REPLACE);
    }
}

}  // namespace ge_stencil_detail

// ConvertStencilFuncState.
inline GeStencilState ge_stencil_state(const GeStencilInput& in) {
    GeStencilState s;
    // The PSP mask names the bits not to write; it applies to clears too.
    s.write_mask = static_cast<uint8_t>(~in.mask_alpha & 0xFF);
    s.enabled = in.test_enable;
    if (!s.enabled) {
        if (in.fb_format == GE_FORMAT_5551) s.write_mask = ge_stencil_detail::mask_5551(s.write_mask);
        return s;
    }

    s.sfail = static_cast<uint8_t>(in.stencil_op & 7);
    s.zfail = static_cast<uint8_t>((in.stencil_op >> 8) & 7);
    s.zpass = static_cast<uint8_t>((in.stencil_op >> 16) & 7);
    s.func = static_cast<uint8_t>(in.stencil_test & 7);
    s.ref = static_cast<uint8_t>((in.stencil_test >> 8) & 0xFF);
    s.test_mask = static_cast<uint8_t>((in.stencil_test >> 16) & 0xFF);

    // Writing only stencil (usually REPLACE x3) with RGB masked and no depth
    // write: the test only gates the stencil write, so it always passes.
    if ((s.sfail == s.zfail || !in.depth_test) && s.sfail == s.zpass) {
        if ((in.mask_rgb & 0xFFFFFF) == 0xFFFFFF && (!in.depth_test || !in.depth_write)) {
            s.func = GE_COMP_ALWAYS;
        }
    }

    switch (in.fb_format) {
    case GE_FORMAT_565:
        s.write_mask = 0;
        break;
    case GE_FORMAT_5551:
        s.write_mask = ge_stencil_detail::mask_5551(s.write_mask);
        ge_stencil_detail::func_5551(s);
        break;
    default:
        break;
    }
    return s;
}

struct GeStencilClear {
    bool clear = false;   // CLEARMODE alpha flag: write the stencil
    uint8_t value = 0;    // the clear's vertex alpha
    uint8_t write_mask = 0;
};

// Clear mode: the alpha/stencil flag (CLEARMODE bit 9, clear_flags bit 1)
// writes the clear vertex alpha (PPSSPP takes the first rectangle's second
// vertex) through the same write mask as draws.
inline GeStencilClear ge_stencil_clear(const GeStencilInput& in, uint32_t clear_flags,
                                       uint8_t vertex_alpha) {
    GeStencilClear c;
    c.clear = (clear_flags & 2) != 0;
    c.value = vertex_alpha;
    c.write_mask = ge_stencil_state(in).write_mask;
    return c;
}
