#pragma once
// Code-overlay banks (M2). Each overlay (e.g. Patapon 3's OL_Title/OL_Mission/
// OL_Azito) is recompiled as a bank of functions for the shared overlay
// window (`recompile --bank`); banks self-register at static init. When the
// main dispatch misses an address inside a window, the runtime resolves it
// against the bank whose id matches the MWo3 header currently in memory,
// after checking (once per activation) that the loaded code is exactly the
// code the bank was compiled from.

#include <cstdint>

#include "recomp.h"  // FuncPtr, PspOverlayBank, psp_overlay_register_bank

enum class PspOverlayStatus {
    NotInWindow,   // address is outside every registered overlay window
    NoBank,        // window holds no overlay, or one with no compiled bank
    HashMismatch,  // loaded overlay code differs from the compiled bank
    NotFound,      // bank is active but has no function at this address
    Found,
};

/// Resolve `vaddr` against the overlay banks; on Found, `*out` is set.
PspOverlayStatus psp_overlay_resolve(uint8_t* rdram, uint32_t vaddr, FuncPtr* out);

/// The runtime's guest RAM, used by the dispatch miss path (set once at boot).
void psp_overlay_set_rdram(uint8_t* rdram);
uint8_t* psp_overlay_rdram();

/// Test hook: forget registered banks and the active-bank cache.
void psp_overlay_reset_for_tests();
