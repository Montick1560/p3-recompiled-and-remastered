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

// NID 0x469989AD is missing from data/niddb, so the generated import table
// (issue #40) carries the walker's canonical fallback name "NID_0x469989AD"
// — handlers for unresolved NIDs register under exactly that name.
// Identification: PPSSPP Core/HLE/scePower.cpp maps 0x469989AD to
// scePowerSetClockFrequency630 (same handler as scePowerSetClockFrequency).
// Returning SCE_OK without touching clocks is the correct no-op.
static void hle_NID_0x469989AD(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// scePowerRegisterCallback (PPSSPP scePower.cpp). 16 user slots; -1 picks the
// first free slot and returns its index. Slots 16..31 are kernel-only.
// No PPSSPP-style callback notify in this runtime.
static constexpr int kPowerCbSlots = 16;
static constexpr int kPowerCbSlotsPrivate = 32;
static constexpr int32_t kPowerTakenSlot = (int32_t)0x80000020;
static constexpr int32_t kPowerSlotsFull = (int32_t)0x80000022;
static constexpr int32_t kPowerInvalidCb = (int32_t)0x80000100;
static constexpr int32_t kPowerInvalidSlot = (int32_t)0x80000102;
static constexpr int32_t kPowerPrivRequired = (int32_t)0x80000023;

static int32_t g_power_cb_slots[kPowerCbSlots];

static void hle_scePowerRegisterCallback(
    uint8_t* rdram, recomp_context* ctx
) {
    const int slot = ctx->r[4];
    const int cb_id = ctx->r[5];
    (void)rdram;
    if (slot < -1 || slot >= kPowerCbSlotsPrivate) {
        ctx->r[2] = kPowerInvalidSlot;
        return;
    }
    if (slot >= kPowerCbSlots) {
        ctx->r[2] = kPowerPrivRequired;
        return;
    }
    if (cb_id == 0) {
        ctx->r[2] = kPowerInvalidCb;
        return;
    }

    int retval = -1;
    if (slot == -1) {
        for (int i = 0; i < kPowerCbSlots; i++) {
            if (g_power_cb_slots[i] == 0 && retval == -1) {
                g_power_cb_slots[i] = cb_id;
                retval = i;
            }
        }
        if (retval == -1) {
            ctx->r[2] = kPowerSlotsFull;
            return;
        }
    } else if (g_power_cb_slots[slot] == 0) {
        g_power_cb_slots[slot] = cb_id;
        retval = 0;
    } else {
        ctx->r[2] = kPowerTakenSlot;
        return;
    }
    ctx->r[2] = retval;
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
    psp_hle_register("NID_0x469989AD",
                      hle_NID_0x469989AD);
    psp_hle_register("scePowerRegisterCallback",
                      hle_scePowerRegisterCallback);
}
