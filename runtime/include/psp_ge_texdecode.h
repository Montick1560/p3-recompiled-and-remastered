#pragma once
// Pure GE texture decoding (no GL): reads a PSP texture from guest RAM and
// converts it to tightly packed RGBA8888 rows of `width` pixels.
// Honors the buffer width (row stride, TEXBUFWIDTH), swizzled (tiled) layout
// and the CLUT index transform. Oracle: PPSSPP GPU/Common/TextureDecoder and
// GPUgstate::transformClutIndex.

#include <cstdint>
#include <vector>

struct GeTexDecodeParams {
    uint32_t addr;       // texture address (guest, unmasked)
    int width;           // pixels
    int height;          // pixels
    int bufw;            // row stride in pixels (TEXBUFWIDTH); 0 -> width
    int format;          // GE_TFMT_*
    bool swizzle;        // TEXMODE bit 0
    uint32_t clut_addr;  // palette address (guest, unmasked)
    int clut_format;     // GE_CMODE_*
    int clut_shift;      // CLUTFORMAT bits 2-6
    int clut_mask;       // CLUTFORMAT bits 8-15
    int clut_start;      // CLUTFORMAT bits 16-20 (in units of 16 entries)
};

/// Bits per pixel of a GE texture format (DXT counted per texel).
int ge_tex_bits_per_pixel(int format);

/// Bytes the texture occupies in guest memory (stride * height, padded to the
/// 8-row tile height when swizzled).
uint32_t ge_tex_byte_span(const GeTexDecodeParams& p);

/// Decode into `rgba` (width * height * 4 bytes). Returns false for formats
/// it does not decode (the caller substitutes a placeholder).
bool ge_decode_texture(const uint8_t* rdram, const GeTexDecodeParams& p,
                       std::vector<uint8_t>& rgba);
