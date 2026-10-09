#include "psp_vfpu.h"
#include "recomp.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>

// ---------------------------------------------------------------------------
// NaN-aware clamp helper
// ---------------------------------------------------------------------------

static inline float nanclamp_local(float f, float lo, float hi) {
    float r = (f <= lo) ? lo : f;
    r = (r >= hi) ? hi : r;
    return r;
}

// ---------------------------------------------------------------------------
// Sort operations (PPSSPP Int_Vsrt1..4)
//
// S is read from vs and swizzled normally; T is read from *vs* too (not vt)
// with its swizzle forced (yxwz for vsrt1/vsrt3, wzyx for vsrt2/vsrt4) while
// the user's abs/neg/const bits still apply. The four lanes are then
// min/max'd with the hardware ordering (vfpu_min/vfpu_max: ties return the
// second argument as written below).
// ---------------------------------------------------------------------------

static void vfpu_vsrt_common(recomp_context* ctx, uint8_t vd, uint8_t vs,
                             uint8_t size, int variant) {
    float s[4] = {}, t[4] = {}, d[4] = {};
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    vfpu_read_vector(t, size, vs, ctx->vfpu);
    const uint32_t forced = (variant == 1 || variant == 3)
        ? vfpu_pfx_swizzle(1, 0, 3, 2)   // yxwz
        : vfpu_pfx_swizzle(3, 2, 1, 0);  // wzyx
    vfpu_apply_prefix_st(t, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], 0xFFu, forced), size);
    switch (variant) {
    case 1:  // (min01, max01, min23, max23)
        d[0] = vfpu_min(t[0], s[0]);
        d[1] = vfpu_max(s[1], t[1]);
        d[2] = vfpu_min(t[2], s[2]);
        d[3] = vfpu_max(s[3], t[3]);
        break;
    case 2:  // (min03, min12, max12, max03)
        d[0] = vfpu_min(t[0], s[0]);
        d[1] = vfpu_min(t[1], s[1]);
        d[2] = vfpu_max(s[2], t[2]);
        d[3] = vfpu_max(s[3], t[3]);
        break;
    case 3:  // (max01, min01, max23, min23)
        d[0] = vfpu_max(s[0], t[0]);
        d[1] = vfpu_min(t[1], s[1]);
        d[2] = vfpu_max(s[2], t[2]);
        d[3] = vfpu_min(t[3], s[3]);
        break;
    default: // 4: (max03, max12, min12, min03)
        d[0] = vfpu_max(s[0], t[0]);
        d[1] = vfpu_max(s[1], t[1]);
        d[2] = vfpu_min(t[2], s[2]);
        d[3] = vfpu_min(t[3], s[3]);
        break;
    }
    vfpu_retain_invalid_swizzle(d, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                                ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vsrt1(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vsrt_common(ctx, vd, vs, size, 1);
}

void vfpu_vsrt2(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vsrt_common(ctx, vd, vs, size, 2);
}

void vfpu_vsrt3(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vsrt_common(ctx, vd, vs, size, 3);
}

void vfpu_vsrt4(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vsrt_common(ctx, vd, vs, size, 4);
}

// ---------------------------------------------------------------------------
// Butterfly operations (PPSSPP Int_Vbfy)
//
// S gets forced negate flags (vbfy1: y,w; vbfy2: z,w) and T is read from vs
// with a forced swizzle (vbfy1: yxwz; vbfy2: zwxy); d = s + t.
// ---------------------------------------------------------------------------

static void vfpu_vbfy_common(recomp_context* ctx, uint8_t vd, uint8_t vs,
                             uint8_t size, bool second) {
    float s[4] = {}, t[4] = {}, d[4] = {};
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], 0,
        second ? vfpu_pfx_negate(0, 0, 1, 1) : vfpu_pfx_negate(0, 1, 0, 1)),
        size);
    vfpu_apply_prefix_st(t, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], VFPU_PFX_ANY_SWIZZLE,
        second ? vfpu_pfx_swizzle(2, 3, 0, 1) : vfpu_pfx_swizzle(1, 0, 3, 2)),
        size);
    for (int i = 0; i < 4; i++) d[i] = s[i] + t[i];
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vbfy1(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vbfy_common(ctx, vd, vs, size, false);
}

void vfpu_vbfy2(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_vbfy_common(ctx, vd, vs, size, true);
}

// ---------------------------------------------------------------------------
// Reduction operations
// ---------------------------------------------------------------------------

