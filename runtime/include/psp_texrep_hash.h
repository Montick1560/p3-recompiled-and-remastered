#pragma once
// PPSSPP texture-replacement keys (GPU/Common/TextureReplacer.cpp ComputeHash,
// TextureCacheCommon.h CacheKey). Pure functions of plain values so packs made
// for PPSSPP match bit for bit. Spec: docs/superpowers/specs/
// 2026-10-10-m3c-hd-textures-design.md, "How PPSSPP keys a replacement".
#include <cstdint>
#include <string>

/// Bits per pixel of a GE texture format (0..10); 0 for invalid formats.
int ge_texrep_bpp(int fmt);

/// Level-0 buffer width as PPSSPP's GetTextureBufw: TEXBUFWIDTH0 aligned down
/// to 16 bytes (DXT: raw & 0x7FF); 0 becomes one 16-byte row.
int ge_texrep_bufw(uint32_t texbufwidth0, int fmt);

/// PPSSPP UpdateMaxSeenV for a new texture: through mode uses the draw's
/// largest V (texels), at least 272, or 0 when unknown; transform mode 512.
uint16_t ge_texrep_max_seen_v(bool through, uint16_t draw_max_v);

/// Rows ComputeHash hashes: a 512-tall texture is clipped to max_seen_v
/// (when 0 < max_seen_v < 512), except swizzled CLUT4 atlases.
int ge_texrep_hash_height(int h, int fmt, bool swizzled, uint16_t max_seen_v);

/// ComputeHash data hash (xxh64 truncated to 32 bits, or xxh32), seed
/// 0xBACD7814. `avail` = bytes readable from `tex`; a texture past it gives 0.
uint32_t ge_texrep_data_hash(const uint8_t* tex, uint64_t avail, int bufw, int w, int h,
                             int fmt, float reduce, bool xxh32);

/// TexCacheEntry::CacheKey: (addr & 0x3FFFFFFF) << 32 | dim, xor cluthash for
/// CLUT formats (4..7).
uint64_t ge_texrep_cachekey(uint32_t addr, int fmt, uint16_t dim, uint32_t cluthash);

struct GeTexrepKey {
    uint64_t cachekey;
    uint32_t hash;
    bool operator==(const GeTexrepKey& o) const { return cachekey == o.cachekey && hash == o.hash; }
};

/// "%016llx%08x": the file/ini name of a key (level 0).
std::string ge_texrep_key_name(const GeTexrepKey& k);
