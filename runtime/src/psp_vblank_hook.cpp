#include "psp_vblank_hook.h"

#include <atomic>

namespace {
std::atomic<PspVblankHook> g_hook{nullptr};
}

void psp_vblank_hook_set(PspVblankHook hook) { g_hook.store(hook, std::memory_order_release); }

void psp_vblank_hook_run(uint8_t* rdram) {
    if (PspVblankHook h = g_hook.load(std::memory_order_relaxed)) h(rdram);
}
