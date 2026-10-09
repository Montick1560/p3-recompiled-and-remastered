#include "psp_vfpu.h"
#include "recomp.h"
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

// ---------------------------------------------------------------------------
// Quarter-turn trig helpers
// ---------------------------------------------------------------------------

/// Quarter-turn sine of a single value.
/// angle=1.0 => 90 degrees => returns 1.0
/// Cardinal values are exact; non-cardinal uses sinf fallback.
static float vfpu_sin_single(float angle) {
    float reduced = std::fmod(angle, 4.0f);
    if (reduced < 0.0f) reduced += 4.0f;

    // Exact cardinal fast paths (bit-exact)
    if (reduced == 0.0f) return 0.0f;
    if (reduced == 1.0f) return 1.0f;
    if (reduced == 2.0f) return 0.0f;
    if (reduced == 3.0f) return -1.0f;

    return std::sinf(reduced * static_cast<float>(M_PI_2));
}

/// Quarter-turn cosine of a single value.
/// cos(x) = sin(x + 1) in quarter-turn convention.
static float vfpu_cos_single(float angle) {
    float reduced = std::fmod(angle, 4.0f);
    if (reduced < 0.0f) reduced += 4.0f;

    // Exact cardinal fast paths (bit-exact)
    if (reduced == 0.0f) return 1.0f;
    if (reduced == 1.0f) return 0.0f;
    if (reduced == 2.0f) return -1.0f;
    if (reduced == 3.0f) return 0.0f;

    return std::cosf(reduced * static_cast<float>(M_PI_2));
}

/// NaN-aware clamp [lo, hi]
static inline float nanclamp_local(float f, float lo, float hi) {
    float r = (f <= lo) ? lo : f;
    r = (r >= hi) ? hi : r;
    return r;
}

// ---------------------------------------------------------------------------
// Unary math ops (all follow read-prefix-compute-prefix_d-write-eat)
// ---------------------------------------------------------------------------

/// Helper macro for unary VFPU ops that read vs and write vd.
#define VFPU_UNARY_OP(name, body) \
void name(recomp_context* ctx, uint8_t*, \
          uint8_t vd, uint8_t vs, uint8_t size) { \
    float s[4], d[4]; \
    vfpu_read_vector(s, size, vs, ctx->vfpu); \
    vfpu_apply_prefix_st(s, \
        ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], size); \
    for (int i = 0; i < size; i++) { body } \
    vfpu_apply_prefix_d(d, \
        ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size); \
    vfpu_write_vector(d, size, vd, ctx->vfpu, \
        ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]); \
    vfpu_eat_prefixes(ctx); \
}

VFPU_UNARY_OP(vfpu_vmov,   d[i] = s[i];)

/// PPSSPP Int_VV2Op "last lane" ops (vrcp, vrsq, vsin, vcos, vexp2, vlog2,
/// vsqrt, vasin, vnrcp, vnsin, vrexp2): lanes before the last are computed
/// from the raw source, the S prefix applies to the last lane only (as lane
/// 0 of the prefix; negate is ignored by vnrcp/vnsin/vrexp2), an out of range
/// swizzle there zeroes the result, and D keeps only lane 0's mask and
/// saturation, applied to the last lane.
#define VFPU_LASTLANE_OP(name, ignore_neg, body) \
void name(recomp_context* ctx, uint8_t*, \
          uint8_t vd, uint8_t vs, uint8_t size) { \
    float s[4], d[4]; \
    vfpu_read_vector(s, size, vs, ctx->vfpu); \
    const uint32_t sp = ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX]; \
    vfpu_apply_prefix_st(&s[size - 1], (ignore_neg) ? \
        vfpu_rewrite_prefix(sp, vfpu_pfx_negate(1, 0, 0, 0), 0) : sp, 1); \
    for (int i = 0; i < size; i++) { body } \
    if (vfpu_last_lane_swizzle_invalid(sp)) d[size - 1] = 0.0f; \
    const uint32_t dp = vfpu_last_lane_dprefix( \
        ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size); \
    vfpu_apply_prefix_d(d, dp, size); \
    vfpu_write_vector(d, size, vd, ctx->vfpu, dp); \
    vfpu_eat_prefixes(ctx); \
}

VFPU_LASTLANE_OP(vfpu_vrcp,  false, d[i] = 1.0f / s[i];)
VFPU_LASTLANE_OP(vfpu_vrsq,  false, d[i] = 1.0f / std::sqrtf(s[i]);)
VFPU_LASTLANE_OP(vfpu_vsin,  false, d[i] = vfpu_sin_single(s[i]);)
VFPU_LASTLANE_OP(vfpu_vcos,  false, d[i] = vfpu_cos_single(s[i]);)
VFPU_LASTLANE_OP(vfpu_vexp2, false, d[i] = std::exp2f(s[i]);)
VFPU_LASTLANE_OP(vfpu_vlog2, false, d[i] = std::log2f(s[i]);)
VFPU_LASTLANE_OP(vfpu_vsqrt, false, d[i] = std::sqrtf(s[i]);)
VFPU_LASTLANE_OP(vfpu_vasin, false,
    d[i] = std::asinf(s[i]) / static_cast<float>(M_PI_2);)
VFPU_LASTLANE_OP(vfpu_vnrcp, true,  d[i] = -(1.0f / s[i]);)
VFPU_LASTLANE_OP(vfpu_vnsin, true,  d[i] = -vfpu_sin_single(s[i]);)
VFPU_LASTLANE_OP(vfpu_vrexp2, true, d[i] = 1.0f / std::exp2f(s[i]);)

