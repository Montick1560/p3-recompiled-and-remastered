// games/patapon3/runtime/hooks_main.cpp — Patapon 3 game module.
//
// Diagnostics only (no behaviour change): PSPRECOMP_P3_GMO_GUARD=1 wraps the
// GMO display-list relocators the model loader calls for every GE command it
// patches (FUN_08859AC0 jump/call targets, FUN_08859D2C vertex addresses,
// FUN_08859E78 texture addresses). a0 points at the BASE+address command pair
// being patched in place, a1/a2 are the loaded/file GMO descriptors; the
// patched pair must lie in the loaded model's GE region
// [a1->+0x28, a1->+0x28 + a2->+0x2C). A pair outside it means the list walker
// left its list and is rewriting unrelated heap memory.

#include "psp_game_module.h"
#include "hle/psp_hle.h"
#include "recomp.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void psp_print_host_backtrace(const char* tag);  // runtime/src/psp_backtrace.cpp

namespace {

uint32_t rd32(uint8_t* rdram, uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, rdram + (addr & 0x07FFFFFFu), 4);
    return v;
}

struct GuardSlot {
    uint32_t addr;
    const char* name;
    FuncPtr orig;
};
GuardSlot g_slots[3] = {
    {0x08859AC0u, "jump/call", nullptr},
    {0x08859D2Cu, "vertex", nullptr},
    {0x08859E78u, "texture", nullptr},
};
std::atomic<int> g_reports{0};
std::atomic<uint64_t> g_calls{0};

void guard_check(int slot, uint8_t* rdram, recomp_context* ctx) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t cmd = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t dst = static_cast<uint32_t>(ctx->r[5]);
    const uint32_t src = static_cast<uint32_t>(ctx->r[6]);
    const uint32_t base = rd32(rdram, dst + 0x28);
    const uint32_t size = rd32(rdram, src + 0x2C);
    if (cmd - base < size) return;  // inside the GE region (unsigned wrap)
    if (g_reports.fetch_add(1) >= 20) return;
    std::fprintf(stderr,
                 "[GMO-GUARD] %s reloc of pair at 0x%08X outside GE region [0x%08X,+0x%X) "
                 "dst=0x%08X src=0x%08X words=%08X %08X (call #%llu)\n",
                 g_slots[slot].name, cmd, base, size, dst, src, rd32(rdram, cmd),
                 rd32(rdram, cmd + 4), static_cast<unsigned long long>(g_calls.load()));
    psp_print_host_backtrace("gmo-guard");
}

template <int N>
void guard_wrapper(uint8_t* rdram, recomp_context* ctx) {
    guard_check(N, rdram, ctx);
    g_slots[N].orig(rdram, ctx);
}

void patapon3_register_hooks(uint8_t* /*rdram*/) {
    const char* e = std::getenv("PSPRECOMP_P3_GMO_GUARD");
    if (!(e && e[0] == '1')) return;
    const FuncPtr wrappers[3] = {guard_wrapper<0>, guard_wrapper<1>, guard_wrapper<2>};
    for (int i = 0; i < 3; i++) {
        g_slots[i].orig = RECOMP_LOOKUP(g_slots[i].addr);
        psp_dispatch_register(g_slots[i].addr, wrappers[i]);
    }
    std::fprintf(stderr, "[GMO-GUARD] watching the GMO display-list relocators\n");
}

void patapon3_on_boot_context(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
void patapon3_on_thread_start(uint8_t* /*rdram*/, uint32_t /*k0_addr*/) {}

const PspGameModule g_patapon3_module = {
    /* id              */ "patapon3",
    /* register_hooks  */ patapon3_register_hooks,
    /* on_boot_context */ patapon3_on_boot_context,
    /* on_thread_start */ patapon3_on_thread_start,
};

}  // namespace

const PspGameModule* psp_game_module() {
    return &g_patapon3_module;
}
