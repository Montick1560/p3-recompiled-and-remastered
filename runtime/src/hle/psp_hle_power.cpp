#include "hle/psp_hle.h"
#include "recomp.h"

// ---- HLE Functions ----
// Patapon only imports one scePower NID: unknown_EBD177D6.
// sceKernelPowerTick is under sceSuspendForUser, registered
// by psp_hle_register_utility().

// The scePower NID 0xEBD177D6 is listed as "unknown_EBD177D6"
// in the stub table. Likely scePowerGetBatteryTemp or similar.
static void hle_unknown_EBD177D6(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

// ---- Registration ----

void psp_hle_register_power() {
    psp_hle_register("unknown_EBD177D6",
                      hle_unknown_EBD177D6);
}
