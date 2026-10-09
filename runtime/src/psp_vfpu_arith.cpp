#include "psp_vfpu.h"
#include "recomp.h"
#include <cmath>
#include <cstring>
#include <cstdint>

static inline bool is_nan_or_inf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return (u & 0x7F800000u) == 0x7F800000u;
}

// ---------------------------------------------------------------------------
// Binary arithmetic -- all follow the read-prefix-compute-prefix_d-write-eat
// pattern.
// ---------------------------------------------------------------------------

void vfpu_vadd(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         size);
    for (int i = 0; i < size; i++) d[i] = s[i] + t[i];
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vsub(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         size);
    for (int i = 0; i < size; i++) d[i] = s[i] - t[i];
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vmul(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         size);
    for (int i = 0; i < size; i++) d[i] = s[i] * t[i];
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vdiv(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    // PPSSPP Int_VecDo3 optype 7: the S/T prefixes apply only to the last
    // lane, an out of range swizzle there zeroes the result, and the D prefix
    // keeps only lane 0's mask/saturation (moved to the last lane).
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    const uint32_t sp = ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX];
    const uint32_t tp = ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX];
    vfpu_apply_prefix_st(&s[size - 1], sp, 1);
    vfpu_apply_prefix_st(&t[size - 1], tp, 1);
    for (int i = 0; i < size; i++) d[i] = s[i] / t[i];
    if (vfpu_last_lane_swizzle_invalid(sp) ||
        vfpu_last_lane_swizzle_invalid(tp)) {
        d[size - 1] = 0.0f;
    }
    const uint32_t dp =
        vfpu_last_lane_dprefix(ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_apply_prefix_d(d, dp, size);
    vfpu_write_vector(d, size, vd, ctx->vfpu, dp);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vmin(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    // PPSSPP Int_Vminmax: inf/NaN operands order by sign and magnitude as
    // integers (-NaN < -inf < finite < inf < NaN); finite operands use
    // std::min(t, s), so a tie (including -0 vs +0) returns t.
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    for (int i = 0; i < size; i++) {
        if (is_nan_or_inf(s[i]) || is_nan_or_inf(t[i])) {
            int32_t si, ti, r;
            std::memcpy(&si, &s[i], 4);
            std::memcpy(&ti, &t[i], 4);
            if (si < 0 && ti < 0) r = (ti > si) ? ti : si;  // both negative: flip
            else                  r = (ti < si) ? ti : si;
            std::memcpy(&d[i], &r, 4);
        } else {
            d[i] = (s[i] < t[i]) ? s[i] : t[i];
        }
    }
    vfpu_retain_invalid_swizzle(d, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                                ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vmax(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    // Same logic as vmin, reversed (std::max(t, s): a tie returns t).
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    for (int i = 0; i < size; i++) {
        if (is_nan_or_inf(s[i]) || is_nan_or_inf(t[i])) {
            int32_t si, ti, r;
            std::memcpy(&si, &s[i], 4);
            std::memcpy(&ti, &t[i], 4);
            if (si < 0 && ti < 0) r = (ti < si) ? ti : si;
            else                  r = (ti > si) ? ti : si;
            std::memcpy(&d[i], &r, 4);
        } else {
            d[i] = (t[i] < s[i]) ? s[i] : t[i];
        }
    }
    vfpu_retain_invalid_swizzle(d, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                                ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vscmp(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t vt,
                uint8_t size) {
    // PPSSPP Int_Vscmp: sign of s - t; a NaN difference falls back to
    // comparing the operands as sign/magnitude integers.
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    for (int i = 0; i < size; i++) {
        float a = s[i] - t[i];
        if (a != a) {
            uint32_t su, tu;
            std::memcpy(&su, &s[i], 4);
            std::memcpy(&tu, &t[i], 4);
            int32_t sm = static_cast<int32_t>(su & 0x7FFFFFFFu);
            int32_t tm = static_cast<int32_t>(tu & 0x7FFFFFFFu);
            int32_t b = ((su >> 31) ? -sm : sm) - ((tu >> 31) ? -tm : tm);
            d[i] = static_cast<float>((0 < b) - (b < 0));
        } else {
            d[i] = static_cast<float>((0.0f < a) - (a < 0.0f));
        }
    }
    vfpu_retain_invalid_swizzle(d, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                                ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vsge(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    // PPSSPP Int_Vsge/Int_Vslt: NaN on either side gives 0; saturation is
    // skipped (it cannot matter), the write mask still applies.
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    for (int i = 0; i < size; i++) {
        if (s[i] != s[i] || t[i] != t[i]) d[i] = 0.0f;
        else d[i] = (s[i] >= t[i]) ? 1.0f : 0.0f;
    }
    vfpu_retain_invalid_swizzle(d, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                                ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vslt(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    // PPSSPP Int_Vsge/Int_Vslt: NaN on either side gives 0; saturation is
    // skipped (it cannot matter), the write mask still applies.
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    for (int i = 0; i < size; i++) {
        if (s[i] != s[i] || t[i] != t[i]) d[i] = 0.0f;
        else d[i] = (s[i] < t[i]) ? 1.0f : 0.0f;
    }
    vfpu_retain_invalid_swizzle(d, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                                ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

// ---------------------------------------------------------------------------
// Reduction / special binary ops
// ---------------------------------------------------------------------------

void vfpu_vdot(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         size);
    float sum = 0.0f;
    for (int i = 0; i < size; i++) sum += s[i] * t[i];
    d[0] = sum;
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], 1);
    vfpu_write_vector(d, 1, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vscl(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, 1, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         1);
    for (int i = 0; i < size; i++) d[i] = s[i] * t[0];
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vhdp(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    float s[4], t[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         size);
    // Homogeneous dot: last source element replaced with 1.0
    float sum = 0.0f;
    for (int i = 0; i < size - 1; i++) sum += s[i] * t[i];
    sum += t[size - 1];  // w component * 1.0
    d[0] = sum;
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], 1);
    vfpu_write_vector(d, 1, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vcrs(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t size) {
    // PPSSPP Int_Vcrs: "half a cross product". S is forced to swizzle (y,z,x)
    // and T to (z,x,y) (user abs/neg/const bits still apply), then the result
    // is the lane-wise product (s1*t2, s2*t0, s0*t1). vcrsp supplies the
    // other half.
    float s[4] = {}, t[4] = {}, d[4] = {};
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], vfpu_pfx_swizzle(3, 3, 3, 0),
        vfpu_pfx_swizzle(1, 2, 0, 0)), size);
    vfpu_apply_prefix_st(t, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX], vfpu_pfx_swizzle(3, 3, 3, 0),
        vfpu_pfx_swizzle(2, 0, 1, 0)), size);
    for (int i = 0; i < size; i++) d[i] = s[i] * t[i];
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vdet(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t vt,
               uint8_t /*size*/) {
    // Determinant (pair only)
    float s[4], t[4], d[4];
    vfpu_read_vector(s, 2, vs, ctx->vfpu);
    vfpu_read_vector(t, 2, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         2);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         2);
    d[0] = s[0] * t[1] - s[1] * t[0];
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], 1);
    vfpu_write_vector(d, 1, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

// ---------------------------------------------------------------------------
// Compare / conditional
// ---------------------------------------------------------------------------

void vfpu_vcmp(recomp_context* ctx, uint8_t*,
               uint8_t vs, uint8_t vt, uint8_t cond,
               uint8_t size) {
    float s[4], t[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_read_vector(t, size, vt, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    vfpu_apply_prefix_st(t, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         size);

    // PPSSPP Int_Vcmp: only bits 4, 5 and the lanes < size of CC change;
    // the EN/EI/ES/NN/NI/NS conditions test s only.
    uint32_t cc = 0;
    int or_val = 0;
    int and_val = 1;
    uint32_t affected_bits = (1u << 4) | (1u << 5);

    for (int i = 0; i < size; i++) {
        int c = 0;
        switch (cond & 0xF) {
        case 0:  c = 0; break;                          // FL
        case 1:  c = (s[i] == t[i]); break;             // EQ
        case 2:  c = (s[i] < t[i]); break;              // LT
        case 3:  c = (s[i] <= t[i]); break;             // LE
        case 4:  c = 1; break;                          // TR
        case 5:  c = (s[i] != t[i]); break;             // NE
        case 6:  c = (s[i] >= t[i]); break;             // GE
        case 7:  c = (s[i] > t[i]); break;              // GT
        case 8:  c = (s[i] == 0.0f); break;             // EZ
        case 9:  c = std::isnan(s[i]); break;           // EN
        case 10: c = std::isinf(s[i]); break;           // EI
        case 11: c = is_nan_or_inf(s[i]); break;        // ES
        case 12: c = (s[i] != 0.0f); break;             // NZ
        case 13: c = !std::isnan(s[i]); break;          // NN
        case 14: c = !std::isinf(s[i]); break;          // NI
        case 15: c = !is_nan_or_inf(s[i]); break;       // NS
        }
        cc |= static_cast<uint32_t>(c) << i;
        or_val |= c;
        and_val &= c;
        affected_bits |= 1u << i;
    }

    uint32_t old = ctx->vfpu_ctrl[VFPU_CTRL_CC];
    ctx->vfpu_ctrl[VFPU_CTRL_CC] =
        (old & ~affected_bits) |
        ((cc | (static_cast<uint32_t>(or_val) << 4) |
          (static_cast<uint32_t>(and_val) << 5)) & affected_bits);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vcmov(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t cc_field,
                uint8_t size) {
    // cc_field packs the PSP vcmov operands as decoded by the Rust
    // decoder: imm3 = bits[2:0] (CC bit selector), tf = bit[3]
    // (the true/false sense bit, op[19]). Matches PPSSPP Int_Vcmov:
    // the conditional move fires when ((CC >> imm3) & 1) == !tf.
    const int imm3 = cc_field & 7;
    const bool tf = (cc_field >> 3) & 1;

    float s[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    // D is read as the T operand and the T prefix applies to it.
    vfpu_read_vector(d, size, vd, ctx->vfpu);
    vfpu_apply_prefix_st(d, ctx->vfpu_ctrl[VFPU_CTRL_TPREFIX],
                         size);

    uint32_t cc = ctx->vfpu_ctrl[VFPU_CTRL_CC];

    if (imm3 < 6) {
        if ((int)((cc >> imm3) & 1) == (int)(!tf)) {
            for (int i = 0; i < size; i++) d[i] = s[i];
        }
    } else if (imm3 == 6) {
        // Per-element: move lane i when CC[i] matches the tf sense.
        for (int i = 0; i < size; i++) {
            if ((int)((cc >> i) & 1) == (int)(!tf)) d[i] = s[i];
        }
    }
    // imm3 == 7 is invalid on hardware (PPSSPP logs and no-ops).

    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}
