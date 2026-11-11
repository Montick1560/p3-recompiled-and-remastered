#pragma once
// When a GE display list must stop at its stall address. PPSSPP (GPU/GPUCommon.cpp) keeps list pc and stall
// masked to 28 bits (EnqueueList, UpdateStall) and stalls exactly at pc == stall: an uncached list address
// (0x4xxxxxxx) and a JUMP target (always 28 bits) must compare equal.
#include <cstdint>

inline bool ge_at_stall(uint32_t pc, uint32_t stall) {
    return stall != 0 && (pc & 0x0FFFFFFFu) == (stall & 0x0FFFFFFFu);
}

// A list parked at the stall it was drawn with stays parked only if the game did not move the stall meanwhile.
inline bool ge_stall_still_current(uint32_t stall_drawn_with, uint32_t stall_now) {
    return (stall_drawn_with & 0x0FFFFFFFu) == (stall_now & 0x0FFFFFFFu);
}