void vfpu_vocp(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    // d = 1 - s (PPSSPP Int_Vocp: the S prefix is forced to negate and T is
    // forced to the constant 1, so a user negate flag cannot flip it to 1+s).
    // NaN inputs yield a positive NaN.
    float s[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    const uint32_t sprefix_no_negate =
        ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX] & ~(0xFu << 16);
    vfpu_apply_prefix_st(s, sprefix_no_negate, size);
    float d[4];
    for (int i = 0; i < size; i++) {
        d[i] = std::isnan(s[i]) ? std::fabs(s[i]) : 1.0f - s[i];
    }
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vsocp(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    // PPSSPP Int_Vsocp: a source of n lanes yields 2n outputs (single ->
    // pair, anything wider -> quad). S is forced to swizzle (x,x,y,y) with
    // negate on the even lanes, T to the constants (1,0,1,0), and the result
    // is clamp(t + s, 0, 1): (1-x, x, 1-y, y). Only the write mask of D
    // applies.
    float s[4] = {}, t[4] = {}, d[4] = {};
    const int out = (size == 1) ? 2 : 4;
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
        VFPU_PFX_ANY_SWIZZLE | vfpu_pfx_negate(1, 1, 1, 1),
        vfpu_pfx_swizzle(0, 0, 1, 1) | vfpu_pfx_negate(1, 0, 1, 0)), out);
    vfpu_apply_prefix_st(t, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], VFPU_PFX_ANY_SWIZZLE,
        vfpu_pfx_constants(VFPU_CONST_ONE, VFPU_CONST_ZERO,
                           VFPU_CONST_ONE, VFPU_CONST_ZERO)), out);
    for (int i = 0; i < out; i++) {
        d[i] = nanclamp_local(t[i] + s[i], 0.0f, 1.0f);
    }
    vfpu_write_vector(d, out, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vfad(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    // PPSSPP Int_Vfad: sum of s[i] * t[i] over four lanes, with T forced to
    // the constant 1 (abs/neg still apply to it); lanes past size are zero.
    float s[4] = {}, t[4] = {};
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], 4);
    vfpu_apply_prefix_st(t, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], VFPU_PFX_ANY_SWIZZLE,
        vfpu_pfx_constants(VFPU_CONST_ONE, VFPU_CONST_ONE,
                           VFPU_CONST_ONE, VFPU_CONST_ONE)), 4);
    float d = 0.0f;
    for (int i = 0; i < 4; i++) d += s[i] * t[i];
    vfpu_apply_prefix_d(&d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], 1);
    vfpu_write_vector(&d, 1, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vavg(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    // PPSSPP Int_Vavg: sum of s[i] * c over four lanes where c is 0, 1/2,
    // 1/3, 1/4 for single/pair/triple/quad (so vavg.s is 0). T keeps only its
    // negate bits.
    float s[4] = {}, t[4] = {};
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], 4);
    int c = (size == 1) ? VFPU_CONST_ZERO
          : (size == 2) ? VFPU_CONST_HALF
          : (size == 3) ? VFPU_CONST_THIRD : VFPU_CONST_FOURTH;
    vfpu_apply_prefix_st(t, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
        VFPU_PFX_ANY_SWIZZLE | vfpu_pfx_abs(1, 1, 1, 1),
        vfpu_pfx_constants(c, c, c, c)), 4);
    float d = 0.0f;
    for (int i = 0; i < 4; i++) d += s[i] * t[i];
    vfpu_apply_prefix_d(&d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], 1);
    vfpu_write_vector(&d, 1, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

// ---------------------------------------------------------------------------
// Random number operations
// ---------------------------------------------------------------------------

// Simple VFPU random state (not cryptographically secure)
static uint32_t vfpu_rng_state = 0x3F800001u;

static uint32_t vfpu_rng_next() {
    // xorshift32 PRNG
    vfpu_rng_state ^= vfpu_rng_state << 13;
    vfpu_rng_state ^= vfpu_rng_state >> 17;
    vfpu_rng_state ^= vfpu_rng_state << 5;
    return vfpu_rng_state;
}

void vfpu_vrnds(recomp_context* ctx, uint8_t*,
                uint8_t vs) {
    float s;
    vfpu_read_vector(&s, 1, vs, ctx->vfpu);
    uint32_t u;
    std::memcpy(&u, &s, 4);
    if (u != 0) vfpu_rng_state = u;
    vfpu_eat_prefixes(ctx);
}

void vfpu_vrndi(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t size) {
    float d[4];
    for (int i = 0; i < size; i++) {
        uint32_t r = vfpu_rng_next();
        std::memcpy(&d[i], &r, 4);
    }
    vfpu_write_vector(d, size, vd, ctx->vfpu, 0);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vrndf1(recomp_context* ctx, uint8_t*,
                 uint8_t vd, uint8_t size) {
    float d[4];
    for (int i = 0; i < size; i++) {
        // Random float in [1.0, 2.0)
        uint32_t r = vfpu_rng_next();
        r = (r & 0x007FFFFFu) | 0x3F800000u;
        std::memcpy(&d[i], &r, 4);
    }
    vfpu_write_vector(d, size, vd, ctx->vfpu, 0);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vrndf2(recomp_context* ctx, uint8_t*,
                 uint8_t vd, uint8_t size) {
    float d[4];
    for (int i = 0; i < size; i++) {
        // Random float in [2.0, 4.0)
        uint32_t r = vfpu_rng_next();
        r = (r & 0x007FFFFFu) | 0x40000000u;
        std::memcpy(&d[i], &r, 4);
    }
    vfpu_write_vector(d, size, vd, ctx->vfpu, 0);
    vfpu_eat_prefixes(ctx);
}

// ---------------------------------------------------------------------------
// Wrap by negative (vwbn)
// ---------------------------------------------------------------------------

void vfpu_vwbn(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t imm8,
               uint8_t size) {
    // PPSSPP Int_Vwbn: lane 0 gets its exponent replaced by imm8 (bits 23:16
    // of the opcode) with the mantissa shifted by the exponent difference
    // (mod 16); the other lanes are copied unchanged.
    float sf[4] = {}, df[4] = {};
    uint32_t s[4], d[4];
    vfpu_read_vector(sf, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    std::memcpy(s, sf, sizeof(s));
    const uint32_t exp = imm8;
    uint32_t sigbit = s[0] & 0x80000000u;
    uint32_t prev_exp = (s[0] & 0x7F800000u) >> 23;
    uint32_t mantissa = (s[0] & 0x007FFFFFu) | 0x00800000u;
    if (prev_exp != 0xFF && prev_exp != 0) {
        if (exp > prev_exp) {
            int shift = static_cast<int8_t>((exp - prev_exp) & 0xF);
            mantissa >>= shift;
        } else {
            int shift = static_cast<int8_t>((prev_exp - exp) & 0xF);
            mantissa <<= shift;
        }
        d[0] = sigbit | (mantissa & 0x007FFFFFu) | (exp << 23);
    } else {
        d[0] = s[0] | (exp << 23);
    }
    for (int i = 1; i < size; i++) d[i] = s[i];
    std::memcpy(df, d, sizeof(df));
    vfpu_retain_invalid_swizzle(df, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                                ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    vfpu_apply_prefix_d(df, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(df, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

// ---------------------------------------------------------------------------
// vsgn, vsbn, vsbz, vlgb, vt4444/vt5551/vt5650 (previously decoded to a no-op)
// ---------------------------------------------------------------------------

void vfpu_vsgn(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    // PPSSPP Int_Vsgn: d[i] = sign(s[i] - t[i]) with T forced to the constant
    // 0 (abs/neg still apply: compare against +-3). A denormal difference is
    // zero, NaN keeps its sign. Lanes of S past size swizzle to 0.
    float s[4] = {}, t[4] = {}, d[4] = {};
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(t, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], VFPU_PFX_ANY_SWIZZLE,
        vfpu_pfx_constants(VFPU_CONST_ZERO, VFPU_CONST_ZERO,
                           VFPU_CONST_ZERO, VFPU_CONST_ZERO)), size);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], 4);
    for (int i = 0; i < size; i++) {
        float diff = s[i] - t[i];
        uint32_t val;
        std::memcpy(&val, &diff, 4);
        if ((val & 0x7F800000u) == 0) d[i] = 0.0f;
        else if ((val >> 31) == 0)    d[i] = 1.0f;
        else                          d[i] = -1.0f;
    }
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vsbn(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size) {
    // PPSSPP Int_Vsbn: lane 0's exponent bits are replaced by
    // (u8)(127 + int_bits(t[0])); other lanes are copied. Zero/denormal/inf/NaN
    // lane 0 pass through.
    float sf[4] = {}, tf[4] = {}, df[4] = {};
    vfpu_read_vector(sf, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    vfpu_read_vector(tf, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(tf, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    uint32_t s[4], d[4];
    int32_t t0;
    std::memcpy(s, sf, sizeof(s));
    std::memcpy(&t0, &tf[0], 4);
    const uint8_t exp = static_cast<uint8_t>(127 + t0);
    uint32_t prev = s[0] & 0x7F800000u;
    if (prev != 0 && prev != 0x7F800000u) {
        d[0] = (s[0] & ~0x7F800000u) | (static_cast<uint32_t>(exp) << 23);
    } else {
        d[0] = s[0];
    }
    for (int i = 1; i < size; i++) d[i] = s[i];
    std::memcpy(df, d, sizeof(df));
    vfpu_apply_prefix_d(df, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(df, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vsbz(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    // PPSSPP Int_Vsbz: lane 0 becomes 2^0 * (1.mantissa) (exponent forced to
    // 127); NaN and zero/denormal pass through; other lanes copied.
    float sf[4] = {}, df[4] = {};
    vfpu_read_vector(sf, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    uint32_t s[4], d[4];
    std::memcpy(s, sf, sizeof(s));
    if (std::isnan(sf[0]) || (s[0] & 0x7F800000u) == 0) {
        d[0] = s[0];
    } else {
        d[0] = (127u << 23) | (s[0] & 0x007FFFFFu);
    }
    for (int i = 1; i < size; i++) d[i] = s[i];
    std::memcpy(df, d, sizeof(df));
    vfpu_apply_prefix_d(df, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(df, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vlgb(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    // PPSSPP Int_Vlgb: lane 0 becomes its unbiased exponent as a float
    // (inf/NaN unchanged, zero/denormal -inf); other lanes copied.
    float sf[4] = {}, df[4] = {};
    vfpu_read_vector(sf, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    uint32_t s[4];
    std::memcpy(s, sf, sizeof(s));
    int exp = (s[0] & 0x7F800000u) >> 23;
    if (exp == 0xFF) {
        df[0] = sf[0];
    } else if (exp == 0) {
        df[0] = -HUGE_VALF;
    } else {
        df[0] = static_cast<float>(exp - 127);
    }
    for (int i = 1; i < size; i++) df[i] = sf[i];
    vfpu_retain_invalid_swizzle(df, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                                ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    vfpu_apply_prefix_d(df, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(df, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

// PPSSPP Int_ColorConv: always reads a quad of 32-bit RGBA words from vs and
// packs each to 16 bits; the result is a pair of words (a single word for a
// .s op). mode 1 = 4444, 2 = 5551, 3 = 5650.
static void vfpu_colorconv(recomp_context* ctx, uint8_t vd, uint8_t vs,
                           uint8_t size, int mode) {
    float sf[4] = {};
    vfpu_read_vector(sf, 4, vs, ctx->vfpu);
    vfpu_apply_prefix_st(sf, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], 4);
    uint32_t s[4];
    std::memcpy(s, sf, sizeof(s));
    uint16_t colors[4];
    for (int i = 0; i < 4; i++) {
        uint32_t in = s[i];
        uint32_t col = 0;
        switch (mode) {
        case 1: {
            uint32_t a = ((in >> 24) & 0xFF) >> 4;
            uint32_t b = ((in >> 16) & 0xFF) >> 4;
            uint32_t g = ((in >> 8) & 0xFF) >> 4;
            uint32_t r = (in & 0xFF) >> 4;
            col = (a << 12) | (b << 8) | (g << 4) | r;
            break;
        }
        case 2: {
            uint32_t a = ((in >> 24) & 0xFF) >> 7;
            uint32_t b = ((in >> 16) & 0xFF) >> 3;
            uint32_t g = ((in >> 8) & 0xFF) >> 3;
            uint32_t r = (in & 0xFF) >> 3;
            col = (a << 15) | (b << 10) | (g << 5) | r;
            break;
        }
        default: {
            uint32_t b = ((in >> 16) & 0xFF) >> 3;
            uint32_t g = ((in >> 8) & 0xFF) >> 2;
            uint32_t r = (in & 0xFF) >> 3;
            col = (b << 11) | (g << 5) | r;
            break;
        }
        }
        colors[i] = static_cast<uint16_t>(col);
    }
    uint32_t ov[2] = {
        static_cast<uint32_t>(colors[0]) | (static_cast<uint32_t>(colors[1]) << 16),
        static_cast<uint32_t>(colors[2]) | (static_cast<uint32_t>(colors[3]) << 16)};
    float of[2];
    std::memcpy(of, ov, sizeof(of));
    vfpu_apply_prefix_d(of, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], 2);
    vfpu_write_vector(of, size == 1 ? 1 : 2, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vt4444(recomp_context* ctx, uint8_t*,
                 uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_colorconv(ctx, vd, vs, size, 1);
}

void vfpu_vt5551(recomp_context* ctx, uint8_t*,
                 uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_colorconv(ctx, vd, vs, size, 2);
}

void vfpu_vt5650(recomp_context* ctx, uint8_t*,
                 uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_colorconv(ctx, vd, vs, size, 3);
}
