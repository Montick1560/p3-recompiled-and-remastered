#include "hle/psp_hle.h"
#include "hle/psp_hle_volatile_mem.h"
#include "recomp.h"

#include <mutex>

// ---- HLE Functions ----
// Patapon only imports one scePower NID: 0xEBD177D6.
// sceKernelPowerTick is under sceSuspendForUser, registered
// by psp_hle_register_utility().

// NID 0xEBD177D6 is missing from data/niddb, so the generated import table
// (issue #40) carries the walker's canonical fallback name "NID_0xEBD177D6"
// — handlers for unresolved NIDs register under exactly that name.
// Identification: PPSSPP Core/HLE/scePower.cpp maps 0xEBD177D6 to
// scePowerSetClockFrequency (exported as "scePower_EBD177D6", the 3.71+
// alias). Returning SCE_OK without touching clocks is the correct no-op.
static void hle_NID_0xEBD177D6(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Volatile memory (sceSuspendForUser) ----
// The game takes the 4 MB volatile region at 0x08400000 for one of its heaps.
// A no-op TryLock left the out-pointers unwritten and the main thread spun
// forever right after boot. Protocol: hle/psp_hle_volatile_mem.h.

static std::mutex g_volatile_mutex;
static PspVolatileMem g_volatile_mem;

static void hle_sceKernelVolatileMemTryLock(
    uint8_t* rdram, recomp_context* ctx
) {
    std::lock_guard<std::mutex> lock(g_volatile_mutex);
    ctx->r[2] = static_cast<int32_t>(psp_volatile_mem_lock(
        g_volatile_mem, rdram, ctx->r[4],
        static_cast<uint32_t>(ctx->r[5]), static_cast<uint32_t>(ctx->r[6])));
}

static void hle_sceKernelVolatileMemUnlock(
    uint8_t* rdram, recomp_context* ctx
) {
    std::lock_guard<std::mutex> lock(g_volatile_mutex);
    ctx->r[2] = static_cast<int32_t>(
        psp_volatile_mem_unlock(g_volatile_mem, ctx->r[4]));
    (void)rdram;
}

// ---- Registration ----

void psp_hle_register_power() {
    psp_hle_register("sceKernelVolatileMemTryLock",
                      hle_sceKernelVolatileMemTryLock);
    psp_hle_register("sceKernelVolatileMemUnlock",
                      hle_sceKernelVolatileMemUnlock);
    psp_hle_register("NID_0xEBD177D6",
                      hle_NID_0xEBD177D6);
}
