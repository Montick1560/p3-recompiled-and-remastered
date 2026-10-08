#pragma once
// PSP display clock: vblank at 60000/1001 Hz (59.94), as integer math on
// the elapsed microseconds since boot so long sessions never drift.
// Frame k starts at ceil(k * 1001000000 / 60000) us.

#include <cstdint>

constexpr int64_t PSP_VBLANK_NUM_US = 1001000000;  // 1001 s in us ...
constexpr int64_t PSP_VBLANK_DEN = 60000;          // ... per 60000 frames

/// Index of the display frame containing `elapsed_us`.
constexpr int64_t psp_vblank_index(int64_t elapsed_us) {
    return elapsed_us * PSP_VBLANK_DEN / PSP_VBLANK_NUM_US;
}

/// Microseconds from `elapsed_us` to the start of the next frame (>= 1).
constexpr int64_t psp_us_until_next_vblank(int64_t elapsed_us) {
    const int64_t next = psp_vblank_index(elapsed_us) + 1;
    const int64_t next_start =
        (next * PSP_VBLANK_NUM_US + PSP_VBLANK_DEN - 1) / PSP_VBLANK_DEN;
    return next_start - elapsed_us;
}
