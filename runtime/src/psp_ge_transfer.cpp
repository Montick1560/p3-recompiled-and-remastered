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

void psp_ge_block_transfer(uint8_t* rdram, const GeTransferRegs& r) {
    const uint32_t src = address(r.src, r.srcw), dst = address(r.dst, r.dstw);
    const uint32_t src_stride = stride(r.srcw), dst_stride = stride(r.dstw);
    const uint32_t sx = r.srcpos & 0x3FF, sy = (r.srcpos >> 10) & 0x3FF;
    const uint32_t dx = r.dstpos & 0x3FF, dy = (r.dstpos >> 10) & 0x3FF;
    const uint32_t width = (r.size & 0x3FF) + 1, height = ((r.size >> 10) & 0x3FF) + 1;
    const uint32_t bpp = (r.start & 1) ? 4 : 2;
    const uint32_t row_bytes = width * bpp;
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t s = (src + ((sy + y) * src_stride + sx) * bpp) & kAddrMask;
        const uint32_t d = (dst + ((dy + y) * dst_stride + dx) * bpp) & kAddrMask;
        if (s + uint64_t{row_bytes} > kMemSize || d + uint64_t{row_bytes} > kMemSize) continue;
        std::memmove(rdram + d, rdram + s, row_bytes);
    }
}
