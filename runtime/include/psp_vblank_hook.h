#pragma once
// One optional callback run on the game thread at every vblank wait (sceDisplayWaitVblankStart*),
// after the vblank interrupt handlers. Set once at boot; empty = one relaxed atomic load per vblank.
#include <cstdint>

using PspVblankHook = void (*)(uint8_t* rdram);
void psp_vblank_hook_set(PspVblankHook hook);
void psp_vblank_hook_run(uint8_t* rdram);
