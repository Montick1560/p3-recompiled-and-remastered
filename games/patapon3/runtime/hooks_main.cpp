// games/patapon3/runtime/hooks_main.cpp — Patapon 3 game module.
//
// Diagnostics (no behaviour change): PSPRECOMP_P3_GMO_GUARD=1 wraps the
// GMO display-list relocators the model loader calls for every GE command it
// patches (FUN_08859AC0 jump/call targets, FUN_08859D2C vertex addresses,
// FUN_08859E78 texture addresses). a0 points at the BASE+address command pair
// being patched in place, a1/a2 are the loaded/file GMO descriptors; the
// patched pair must lie in the loaded model's GE region
// [a1->+0x28, a1->+0x28 + a2->+0x2C). A pair outside it means the list walker
// left its list and is rewriting unrelated heap memory.
//
// 60 fps (PSPRECOMP_PATCHES=60fps, games/patapon3/game.toml): the game reads
// input once per frame in FUN_08851EC8 (a0 = input owner: +16 enabled byte,
// +32 pad block, +40 input object) and counts in frames everywhere:
// - FUN_0883D434 fills the pad block: held/released/repeat/pressed words for
//   three views (+96 all samples, +112 latest, +128 older) and per-button
//   held counters (+144, +240) with a 20-frame repeat delay, 6-frame repeat;
// - FUN_08852628 builds the input object from it: held +0, pressed +4,
//   released +8, repeat +12 (its own per-button counters at +104, delay +180,
//   interval +184), the same four with circle/cross swapped at +16..+28, and
//   the latest/older views' held/pressed at +196/+200 and +212/+216.
// Written for 30 fps, so at 60 menus scroll twice as fast (a 120 ms tap moved
// the language cursor two rows). The whole update runs at most once per two
// vblanks; a frame in between keeps the held words and clears the edge words,
// so each press, release and repeat reaches one frame, at the 30 fps timing.
#include "psp_game_module.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_intr.h"
#include "psp_patch_sets.h"
#include "psp_vblank_clock.h"
#include "psp_vblank_pacer.h"
#include "recomp.h"

#include <atomic>
#include <initializer_list>
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

constexpr uint32_t kInputUpdate = 0x08851EC8u;
FuncPtr g_input_update = nullptr;
VblankPacer g_input_pacer;

void clear_words(uint8_t* rdram, uint32_t base, std::initializer_list<uint32_t> offsets) {
    for (uint32_t off : offsets) std::memset(rdram + ((base + off) & 0x07FFFFFFu), 0, 4);
}

void input_update_30hz(uint8_t* rdram, recomp_context* ctx) {
    if (g_input_pacer.due(psp_vblank_index(psp_display_elapsed_us()), 2)) {
        g_input_update(rdram, ctx);
        return;
    }
    const uint32_t owner = static_cast<uint32_t>(ctx->r[4]);
    if (rdram[(owner + 16) & 0x07FFFFFFu] == 0) return;  // the update skips it too
    clear_words(rdram, rd32(rdram, owner + 32),
                {100, 104, 108, 116, 120, 124, 132, 136, 140});
    clear_words(rdram, owner + 40, {4, 8, 12, 20, 24, 28, 200, 216});
}

void patapon3_register_hooks(uint8_t* /*rdram*/) {
    if (psp_patch_set_active("60fps")) {
        g_input_update = RECOMP_LOOKUP(kInputUpdate);
        psp_dispatch_register(kInputUpdate, input_update_30hz);
        std::fprintf(stderr, "[P3] 60fps: input update paced to 30 Hz\n");
    }
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
