#pragma once
// Interrupt-manager HLE glue: the shared sub-interrupt table and the
// vblank dispatch that runs the game's enabled PSP_VBLANK_INT handlers.

#include "psp_subintr.h"
#include "recomp.h"

#include <cstdint>

PspSubIntrTable& psp_subintr_table();

/// Microseconds since the display clock started (first use). Shared by the
/// vblank waits, sceDisplayGetVcount and the vblank interrupt dispatch.
int64_t psp_display_elapsed_us();

/// Run every enabled vblank sub-interrupt handler once for the current
/// display frame (60 Hz, wall clock). Called from the vblank waits; extra
/// calls within the same frame are no-ops, so several waiting threads do
/// not fire a frame's handlers twice. Guest registers are preserved.
void psp_intr_dispatch_vblank(uint8_t* rdram, recomp_context* ctx);