#undef VFPU_LASTLANE_OP

/// vabs / vneg are prefix hacks in PPSSPP: the S prefix gets abs (or negate)
/// forced on every lane and then applies normally, so a user negate/abs still
/// combines (abs then negate order), and constants are affected.
static void vfpu_forced_prefix_move(recomp_context* ctx, uint8_t vd,
                                    uint8_t vs, uint8_t size,
                                    uint32_t add) {
    float s[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], 0, add), size);
    vfpu_apply_prefix_d(s, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX], size);
    vfpu_write_vector(s, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vabs(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_forced_prefix_move(ctx, vd, vs, size, vfpu_pfx_abs(1, 1, 1, 1));
}

void vfpu_vneg(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t vs, uint8_t size) {
    vfpu_forced_prefix_move(ctx, vd, vs, size, vfpu_pfx_negate(1, 1, 1, 1));
}

#undef VFPU_UNARY_OP

// ---------------------------------------------------------------------------
// Saturation ops
// ---------------------------------------------------------------------------

void vfpu_vsat0(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    float s[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    for (int i = 0; i < size; i++) {
        d[i] = nanclamp_local(s[i], 0.0f, 1.0f);
    }
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vsat1(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t vs, uint8_t size) {
    float s[4], d[4];
    vfpu_read_vector(s, size, vs, ctx->vfpu);
    vfpu_apply_prefix_st(s, ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX],
                         size);
    for (int i = 0; i < size; i++) {
        d[i] = nanclamp_local(s[i], -1.0f, 1.0f);
    }
    // vsat1 is itself a prefix hack: the D saturation does not apply, only
    // the write mask does (PPSSPP Int_VV2Op optype 5).
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

// ---------------------------------------------------------------------------
// Identity / zero / one (ignore SPREFIX, apply DPREFIX)
// ---------------------------------------------------------------------------

void vfpu_vidt(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t size) {
    // PPSSPP Int_Vidt: the S prefix is replaced by constants (1 at the lane
    // matching vd's position, 0 elsewhere) with user abs/neg still applying.
    // The position is vd & 3 for triple/quad but vd & 1 for pair.
    float d[4] = {};
    const int offmask = (size == 4 || size == 3) ? 3 : 1;
    const int off = vd & offmask;
    auto k = [&](int lane) {
        return (off == (lane & offmask)) ? VFPU_CONST_ONE : VFPU_CONST_ZERO;
    };
    vfpu_apply_prefix_st(d, vfpu_rewrite_prefix(
        ctx->vfpu_ctrl[VFPU_CTRL_SPREFIX], VFPU_PFX_ANY_SWIZZLE,
        vfpu_pfx_constants(k(0), k(1), k(2), k(3))), size);
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vzero(recomp_context* ctx, uint8_t*,
                uint8_t vd, uint8_t size) {
    float d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

void vfpu_vone(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t size) {
    float d[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}

// ---------------------------------------------------------------------------
// VFPU constants table (vcst)
// ---------------------------------------------------------------------------

static const float vfpu_cst_table[32] = {
    0.0f,                        // 0: undefined (use 0)
    HUGE_VALF,                   // 1: VFPU_HUGE
    static_cast<float>(M_SQRT2), // 2: VFPU_SQRT2
    static_cast<float>(M_SQRT1_2), // 3: VFPU_SQRT1_2
    static_cast<float>(M_2_SQRTPI), // 4: VFPU_2_SQRTPI
    static_cast<float>(M_2_PI),  // 5: VFPU_2_PI
    static_cast<float>(M_1_PI), // 6: VFPU_1_PI
    static_cast<float>(M_PI_4), // 7: VFPU_PI_4
    static_cast<float>(M_PI_2), // 8: VFPU_PI_2
    static_cast<float>(M_PI),   // 9: VFPU_PI
    static_cast<float>(M_E),    // 10: VFPU_E
    static_cast<float>(M_LOG2E), // 11: VFPU_LOG2E
    static_cast<float>(M_LOG10E), // 12: VFPU_LOG10E
    static_cast<float>(M_LN2),  // 13: VFPU_LN2
    static_cast<float>(M_LN10), // 14: VFPU_LN10
    static_cast<float>(2.0 * M_PI), // 15: VFPU_2PI
    static_cast<float>(M_PI / 6.0), // 16: VFPU_PI_6
    static_cast<float>(std::log10(2.0)), // 17: VFPU_LOG10_2
    static_cast<float>(std::log2(10.0)), // 18: VFPU_LOG2_10
    static_cast<float>(std::sqrt(3.0) / 2.0), // 19: VFPU_SQRT3_2
    0.0f, 0.0f, 0.0f, 0.0f,    // 20-23: reserved
    0.0f, 0.0f, 0.0f, 0.0f,    // 24-27: reserved
    0.0f, 0.0f, 0.0f, 0.0f,    // 28-31: reserved
};

void vfpu_vcst(recomp_context* ctx, uint8_t*,
               uint8_t vd, uint8_t imm5, uint8_t size) {
    float d[4];
    float val = vfpu_cst_table[imm5 & 0x1F];
    for (int i = 0; i < size; i++) d[i] = val;
    vfpu_apply_prefix_d(d, ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX],
                        size);
    vfpu_write_vector(d, size, vd, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
    vfpu_eat_prefixes(ctx);
}
