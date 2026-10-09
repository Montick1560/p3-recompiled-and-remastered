#pragma once
#include <cstdint>

// Forward declaration (defined in recomp.h)
struct recomp_context;

// VFPU control register indices
constexpr int VFPU_CTRL_SPREFIX = 0;
constexpr int VFPU_CTRL_TPREFIX = 1;
constexpr int VFPU_CTRL_DPREFIX = 2;
constexpr int VFPU_CTRL_CC = 3;
constexpr int VFPU_CTRL_INF4 = 4;
constexpr int VFPU_CTRL_RSV5 = 5;
constexpr int VFPU_CTRL_RSV6 = 6;
constexpr int VFPU_CTRL_REV = 7;
constexpr int VFPU_CTRL_RCX0 = 8;
constexpr int VFPU_CTRL_RCX1 = 9;
constexpr int VFPU_CTRL_RCX2 = 10;
constexpr int VFPU_CTRL_RCX3 = 11;
constexpr int VFPU_CTRL_RCX4 = 12;
constexpr int VFPU_CTRL_RCX5 = 13;
constexpr int VFPU_CTRL_RCX6 = 14;
constexpr int VFPU_CTRL_RCX7 = 15;

// ---------------------------------------------------------------------------
// Register helpers
// ---------------------------------------------------------------------------

/// Physical register-file layout follows PPSSPP's convention:
///   vfpu[mtx * 16 + col * 4 + row]
/// i.e. each matrix is stored column-major in the flat array.
/// This single helper is shared by all VFPU translation units so the
/// layout choice stays encapsulated in one place.
inline int vfpu_single_index(int reg) {
    int mtx = (reg >> 2) & 7;
    int col = reg & 3;
    int row = (reg >> 5) & 3;
    return mtx * 16 + col * 4 + row;
}

void vfpu_read_vector(float* dst, int n, int reg,
                      const float vfpu[128]);
void vfpu_write_vector(const float* src, int n, int reg,
                       float vfpu[128], uint32_t dprefix);

// ---------------------------------------------------------------------------
// Prefix helpers
// ---------------------------------------------------------------------------
void vfpu_apply_prefix_st(float* r, uint32_t prefix, int n);
/// PPSSPP ApplyPrefixST with an explicit fill for lanes >= n (an out of range
/// swizzle reads `invalid`; vfpu_apply_prefix_st uses 0.0f).
void vfpu_apply_prefix_st_inv(float* r, uint32_t prefix, int n, float invalid);
void vfpu_apply_prefix_d(float* r, uint32_t dprefix, int n);

// ---------------------------------------------------------------------------
// PPSSPP prefix-rewrite helpers (VFPURewritePrefix & friends). Several VFPU
// ops force parts of the S/T prefix (a fixed swizzle, forced negate, forced
// constants) while the user's other prefix bits still apply.
// ---------------------------------------------------------------------------
constexpr uint32_t VFPU_PFX_ANY_SWIZZLE = 0x000000FFu;
inline uint32_t vfpu_pfx_swizzle(int x, int y, int z, int w) {
    return (uint32_t)(x | (y << 2) | (z << 4) | (w << 6));
}
inline uint32_t vfpu_pfx_mask4(int x, int y, int z, int w) {
    return (uint32_t)(x | (y << 1) | (z << 2) | (w << 3));
}
inline uint32_t vfpu_pfx_abs(int x, int y, int z, int w) {
    return vfpu_pfx_mask4(x, y, z, w) << 8;
}
inline uint32_t vfpu_pfx_negate(int x, int y, int z, int w) {
    return vfpu_pfx_mask4(x, y, z, w) << 16;
}
/// Constant selectors (index into the prefix constant table). -1 = leave lane.
enum : int {
    VFPU_CONST_NONE = -1, VFPU_CONST_ZERO = 0, VFPU_CONST_ONE, VFPU_CONST_TWO,
    VFPU_CONST_HALF, VFPU_CONST_THREE, VFPU_CONST_THIRD, VFPU_CONST_FOURTH,
    VFPU_CONST_SIXTH
};
/// Prefix bits that force lane `lane` to constant `c` (swizzle + abs + const flag).
inline uint32_t vfpu_pfx_const_lane(int lane, int c) {
    if (c < 0) return 0;
    return (uint32_t)(((c & 3) << (lane * 2)) | (((c >> 2) & 1) << (8 + lane))
                      | (1 << (12 + lane)));
}
inline uint32_t vfpu_pfx_constants(int x, int y, int z, int w) {
    return vfpu_pfx_const_lane(0, x) | vfpu_pfx_const_lane(1, y)
         | vfpu_pfx_const_lane(2, z) | vfpu_pfx_const_lane(3, w);
}
inline uint32_t vfpu_rewrite_prefix(uint32_t prefix, uint32_t remove, uint32_t add) {
    return (prefix & ~remove) | add;
}
/// PPSSPP RetainInvalidSwizzleST: zero d[i] where the S or T swizzle of lane i
/// names a lane >= n without the constant flag.
void vfpu_retain_invalid_swizzle(float* d, uint32_t sprefix, uint32_t tprefix, int n);
/// PPSSPP LastLaneSwizzleInvalid: the single-lane prefix of a last-lane op
/// (vrcp, vsin, vdiv...) names a lane other than the first.
bool vfpu_last_lane_swizzle_invalid(uint32_t prefix);
/// PPSSPP's min/max ordering (ties return b; denormals and +-0 tie).
float vfpu_min(float a, float b);
float vfpu_max(float a, float b);
/// Half-float conversions exactly as PPSSPP vfpu_h2f / vfpu_f2h.
uint32_t vfpu_h2f_bits(uint16_t h);
uint16_t vfpu_f2h_bits(uint32_t f);
void vfpu_eat_prefixes(recomp_context* ctx);
void vfpu_set_prefix(recomp_context* ctx, int reg_idx,
                     uint32_t data);

