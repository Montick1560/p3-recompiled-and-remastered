#include "psp_ge_rtt.h"

#include <algorithm>

namespace {

constexpr uint32_t kFbWidth = 480, kFbHeight = 272;

bool in_vram(uint32_t addr) {
    return ((addr & 0x07FFFFFFu) & 0xFF800000u) == 0x04000000u;
}

uint32_t texel_bytes(int tex_fmt) {
    if (tex_fmt == 3) return 4;
    return (tex_fmt >= 0 && tex_fmt <= 2) ? 2 : 0;
}

uint32_t pixel_bytes(uint32_t fb_fmt) { return fb_fmt == 3 ? 4u : 2u; }

}  // namespace

GeRttHit ge_rtt_find(const GeRttTarget* targets, int count, uint32_t tex_addr, int tex_fmt,
                     uint32_t tex_bufw, int tex_h) {
    GeRttHit best{-1, 0, 0};
    const uint32_t tbytes = texel_bytes(tex_fmt);
    if (tbytes == 0 || !in_vram(tex_addr)) return best;
    const uint32_t off = tex_addr & 0x001FFFFFu;
    uint32_t best_delta = 0xFFFFFFFFu;
    for (int i = 0; i < count; ++i) {
        const GeRttTarget& t = targets[i];
        if (!t.valid || t.stride == 0) continue;
        const uint32_t pbytes = pixel_bytes(t.fb_fmt);
        const uint32_t row = t.stride * pbytes;
        if (pbytes != tbytes || off < t.key) continue;
        if (tex_h > 1 && tex_bufw * tbytes != row) continue;   // PPSSPP stride rule
        const uint32_t delta = off - t.key;
        const uint32_t y = delta / row, x = (delta % row) / pbytes;
        if (x >= kFbWidth || y >= 512) continue;
        if (y > 0 && y + static_cast<uint32_t>(tex_h) / 4 >= kFbHeight) continue;
        if (y > 32 && 0x04000000u + t.key > 0x04110000u) continue;
        if (delta >= best_delta) continue;
        best = {i, static_cast<int>(x), static_cast<int>(y)};
        best_delta = delta;
    }
    return best;
}

int ge_rtt_overlap(const GeRttTarget* targets, int count, uint32_t addr, uint32_t bytes) {
    if (bytes == 0 || !in_vram(addr)) return -1;
    const uint32_t lo = addr & 0x001FFFFFu, hi = lo + bytes;
    for (int i = 0; i < count; ++i) {
        const GeRttTarget& t = targets[i];
        if (!t.valid || t.stride == 0) continue;
        const uint32_t end = t.key + t.stride * pixel_bytes(t.fb_fmt) * kFbHeight;
        if (lo < end && hi > t.key) return i;
    }
    return -1;
}

uint32_t ge_rtt_tex_bytes(int tex_fmt, uint32_t tex_bufw, int tex_h) {
    static const uint8_t kBits[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};
    if (tex_fmt < 0 || tex_fmt > 10 || tex_h <= 0) return 0;
    const uint32_t w = tex_bufw ? tex_bufw : 1;
    return (w * static_cast<uint32_t>(tex_h) * kBits[tex_fmt] + 7) / 8;
}

GeRttBlit ge_rtt_blit_rect(int x, int y, int w, int h, int scale) {
    GeRttBlit b{true, 0, 0, 0, 0, 0, 0};
    const int vw = std::min(w, static_cast<int>(kFbWidth) - x);
    const int vh = std::min(h, static_cast<int>(kFbHeight) - y);
    if (x < 0 || y < 0 || vw <= 0 || vh <= 0 || scale <= 0) return b;
    b.empty = false;
    b.src_x0 = x * scale;
    b.src_x1 = (x + vw) * scale;
    b.src_y0 = (static_cast<int>(kFbHeight) - y) * scale;       // PSP row y (top of window)
    b.src_y1 = (static_cast<int>(kFbHeight) - y - vh) * scale;  // flipped on purpose
    b.dst_w = vw * scale;
    b.dst_h = vh * scale;
    return b;
}
