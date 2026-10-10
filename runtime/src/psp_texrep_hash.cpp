#include "psp_texrep_hash.h"

#include "xxhash.h"

#include <algorithm>
#include <cstdio>

namespace {
constexpr uint32_t kSeed = 0xBACD7814u;
constexpr int kBpp[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};

uint32_t hash_bytes(const uint8_t* p, uint32_t n, bool xxh32) {
    return xxh32 ? XXH32(p, n, kSeed) : static_cast<uint32_t>(XXH64(p, n, kSeed));
}
}  // namespace

int ge_texrep_bpp(int fmt) {
    return (fmt >= 0 && fmt <= 10) ? kBpp[fmt] : 0;
}

int ge_texrep_bufw(uint32_t texbufwidth0, int fmt) {
    const int bpp = ge_texrep_bpp(fmt);
    if (bpp == 0) return 0;
    const uint32_t mask = fmt >= 8 ? 0x7FFu : (0x7FFu & ~static_cast<uint32_t>((8 * 16) / bpp - 1));
    const int bufw = static_cast<int>(texbufwidth0 & mask);
    return bufw == 0 ? (8 * 16) / bpp : bufw;
}

uint16_t ge_texrep_max_seen_v(bool through, uint16_t draw_max_v) {
    if (!through) return 512;
    if (draw_max_v == 0) return 0;
    return std::max<uint16_t>(272, draw_max_v);
}

int ge_texrep_hash_height(int h, int fmt, bool swizzled, uint16_t max_seen_v) {
    const uint16_t seen = (h == 512 && swizzled && fmt == 4) ? 512 : max_seen_v;
    return (h == 512 && seen < 512 && seen != 0) ? static_cast<int>(seen) : h;
}

uint32_t ge_texrep_data_hash(const uint8_t* tex, uint64_t avail, int bufw, int w, int h,
                             int fmt, float reduce, bool xxh32) {
    const uint32_t bpp = static_cast<uint32_t>(ge_texrep_bpp(fmt));
    reduce = std::clamp(reduce, 0.0f, 1.0f);
    if (bufw <= w) {
        // Contiguous. PPSSPP: u32 sizeInRAM = (bpp * totalPixels) / 8 * reduce.
        const uint32_t total = static_cast<uint32_t>(bufw * h + (w - bufw));
        const uint32_t size = static_cast<uint32_t>((bpp * total) / 8 * reduce);
        if (avail < size) return 0;
        return hash_bytes(tex, size, xxh32);
    }
    // Strided: hash each row, combine as result * 11 ^ row.
    const uint32_t row = static_cast<uint32_t>((bpp * static_cast<uint32_t>(w)) / 8 * reduce);
    const uint32_t stride = (bpp * static_cast<uint32_t>(bufw)) / 8;
    const uint64_t span = h > 0 ? static_cast<uint64_t>(h - 1) * stride + row : 0;
    if (avail < span) return 0;
    uint32_t result = 0;
    for (int y = 0; y < h; y++) {
        result = (result * 11u) ^ hash_bytes(tex + static_cast<size_t>(y) * stride, row, xxh32);
    }
    return result;
}

uint64_t ge_texrep_cachekey(uint32_t addr, int fmt, uint16_t dim, uint32_t cluthash) {
    uint64_t key = (static_cast<uint64_t>(addr & 0x3FFFFFFFu) << 32) | dim;
    if (fmt & 4) key ^= cluthash;
    return key;
}

std::string ge_texrep_key_name(const GeTexrepKey& k) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx%08x",
                  static_cast<unsigned long long>(k.cachekey), k.hash);
    return buf;
}
