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

// PSPRECOMP_P3_ANIM_TRACE=1 (diagnostic): FUN_088785A8 (a0 = pose cache, a1 = motion id,
// a2 = whole frame) calls the model's vtable +0x38 with the frame; log each distinct
// vtable method once so the pose path can be decompiled.
constexpr uint32_t kPoseCache = 0x088785A8u;
FuncPtr g_pose_cache = nullptr;
std::atomic<int> g_anim_reports{0};
uint32_t g_seen_methods[16] = {};

void pose_cache_trace(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t cache = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t model = rd32(rdram, cache + 4);
    const uint32_t vtbl = model ? rd32(rdram, model) : 0;
    const uint32_t method = vtbl ? rd32(rdram, vtbl + 0x38) : 0;
    bool seen = false;
    for (uint32_t m : g_seen_methods) seen |= (m == method);
    if (!seen && g_anim_reports.load() < 16) {
        g_seen_methods[g_anim_reports.fetch_add(1)] = method;
        std::fprintf(stderr,
                     "[ANIM] cache=0x%08X model=0x%08X vtbl=0x%08X +0x38=0x%08X motion=%u "
                     "frame=%d ra=0x%08X\n",
                     cache, model, vtbl, method, static_cast<unsigned>(ctx->r[5] & 0xFFFF),
                     static_cast<int>(ctx->r[6]), static_cast<uint32_t>(ctx->r[31]));
    }
    g_pose_cache(rdram, ctx);
}

// Smooth animation (PSPRECOMP_P3_ANIM_SMOOTH, on by default with the 60fps set): the motion
// player (a0, clock float +0x68, motion id +0x78) samples poses at a whole frame: the tick
// FUN_08878A18 and the draw FUN_08878B18 pass (int)+0x68 to the pose cache FUN_088785A8, whose
// model method (vtable +0x38, FUN_08879DF4) converts it back to float and hands it to the
// motion playhead setter FUN_089DAF74 (a0 = motion, f12 = time -> motion +0x1C), then
// evaluates with FUN_089DBD2C (a0 = GMO model). Its per-motion evaluator FUN_089DB1E0 rounds
// the playhead to a whole frame (floor(t + 0.5), 0x089DB9F0) unless model +0x28 has bit 4;
// only then are keys interpolated at the float time. At 60 fps the clock moves ~0.37 frames
// per vblank, so each pose showed for 2-3 vblanks. While a player tick/draw runs we store
// the player's real clock instead of the whole frame and set bit 4 around the evaluation.
// The clock itself is untouched, so frame-keyed gameplay events (hits, sounds) are
// unchanged; only what is drawn moves.
constexpr uint32_t kMotionTick = 0x08878A18u;
constexpr uint32_t kMotionDraw = 0x08878B18u;
constexpr uint32_t kPlayheadSet = 0x089DAF74u;
constexpr uint32_t kPoseEval = 0x089DBD2Cu;
constexpr uint32_t kModelInterpolate = 0x4u;  // model +0x28 flag tested at 0x089DB2B0
FuncPtr g_motion_tick = nullptr;
FuncPtr g_pose_eval = nullptr;
FuncPtr g_motion_draw = nullptr;
FuncPtr g_playhead_set = nullptr;
// Game threads run one at a time on the host (cooperative scheduler), but keep it per thread.
thread_local bool g_smooth_scope = false;
thread_local float g_smooth_time = 0.0f;

template <FuncPtr* Orig>
void motion_scope(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t clock = rd32(rdram, static_cast<uint32_t>(ctx->r[4]) + 0x68);
    float t;
    std::memcpy(&t, &clock, 4);
    const bool outer = g_smooth_scope;
    const float outer_time = g_smooth_time;
    g_smooth_scope = true;
    g_smooth_time = t;
    (*Orig)(rdram, ctx);
    g_smooth_scope = outer;
    g_smooth_time = outer_time;
}

void playhead_set_smooth(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t motion = static_cast<uint32_t>(ctx->r[4]);
    const float whole = ctx->f[12];
    g_playhead_set(rdram, ctx);
    // Only replace the value the player itself asked for (same whole frame), so a nested
    // caller with another frame keeps its own.
    if (g_smooth_scope && motion != 0 && whole == static_cast<float>(psp_f2i(g_smooth_time))) {
        std::memcpy(rdram + ((motion + 0x1C) & 0x07FFFFFFu), &g_smooth_time, 4);
    }
}

void pose_eval_smooth(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t model = static_cast<uint32_t>(ctx->r[4]);
    if (!g_smooth_scope || model == 0) {
        g_pose_eval(rdram, ctx);
        return;
    }
    uint8_t* flags = rdram + ((model + 0x28) & 0x07FFFFFFu);
    uint32_t f;
    std::memcpy(&f, flags, 4);
    if (f & kModelInterpolate) {
        g_pose_eval(rdram, ctx);
        return;
    }
    f |= kModelInterpolate;
    std::memcpy(flags, &f, 4);
    g_pose_eval(rdram, ctx);
    std::memcpy(&f, flags, 4);
    f &= ~kModelInterpolate;
    std::memcpy(flags, &f, 4);
}

void patapon3_register_hooks(uint8_t* /*rdram*/) {
    const char* smooth = std::getenv("PSPRECOMP_P3_ANIM_SMOOTH");
    const bool smooth_on = (smooth && smooth[0]) ? smooth[0] == '1' : psp_patch_set_active("60fps");
    if (smooth_on) {
        g_motion_tick = RECOMP_LOOKUP(kMotionTick);
        g_motion_draw = RECOMP_LOOKUP(kMotionDraw);
        g_playhead_set = RECOMP_LOOKUP(kPlayheadSet);
        g_pose_eval = RECOMP_LOOKUP(kPoseEval);
        psp_dispatch_register(kMotionTick, motion_scope<&g_motion_tick>);
        psp_dispatch_register(kMotionDraw, motion_scope<&g_motion_draw>);
        psp_dispatch_register(kPlayheadSet, playhead_set_smooth);
        psp_dispatch_register(kPoseEval, pose_eval_smooth);
        std::fprintf(stderr, "[P3] smooth animation: poses sampled at the fractional frame\n");
    }
    const char* trace = std::getenv("PSPRECOMP_P3_ANIM_TRACE");
    if (trace && trace[0] == '1') {
        g_pose_cache = RECOMP_LOOKUP(kPoseCache);
        psp_dispatch_register(kPoseCache, pose_cache_trace);
        std::fprintf(stderr, "[ANIM] tracing the pose path (FUN_088785A8)\n");
    }
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
