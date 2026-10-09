#include "psp_vfpu.h"
#include "recomp.h"
#include <cmath>
#include <cstring>
#include <cstdint>

// All conversions follow PPSSPP InterpreterVFPU.cpp (Int_Vf2i, Int_Vi2f,
// Int_Vh2f, Int_Vf2h, Int_Vx2i, Int_Vi2x). Integer results travel through the
// float register file as raw bits, so the D prefix is "mask only" for the
// float-to-integer direction: saturation would reinterpret the bits as a
// float, and PPSSPP skips it there (ApplyPrefixD(..., onlyWriteMask=true));
// vfpu_write_vector still applies the write mask.

static inline float bits_to_float(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

static inline uint32_t float_to_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// ---------------------------------------------------------------------------
// Float-to-integer conversions
// ---------------------------------------------------------------------------

// mode: 0 = n (nearest even), 1 = z (toward zero), 2 = u (ceil), 3 = d (floor)
static void vfpu_vf2i_common(recomp_context* ctx, uint8_t vd, uint8_t vs,
                             uint8_t imm5, uint8_t size, int mode) {
    float s[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    const float mult = static_cast<float>(1ULL << (imm5 & 0x1F));
    float d[4];
    for (int i = 0; i < size; i++) {
        int32_t r;
        if (std::isnan(s[i])) {
            r = 0x7FFFFFFF;
        } else {
            double sv = s[i] * mult;  // float product, widened
            if (sv > static_cast<double>(0x7FFFFFFF)) {
                r = 0x7FFFFFFF;
            } else if (sv <= static_cast<double>(INT32_MIN)) {
                r = INT32_MIN;
            } else {
                switch (mode) {
                case 0:  r = static_cast<int32_t>(std::nearbyint(sv)); break;
                case 1:  r = s[i] >= 0 ? static_cast<int32_t>(std::floor(sv))
                                       : static_cast<int32_t>(std::ceil(sv));
                         break;
                case 2:  r = static_cast<int32_t>(std::ceil(sv)); break;
                default: r = static_cast<int32_t>(std::floor(sv)); break;
                }
            }
        }
        d[i] = bits_to_float(static_cast<uint32_t>(r));
    }
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vf2in(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t imm5,
                uint8_t size) {
    vfpu_vf2i_common(ctx, vd, vs, imm5, size, 0);
}

void vfpu_vf2iz(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t imm5,
                uint8_t size) {
    vfpu_vf2i_common(ctx, vd, vs, imm5, size, 1);
}

void vfpu_vf2iu(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t imm5,
                uint8_t size) {
    vfpu_vf2i_common(ctx, vd, vs, imm5, size, 2);
}

void vfpu_vf2id(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t imm5,
                uint8_t size) {
    vfpu_vf2i_common(ctx, vd, vs, imm5, size, 3);
}

// ---------------------------------------------------------------------------
// Integer-to-float conversion
// ---------------------------------------------------------------------------

void vfpu_vi2f(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t imm5,
               uint8_t size) {
    float s[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    // The S prefix (swizzle/abs/neg/const) applies to the raw bits.
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    const float mult = 1.0f / static_cast<float>(1ULL << (imm5 & 0x1F));
    float d[4];
    for (int i = 0; i < size; i++) {
        int32_t iv = static_cast<int32_t>(float_to_bits(s[i]));
        d[i] = static_cast<float>(iv) * mult;
    }
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

// ---------------------------------------------------------------------------
// vi2uc / vi2c / vi2us / vi2s (PPSSPP Int_Vi2x)
// ---------------------------------------------------------------------------

static void vfpu_vi2x_common(recomp_context* ctx, uint8_t vd, uint8_t vs,
                             uint8_t size, int variant) {
    float sf[4] = {};
    vfpu_read_vector(sf, size, vs, ctx->vfpu);
    // The swizzle always covers four lanes; lanes past size read as 0.
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], 4);
    int32_t s[4];
    std::memcpy(s, sf, sizeof(s));
    uint32_t d[2] = {0, 0};
    int out = 1;
    switch (variant) {
    case 0:  // vi2uc: clamp negatives to 0, take bits 30:23
        for (int i = 0; i < 4; i++) {
            int32_t v = s[i];
            if (v < 0) v = 0;
            v >>= 23;
            d[0] |= (static_cast<uint32_t>(v) & 0xFF) << (i * 8);
        }
        break;
    case 1:  // vi2c: take bits 31:24
        for (int i = 0; i < 4; i++) {
            d[0] |= (static_cast<uint32_t>(s[i]) >> 24) << (i * 8);
        }
        break;
    case 2: {  // vi2us: clamp negatives to 0, take bits 30:15 of word pairs
        int elems = (size + 1) / 2;
        for (int i = 0; i < elems; i++) {
            int32_t low = s[i * 2];
            int32_t high = s[i * 2 + 1];
            if (low < 0) low = 0;
            if (high < 0) high = 0;
            low >>= 15;
            high >>= 15;
            d[i] = static_cast<uint32_t>(low) |
                   (static_cast<uint32_t>(high) << 16);
        }
        out = (size >= 3) ? 2 : 1;
        break;
    }
    default: {  // vi2s: take the high halves of word pairs
        int elems = (size + 1) / 2;
        for (int i = 0; i < elems; i++) {
            uint32_t low = static_cast<uint32_t>(s[i * 2]) >> 16;
            uint32_t high = static_cast<uint32_t>(s[i * 2 + 1]) >> 16;
            d[i] = low | (high << 16);
        }
        out = (size >= 3) ? 2 : 1;
        break;
    }
    }
    float df[2];
    std::memcpy(df, d, sizeof(df));
    vfpu_apply_prefix_d(df, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], out);
    vfpu_write_vector(df, out, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vi2uc(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vi2x_common(ctx, vd, vs, size, 0);
}

void vfpu_vi2c(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vi2x_common(ctx, vd, vs, size, 1);
}

void vfpu_vi2us(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vi2x_common(ctx, vd, vs, size, 2);
}

void vfpu_vi2s(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vi2x_common(ctx, vd, vs, size, 3);
}

// ---------------------------------------------------------------------------
// vuc2i / vc2i / vus2i / vs2i (PPSSPP Int_Vx2i)
// ---------------------------------------------------------------------------

// variant: 0 = vuc2i, 1 = vc2i, 2 = vus2i, 3 = vs2i
static void vfpu_vx2i_common(recomp_context* ctx, uint8_t vd, uint8_t vs,
                             uint8_t size, int variant) {
    float sf[4];
    vfpu_read_vector(sf, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    uint32_t s[4] = {};
    std::memcpy(s, sf, sizeof(float) * size);
    uint32_t d[4] = {};
    int out = 4;
    switch (variant) {
    case 0: {  // vuc2i: replicate each byte into all four, then >> 1
        uint32_t value = s[0];
        for (int i = 0; i < 4; i++) {
            d[i] = ((value & 0xFF) * 0x01010101u) >> 1;
            value >>= 8;
        }
        break;
    }
    case 1:  // vc2i: byte i to the top of word i, no shift (signed)
        d[0] = (s[0] & 0xFF) << 24;
        d[1] = (s[0] & 0xFF00) << 16;
        d[2] = (s[0] & 0xFF0000) << 8;
        d[3] = (s[0] & 0xFF000000u);
        break;
    default: {  // vus2i (2) / vs2i (3)
        // Single: one word -> pair. Everything wider uses two words -> quad.
        int n = 1;
        out = 2;
        if (size >= 2) {
            n = 2;
            out = 4;
        }
        for (int i = 0; i < n; i++) {
            uint32_t value = s[i];
            if (variant == 2) {
                d[i * 2] = (value & 0xFFFF) << 15;
                d[i * 2 + 1] = (value & 0xFFFF0000u) >> 1;
            } else {
                d[i * 2] = (value & 0xFFFF) << 16;
                d[i * 2 + 1] = value & 0xFFFF0000u;
            }
        }
        break;
    }
    }
    float df[4];
    std::memcpy(df, d, sizeof(df));
    // Saturation does in fact apply here (PPSSPP applies the full D prefix).
    vfpu_apply_prefix_d(df, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], out);
    vfpu_write_vector(df, out, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vuc2i(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vx2i_common(ctx, vd, vs, size, 0);
}

void vfpu_vc2i(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vx2i_common(ctx, vd, vs, size, 1);
}

void vfpu_vus2i(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vx2i_common(ctx, vd, vs, size, 2);
}

void vfpu_vs2i(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vx2i_common(ctx, vd, vs, size, 3);
}

// ---------------------------------------------------------------------------
// Half-float conversion
// ---------------------------------------------------------------------------

void vfpu_vf2h(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    float sf[4] = {};
    vfpu_read_vector(sf, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], 4);
    uint32_t s[4];
    std::memcpy(s, sf, sizeof(s));
    uint32_t d[2];
    d[0] = vfpu_f2h_bits(s[0]) | (static_cast<uint32_t>(vfpu_f2h_bits(s[1])) << 16);
    d[1] = vfpu_f2h_bits(s[2]) | (static_cast<uint32_t>(vfpu_f2h_bits(s[3])) << 16);
    const int out = (size >= 3) ? 2 : 1;
    float df[2];
    std::memcpy(df, d, sizeof(df));
    vfpu_apply_prefix_d(df, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], out);
    vfpu_write_vector(df, out, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vh2f(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    // PPSSPP Int_Vh2f: a single source word gives 2 lanes; a pair or wider
    // reads two words and gives 4 lanes.
    float sf[4] = {};
    vfpu_read_vector(sf, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    uint32_t s[4] = {};
    std::memcpy(s, sf, sizeof(float) * size);
    uint32_t d[4];
    int out;
    d[0] = vfpu_h2f_bits(static_cast<uint16_t>(s[0] & 0xFFFF));
    d[1] = vfpu_h2f_bits(static_cast<uint16_t>(s[0] >> 16));
    if (size == 1) {
        out = 2;
    } else {
        out = 4;
        d[2] = vfpu_h2f_bits(static_cast<uint16_t>(s[1] & 0xFFFF));
        d[3] = vfpu_h2f_bits(static_cast<uint16_t>(s[1] >> 16));
    }
    float df[4];
    std::memcpy(df, d, sizeof(df));
    vfpu_apply_prefix_d(df, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], out);
    vfpu_write_vector(df, out, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}
