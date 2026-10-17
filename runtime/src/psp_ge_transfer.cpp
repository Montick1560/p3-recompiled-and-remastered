#include "psp_ge_transfer.h"

#include <cstring>

// GE block transfer, memory path of PPSSPP GPUCommon::DoBlockTransfer
// (GPU/GPUCommon.cpp) with the GPUState.h register getters. Addresses are
// masked into the 128 MB guest image like every other runtime access; rows
// are moved one at a time so overlapping rects copy correctly.

namespace {

constexpr uint32_t kAddrMask = 0x07FFFFFFu;
constexpr uint64_t kMemSize = 0x08000000u;

uint32_t address(uint32_t lo, uint32_t w) {
    uint32_t a = (lo & 0xFFFFF0u) | ((w & 0xFF0000u) << 8);
    // VRAM wraps: 0x048xxxxx mirrors 0x040xxxxx.
    if ((a & 0x04800000u) == 0x04800000u) a &= ~0x00800000u;
    return a;
}

uint32_t stride(uint32_t w) {
    uint32_t s = w & 0x7F8u;
    return s > 0x400u ? 0 : s;
}

}  // namespace

GeTransferRect psp_ge_transfer_rect(const GeTransferRegs& r) {
    GeTransferRect t;
    t.src = address(r.src, r.srcw);
    t.dst = address(r.dst, r.dstw);
    t.src_stride = stride(r.srcw);
    t.dst_stride = stride(r.dstw);
    t.sx = r.srcpos & 0x3FF; t.sy = (r.srcpos >> 10) & 0x3FF;
    t.dx = r.dstpos & 0x3FF; t.dy = (r.dstpos >> 10) & 0x3FF;
    t.width = (r.size & 0x3FF) + 1; t.height = ((r.size >> 10) & 0x3FF) + 1;
    t.bpp = (r.start & 1) ? 4 : 2;
    return t;
}

void psp_ge_block_transfer(uint8_t* rdram, const GeTransferRegs& r) {
    const GeTransferRect t = psp_ge_transfer_rect(r);
    const uint32_t row_bytes = t.width * t.bpp;
    for (uint32_t y = 0; y < t.height; y++) {
        const uint32_t s = (t.src + ((t.sy + y) * t.src_stride + t.sx) * t.bpp) & kAddrMask;
        const uint32_t d = (t.dst + ((t.dy + y) * t.dst_stride + t.dx) * t.bpp) & kAddrMask;
        if (s + uint64_t{row_bytes} > kMemSize || d + uint64_t{row_bytes} > kMemSize) continue;
        std::memmove(rdram + d, rdram + s, row_bytes);
    }
}
