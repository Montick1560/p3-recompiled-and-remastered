#include "psp_fpu.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <xmmintrin.h>
#endif

void psp_fpu_apply_fcr31(uint32_t fcr31) {
    const bool ftz = (fcr31 & 0x01000000u) != 0;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    unsigned csr = _mm_getcsr();
    csr = ftz ? (csr | 0x8000u) : (csr & ~0x8000u);
    _mm_setcsr(csr);
#elif defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr = ftz ? (fpcr | (1ull << 24)) : (fpcr & ~(1ull << 24));
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#else
    (void)ftz;  // no portable flush-to-zero control
#endif
}
