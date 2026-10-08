#include "psp_ge_texdecode.h"
#include "psp_ge_constants.h"

#include <cstring>

namespace {

constexpr uint32_t kAddrMask = 0x07FFFFFFU;
constexpr uint32_t kMemSize = 0x08000000U;

// Copy `len` guest bytes at `addr` (zero-filled past the end of RAM).
void read_guest(const uint8_t* rdram, uint32_t addr, uint8_t* dst, uint32_t len) {
    uint32_t off = addr & kAddrMask;
    uint32_t n = (off < kMemSize) ? std::min(len, kMemSize - off) : 0;
    if (n) std::memcpy(dst, rdram + off, n);
    if (n < len) std::memset(dst + n, 0, len - n);
}

uint8_t expand5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
uint8_t expand6(uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
uint8_t expand4(uint32_t v) { return static_cast<uint8_t>((v << 4) | v); }

// 16/32-bit color -> RGBA8888, for both texels and palette entries.
void color16(uint16_t v, int fmt, uint8_t* out) {
    switch (fmt) {
    case GE_TFMT_5650:  // == GE_CMODE_16BIT_BGR5650
        out[0] = expand5(v & 0x1F); out[1] = expand6((v >> 5) & 0x3F);
        out[2] = expand5((v >> 11) & 0x1F); out[3] = 255;
        break;
    case GE_TFMT_5551:  // == GE_CMODE_16BIT_ABGR5551
        out[0] = expand5(v & 0x1F); out[1] = expand5((v >> 5) & 0x1F);
        out[2] = expand5((v >> 10) & 0x1F); out[3] = (v & 0x8000) ? 255 : 0;
        break;
    default:            // 4444
        out[0] = expand4(v & 0xF); out[1] = expand4((v >> 4) & 0xF);
        out[2] = expand4((v >> 8) & 0xF); out[3] = expand4((v >> 12) & 0xF);
        break;
    }
}

void palette_entry(const uint8_t* rdram, const GeTexDecodeParams& p, uint32_t raw_index,
                   uint8_t* out) {
    // PPSSPP transformClutIndex: shift, mask, then OR the start offset
    // (wrapping at 1024 bytes of palette).
    const bool is32 = p.clut_format == GE_CMODE_32BIT_ABGR8888;
    const uint32_t wrap = is32 ? 0xFFU : 0x1FFU;
    const uint32_t index = ((raw_index >> p.clut_shift) & static_cast<uint32_t>(p.clut_mask))
                         | ((static_cast<uint32_t>(p.clut_start) << 4) & wrap);
    if (is32) {
        read_guest(rdram, p.clut_addr + index * 4, out, 4);
    } else {
        uint8_t b[2];
        read_guest(rdram, p.clut_addr + index * 2, b, 2);
        color16(static_cast<uint16_t>(b[0] | (b[1] << 8)), p.clut_format, out);
    }
}

}  // namespace

int ge_tex_bits_per_pixel(int format) {
    switch (format) {
    case GE_TFMT_5650:
    case GE_TFMT_5551:
    case GE_TFMT_4444:  return 16;
    case GE_TFMT_8888:  return 32;
    case GE_TFMT_CLUT4: return 4;
    case GE_TFMT_CLUT8: return 8;
    case GE_TFMT_CLUT16: return 16;
    case GE_TFMT_CLUT32: return 32;
    case GE_TFMT_DXT1:  return 4;
    case GE_TFMT_DXT3:
    case GE_TFMT_DXT5:  return 8;
    default:            return 32;
    }
}

static int stride_pixels(const GeTexDecodeParams& p) {
    return p.bufw > 0 ? p.bufw : p.width;
}

uint32_t ge_tex_byte_span(const GeTexDecodeParams& p) {
    const uint32_t stride_bytes =
        static_cast<uint32_t>(stride_pixels(p) * ge_tex_bits_per_pixel(p.format) + 7) / 8;
    const uint32_t rows = p.swizzle ? ((static_cast<uint32_t>(p.height) + 7) & ~7U)
                                    : static_cast<uint32_t>(p.height);
    return stride_bytes * rows;
}

bool ge_decode_texture(const uint8_t* rdram, const GeTexDecodeParams& p,
                       std::vector<uint8_t>& rgba) {
    const int bpp = ge_tex_bits_per_pixel(p.format);
    const bool clut = p.format == GE_TFMT_CLUT4 || p.format == GE_TFMT_CLUT8
                   || p.format == GE_TFMT_CLUT16 || p.format == GE_TFMT_CLUT32;
    const bool direct = p.format == GE_TFMT_5650 || p.format == GE_TFMT_5551
                     || p.format == GE_TFMT_4444 || p.format == GE_TFMT_8888;
    rgba.assign(static_cast<size_t>(p.width) * p.height * 4, 0);
    if (!clut && !direct) return false;

    // Linear (row-major, `stride` bytes per row) copy of the texture memory.
    const uint32_t stride = static_cast<uint32_t>(stride_pixels(p) * bpp + 7) / 8;
    const uint32_t span = ge_tex_byte_span(p);
    std::vector<uint8_t> raw(span);
    read_guest(rdram, p.addr, raw.data(), span);
    std::vector<uint8_t> linear;
    const uint8_t* src = raw.data();
    if (p.swizzle && stride >= 16) {
        // Tiles of 16 bytes x 8 rows, tile rows covering the full stride.
        linear.assign(span, 0);
        const uint32_t tiles_x = stride / 16;
        const uint32_t tile_rows = span / stride / 8;
        uint32_t s = 0;
        for (uint32_t ty = 0; ty < tile_rows; ty++) {
            for (uint32_t tx = 0; tx < tiles_x; tx++) {
                for (uint32_t r = 0; r < 8; r++) {
                    std::memcpy(&linear[(ty * 8 + r) * stride + tx * 16], &raw[s], 16);
                    s += 16;
                }
            }
        }
        src = linear.data();
    }

    for (int y = 0; y < p.height; y++) {
        const uint8_t* row = src + static_cast<size_t>(y) * stride;
        uint8_t* out = &rgba[static_cast<size_t>(y) * p.width * 4];
        for (int x = 0; x < p.width; x++, out += 4) {
            switch (p.format) {
            case GE_TFMT_8888:
                std::memcpy(out, row + x * 4, 4);
                break;
            case GE_TFMT_5650:
            case GE_TFMT_5551:
            case GE_TFMT_4444:
                color16(static_cast<uint16_t>(row[x * 2] | (row[x * 2 + 1] << 8)), p.format, out);
                break;
            case GE_TFMT_CLUT4: {
                const uint8_t b = row[x / 2];
                palette_entry(rdram, p, (x & 1) ? (b >> 4) : (b & 0xF), out);
                break;
            }
            case GE_TFMT_CLUT8:
                palette_entry(rdram, p, row[x], out);
                break;
            case GE_TFMT_CLUT16:
                palette_entry(rdram, p, static_cast<uint32_t>(row[x * 2] | (row[x * 2 + 1] << 8)), out);
                break;
            default: {  // CLUT32
                uint32_t v;
                std::memcpy(&v, row + x * 4, 4);
                palette_entry(rdram, p, v, out);
                break;
            }
            }
        }
    }
    return true;
}