/// Initialise a freshly-zeroed context's VFPU control registers to their
/// hardware-reset/"no prefix pending" state. The S/T prefixes default to the
/// identity swizzle 0xE4 (lane i <- component i); a zero prefix is NOT the
/// default -- it is an explicit "all lanes <- component 0" swizzle. Every
/// recomp_context must be passed through this after a memset(0), or the first
/// VFPU arithmetic op on the thread (before any eat_prefixes resets the state)
/// silently swizzles all operand lanes to component 0 and corrupts the result.
void vfpu_init_context(recomp_context* ctx);

// ---------------------------------------------------------------------------
// Control register moves
// ---------------------------------------------------------------------------
void vfpu_mfv(recomp_context* ctx, int rt_idx, uint8_t vd);
void vfpu_mtv(recomp_context* ctx, int rt_idx, uint8_t vd);
void vfpu_mfvc(recomp_context* ctx, int rt_idx, int imm);
void vfpu_mtvc(recomp_context* ctx, int rt_idx, int imm);
void vfpu_vmfvc(recomp_context* ctx, uint8_t vd, uint8_t imm);
void vfpu_vmtvc(recomp_context* ctx, uint8_t vs, uint8_t imm);
uint32_t vfpu_last_lane_dprefix(uint32_t d, int n);

// ---------------------------------------------------------------------------
// Immediate loads
// ---------------------------------------------------------------------------
void vfpu_viim(recomp_context* ctx, uint8_t vt, uint16_t imm);
void vfpu_vfim(recomp_context* ctx, uint8_t vt, uint16_t imm);

