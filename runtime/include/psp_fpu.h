#pragma once
#include <cstdint>

// Allegrex FCR31 -> host FPU mode for the calling thread. Bit 24 (FS) makes the
// PSP flush denormal results to zero; on x86 this sets MXCSR.FTZ, on ARM64
// FPCR.FZ (PPSSPP ApplyHostRoundingMode). Rounding stays with the explicit
// conversions in the generated code. Called by emitted `ctc1 $31` code.
void psp_fpu_apply_fcr31(uint32_t fcr31);
