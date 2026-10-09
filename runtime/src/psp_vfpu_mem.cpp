#include "psp_vfpu.h"
#include "recomp.h"
#include <cstring>

// Single-register flat index comes from the shared vfpu_single_index()
// inline in psp_vfpu.h (layout: vfpu[mtx*16 + col*4 + row]).

// ---------------------------------------------------------------------------
// Memory load/store operations
// All use 0x07FFFFFFU address masking (via psp_mem_read/write)
// ---------------------------------------------------------------------------

void vfpu_lv_s(recomp_context* ctx, uint8_t* rdram,
               uint8_t vt, uint8_t rs, int16_t offset) {
    uint32_t addr =
        static_cast<uint32_t>(ctx->r[rs]) + offset;
    float val = psp_mem_read<float>(rdram, addr);
    int idx = vfpu_single_index(vt);
    ctx->vfpu[idx] = val;
}

void vfpu_sv_s(recomp_context* ctx, uint8_t* rdram,
               uint8_t vt, uint8_t rs, int16_t offset) {
    uint32_t addr =
        static_cast<uint32_t>(ctx->r[rs]) + offset;
    int idx = vfpu_single_index(vt);
    psp_mem_write<float>(rdram, addr, ctx->vfpu[idx]);
}

void vfpu_lv_q(recomp_context* ctx, uint8_t* rdram,
               uint8_t vt, uint8_t rs, int16_t offset) {
    uint32_t addr =
        static_cast<uint32_t>(ctx->r[rs]) + offset;
    addr &= ~0xFu;  // 16-byte align
    float d[4];
    for (int i = 0; i < 4; i++) {
        d[i] = psp_mem_read<float>(rdram, addr + i * 4);
    }
    // PPSSPP WriteVector applies the D-prefix write mask (lv.q does not
    // consume the prefix).
    vfpu_write_vector(d, 4, vt, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
}

void vfpu_sv_q(recomp_context* ctx, uint8_t* rdram,
               uint8_t vt, uint8_t rs, int16_t offset) {
    uint32_t addr =
        static_cast<uint32_t>(ctx->r[rs]) + offset;
    addr &= ~0xFu;  // 16-byte align
    float s[4];
    vfpu_read_vector(s, 4, vt, ctx->vfpu);
    for (int i = 0; i < 4; i++) {
        psp_mem_write<float>(rdram, addr + i * 4, s[i]);
    }
}

// lvl.q / lvr.q / svl.q / svr.q follow PPSSPP Int_SVQ (InterpreterVFPU.cpp
// "case 53" / "case 61"). With offset = (addr >> 2) & 3:
//   lvl: d[3 - i] = mem[addr - 4 i]  for i in 0..offset
//   lvr: d[i]     = mem[addr + 4 i]  for i in 0..(3 - offset)
// svl / svr store the same lanes the same way. The address is not aligned
// down; lanes that are not touched keep their previous value.
void vfpu_lvl_q(recomp_context* ctx, uint8_t* rdram,
                uint8_t vt, uint8_t rs, int16_t offset) {
    uint32_t addr =
        static_cast<uint32_t>(ctx->r[rs]) + offset;
    float d[4];
    vfpu_read_vector(d, 4, vt, ctx->vfpu);  // untouched lanes keep their value
    int lane = (addr >> 2) & 3;
    for (int i = 0; i < lane + 1; i++) {
        d[3 - i] = psp_mem_read<float>(rdram, addr - 4u * i);
    }
    vfpu_write_vector(d, 4, vt, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
}

void vfpu_lvr_q(recomp_context* ctx, uint8_t* rdram,
                uint8_t vt, uint8_t rs, int16_t offset) {
    uint32_t addr =
        static_cast<uint32_t>(ctx->r[rs]) + offset;
    float d[4];
    vfpu_read_vector(d, 4, vt, ctx->vfpu);
    int lane = (addr >> 2) & 3;
    for (int i = 0; i < (3 - lane) + 1; i++) {
        d[i] = psp_mem_read<float>(rdram, addr + 4u * i);
    }
    vfpu_write_vector(d, 4, vt, ctx->vfpu,
                      ctx->vfpu_ctrl[VFPU_CTRL_DPREFIX]);
}

void vfpu_svl_q(recomp_context* ctx, uint8_t* rdram,
                uint8_t vt, uint8_t rs, int16_t offset) {
    uint32_t addr =
        static_cast<uint32_t>(ctx->r[rs]) + offset;
    float s[4];
    vfpu_read_vector(s, 4, vt, ctx->vfpu);
    int lane = (addr >> 2) & 3;
    for (int i = 0; i < lane + 1; i++) {
        psp_mem_write<float>(rdram, addr - 4u * i, s[3 - i]);
    }
}

void vfpu_svr_q(recomp_context* ctx, uint8_t* rdram,
                uint8_t vt, uint8_t rs, int16_t offset) {
    uint32_t addr =
        static_cast<uint32_t>(ctx->r[rs]) + offset;
    float s[4];
    vfpu_read_vector(s, 4, vt, ctx->vfpu);
    int lane = (addr >> 2) & 3;
    for (int i = 0; i < (3 - lane) + 1; i++) {
        psp_mem_write<float>(rdram, addr + 4u * i, s[i]);
    }
}