// ---------------------------------------------------------------------------
// Arithmetic (binary)
// ---------------------------------------------------------------------------
void vfpu_vadd(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vsub(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vmul(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vdiv(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vmin(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vmax(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vscmp(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vsge(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vslt(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);

// Reduction / special binary ops
void vfpu_vdot(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vscl(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vhdp(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vcrs(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vdet(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);

// Compare / conditional
void vfpu_vcmp(recomp_context* ctx, uint8_t* rdram,
               uint8_t vs, uint8_t vt, uint8_t cond, uint8_t size);
void vfpu_vcmov(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t cc, uint8_t size);

// ---------------------------------------------------------------------------
// Trig / unary math
// ---------------------------------------------------------------------------
void vfpu_vmov(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vabs(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vneg(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vrcp(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vrsq(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vsin(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vcos(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vexp2(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vlog2(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vsqrt(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vasin(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vnrcp(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vnsin(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vrexp2(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t vs, uint8_t size);

// Identity / zero / one / saturation
void vfpu_vidt(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t size);
void vfpu_vzero(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t size);
void vfpu_vone(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t size);
void vfpu_vsat0(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vsat1(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);

// Constants
void vfpu_vcst(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t imm5, uint8_t size);

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------
void vfpu_vf2in(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t imm5, uint8_t size);
void vfpu_vf2iz(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t imm5, uint8_t size);
void vfpu_vf2iu(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t imm5, uint8_t size);
void vfpu_vf2id(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t imm5, uint8_t size);
void vfpu_vi2f(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t imm5, uint8_t size);
void vfpu_vi2uc(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vi2c(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vi2us(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vi2s(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vuc2i(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vc2i(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vus2i(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vs2i(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vf2h(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vh2f(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);

// ---------------------------------------------------------------------------
// Matrix operations
// ---------------------------------------------------------------------------
void vfpu_vmmul(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vmscl(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vtfm2(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t vt);
void vfpu_vtfm3(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t vt);
void vfpu_vtfm4(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t vt);
void vfpu_vhtfm2(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t vs, uint8_t vt);
void vfpu_vhtfm3(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t vs, uint8_t vt);
void vfpu_vhtfm4(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t vs, uint8_t vt);
void vfpu_vcrsp(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t vt);
void vfpu_vqmul(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t vt);
void vfpu_vmmov(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vmidt(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t size);
void vfpu_vmzero(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t size);
void vfpu_vmone(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t size);

// ---------------------------------------------------------------------------
// Memory operations
// ---------------------------------------------------------------------------
void vfpu_lv_s(recomp_context* ctx, uint8_t* rdram,
               uint8_t vt, uint8_t rs, int16_t offset);
void vfpu_sv_s(recomp_context* ctx, uint8_t* rdram,
               uint8_t vt, uint8_t rs, int16_t offset);
void vfpu_lv_q(recomp_context* ctx, uint8_t* rdram,
               uint8_t vt, uint8_t rs, int16_t offset);
void vfpu_sv_q(recomp_context* ctx, uint8_t* rdram,
               uint8_t vt, uint8_t rs, int16_t offset);
void vfpu_lvl_q(recomp_context* ctx, uint8_t* rdram,
                uint8_t vt, uint8_t rs, int16_t offset);
void vfpu_lvr_q(recomp_context* ctx, uint8_t* rdram,
                uint8_t vt, uint8_t rs, int16_t offset);
void vfpu_svl_q(recomp_context* ctx, uint8_t* rdram,
                uint8_t vt, uint8_t rs, int16_t offset);
void vfpu_svr_q(recomp_context* ctx, uint8_t* rdram,
                uint8_t vt, uint8_t rs, int16_t offset);

// ---------------------------------------------------------------------------
// Sort / pack / misc
// ---------------------------------------------------------------------------
void vfpu_vsrt1(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vsrt2(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vsrt3(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vsrt4(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vbfy1(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vbfy2(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vocp(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vsocp(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vfad(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vavg(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);

// Sign / exponent / color pack (decoded since the VFPU9/VFPU7/VFPU0 fixes)
void vfpu_vsgn(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vsbn(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t vt, uint8_t size);
void vfpu_vsbz(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vlgb(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vt4444(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vt5551(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t vs, uint8_t size);
void vfpu_vt5650(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t vs, uint8_t size);

// Random
void vfpu_vrnds(recomp_context* ctx, uint8_t* rdram,
                uint8_t vs);
void vfpu_vrndi(recomp_context* ctx, uint8_t* rdram,
                uint8_t vd, uint8_t size);
void vfpu_vrndf1(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t size);
void vfpu_vrndf2(recomp_context* ctx, uint8_t* rdram,
                 uint8_t vd, uint8_t size);

// Rotation
void vfpu_vrot(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint16_t imm5, uint8_t size);

// Wrap by negative (complex)
void vfpu_vwbn(recomp_context* ctx, uint8_t* rdram,
               uint8_t vd, uint8_t vs, uint8_t imm8, uint8_t size);

// Flush / no-op
void vfpu_vflush();

// Unknown opcode stub (log-once warning)
void vfpu_unknown_stub(recomp_context* ctx, uint8_t* rdram,
                       uint32_t opcode, uint32_t pc);
