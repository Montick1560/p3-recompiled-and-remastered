#pragma once

#include <cstdint>

/// Per-channel color write enables from the GE pixel masks (MASKRGB,
/// MASKALPHA: a set bit means "do not write"), PPSSPP ConvertMaskState
/// semantics for a fixed-function backend: a channel is written when its
/// whole byte is unmasked, not when fully masked, and a partial mask uses
/// PPSSPP's heuristic (written when at least 128 of its bits' weight is
/// writable). Out: write[0..3] = R, G, B, A.
inline void ge_color_write_mask(uint32_t mask_rgb, uint32_t mask_alpha, bool write[4]) {
    const uint32_t color_mask = ~((mask_rgb & 0xFFFFFFu) | ((mask_alpha & 0xFFu) << 24));
    for (int i = 0; i < 4; i++) {
        const uint32_t ch = (color_mask >> (i * 8)) & 0xFFu;
        write[i] = ch >= 128;
    }
}
