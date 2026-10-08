// Local shim replacing PPSSPP's ppsspp_config.h for the standalone
// ATRAC3/ATRAC3plus decoder: only the PPSSPP_ARCH() probes the decoder uses.
#pragma once

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define AT3_ARCH_X86_FAMILY 1
#else
#define AT3_ARCH_X86_FAMILY 0
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define AT3_ARCH_SSE2 1
#else
#define AT3_ARCH_SSE2 0
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#define AT3_ARCH_ARM_NEON 1
#else
#define AT3_ARCH_ARM_NEON 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define AT3_ARCH_ARM64 1
#else
#define AT3_ARCH_ARM64 0
#endif

#define AT3_ARCH_X86 AT3_ARCH_X86_FAMILY
#define AT3_ARCH_AMD64 AT3_ARCH_X86_FAMILY
#define PPSSPP_ARCH(x) (AT3_ARCH_##x)
