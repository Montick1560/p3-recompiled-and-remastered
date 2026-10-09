#pragma once

#include <cstdint>

/// SceKernelSysClock (64-bit microsecond count) split as
/// sceKernelSysClock2USec does in PPSSPP (Core/HLE/sceKernelTime.cpp):
/// `high` = clock / 1000000 (seconds), `low` = clock % 1000000, both
/// truncated to 32 bits.
inline void psp_sysclock_split(uint64_t clock, uint32_t& high, uint32_t& low) {
    high = static_cast<uint32_t>(clock / 1000000ULL);
    low = static_cast<uint32_t>(clock % 1000000ULL);
}
