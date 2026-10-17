#pragma once

#include <cstdint>

/// Raw GE block-transfer registers (24-bit command data), as latched by the
/// TRANSFERSRC/SRCW/DST/DSTW/SRCPOS/DSTPOS/SIZE/START commands.
struct GeTransferRegs {
    uint32_t src, srcw, dst, dstw, srcpos, dstpos, size, start;
};

/// Decoded TRANSFER registers, PPSSPP GPUCommon::DoBlockTransfer: full addresses (VRAM mirror
/// folded), strides and positions in pixels, rect size, bytes per pixel.
struct GeTransferRect {
    uint32_t src, dst, src_stride, dst_stride, sx, sy, dx, dy, width, height, bpp;
};
GeTransferRect psp_ge_transfer_rect(const GeTransferRegs& r);

/// TRANSFERSTART: copy a width x height rect of 2- or 4-byte pixels between
/// guest memory images, row by row (overlap-safe), with PPSSPP's register
/// decoding and VRAM mirror rule (GPUCommon::DoBlockTransfer, memory path).
void psp_ge_block_transfer(uint8_t* rdram, const GeTransferRegs& r);
