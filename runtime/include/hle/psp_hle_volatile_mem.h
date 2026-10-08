#pragma once
// PSP volatile-memory lock protocol (sceSuspendForUser
// sceKernelVolatileMem{Lock,TryLock,Unlock}). Oracle: PPSSPP
// Core/HLE/scePower.cpp KernelVolatileMemLock / KernelVolatileMemUnlock.
// The volatile region is always 0x08400000..0x08800000 (4 MB, below the
// user module). Pure protocol logic, unit-tested standalone by
// tests/test_volatile_mem.cpp (no SDL/GL/scheduler deps).

#include <cstdint>

#include "recomp.h"  // psp_mem_write (masked, NULL-page guarded)

constexpr uint32_t PSP_VOLATILE_MEM_ADDR = 0x08400000U;
constexpr uint32_t PSP_VOLATILE_MEM_SIZE = 0x00400000U;

constexpr uint32_t PSP_VMEM_ERROR_INVALID_MODE = 0x80000107U;  // SCE_KERNEL_ERROR_INVALID_MODE
constexpr uint32_t PSP_VMEM_ERROR_NOT_LOCKED   = 0x800201AEU;  // SCE_KERNEL_ERROR_SEMA_OVF
constexpr uint32_t PSP_VMEM_ERROR_IN_USE       = 0x802B0200U;  // SCE_KERNEL_ERROR_POWER_VMEM_IN_USE

struct PspVolatileMem {
    bool locked = false;
};

/// Try to take the volatile region. On success writes the base address to
/// *paddr and the size to *psize (each skipped if the pointer is invalid).
inline uint32_t psp_volatile_mem_lock(PspVolatileMem& vm, uint8_t* rdram,
                                      int32_t type, uint32_t paddr, uint32_t psize) {
    if (type != 0) return PSP_VMEM_ERROR_INVALID_MODE;
    if (vm.locked) return PSP_VMEM_ERROR_IN_USE;
    psp_mem_write<uint32_t>(rdram, paddr, PSP_VOLATILE_MEM_ADDR);
    psp_mem_write<uint32_t>(rdram, psize, PSP_VOLATILE_MEM_SIZE);
    vm.locked = true;
    return 0;
}

inline uint32_t psp_volatile_mem_unlock(PspVolatileMem& vm, int32_t type) {
    if (type != 0) return PSP_VMEM_ERROR_INVALID_MODE;
    if (!vm.locked) return PSP_VMEM_ERROR_NOT_LOCKED;
    vm.locked = false;
    return 0;
}
