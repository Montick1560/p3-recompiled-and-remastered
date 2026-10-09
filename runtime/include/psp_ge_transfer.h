#pragma once

#include <cstdint>

/// Raw GE block-transfer registers (24-bit command data), as latched by the
/// TRANSFERSRC/SRCW/DST/DSTW/SRCPOS/DSTPOS/SIZE/START commands.
struct GeTransferRegs {
    uint32_t src, srcw, dst, dstw, srcpos, dstpos, size, start;
};

/// TRANSFERSTART: copy a width x height rect of 2- or 4-byte pixels between
/// guest memory images, row by row (overlap-safe), with PPSSPP's register
/// decoding and VRAM mirror rule (GPUCommon::DoBlockTransfer, memory path).
void psp_ge_block_transfer(uint8_t* rdram, const GeTransferRegs& r);
