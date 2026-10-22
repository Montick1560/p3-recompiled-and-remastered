#pragma once
// Is this function's code loaded at `addr`? Overlay banks share one address
// window, so a game hook registered on an overlay address must check, per
// call, that the loaded bank put the function it was written for there
// (another overlay, or another release of the game, may have other code at
// the same address). Compares the first `n` code words, read little-endian
// from guest memory with the 0x07FFFFFF mask. Pure.
#include <cstddef>
#include <cstdint>
#include <cstring>

inline bool psp_code_matches(const uint8_t* rdram, uint32_t addr, const uint32_t* words, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t w;
        std::memcpy(&w, rdram + ((addr + 4 * static_cast<uint32_t>(i)) & 0x07FFFFFFu), 4);
        if (w != words[i]) return false;
    }
    return true;
}
