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
#include "p3_achievements.h"
#include "psp_achievements_run.h"
#include "psp_anim_blend.h"
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
#include <string>
#include <vector>

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
// Motions whose playhead the player set inside the current tick/draw scope, with the whole frame
// it asked for (FUN_089DAF74's f12). Saved/restored by motion_scope like the clock.
struct ScopeMotions {
    uint32_t addr[8];
    float whole[8];
    int n;
};
thread_local ScopeMotions g_scope_motions = {};

template <FuncPtr* Orig>
void motion_scope(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t clock = rd32(rdram, static_cast<uint32_t>(ctx->r[4]) + 0x68);
    float t;
    std::memcpy(&t, &clock, 4);
    const bool outer = g_smooth_scope;
    const float outer_time = g_smooth_time;
    const ScopeMotions outer_motions = g_scope_motions;
    g_smooth_scope = true;
    g_smooth_time = t;
    g_scope_motions.n = 0;
    (*Orig)(rdram, ctx);
    g_smooth_scope = outer;
    g_smooth_time = outer_time;
    g_scope_motions = outer_motions;
}

// PSPRECOMP_P3_ANIM_SMOOTH_DIAG=interp (A/B only): the earlier smooth mode — the player's
// fractional clock as the playhead plus the model's interpolate flag (see pose_eval_interp).
bool g_diag_interp = false;

void playhead_set_smooth(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t motion = static_cast<uint32_t>(ctx->r[4]);
    const float whole = ctx->f[12];
    g_playhead_set(rdram, ctx);
    if (g_smooth_scope && motion != 0 && g_scope_motions.n < 8) {
        g_scope_motions.addr[g_scope_motions.n] = motion;
        g_scope_motions.whole[g_scope_motions.n] = whole;
        g_scope_motions.n++;
    }
    // Only replace the value the player itself asked for (same whole frame), so a nested
    // caller with another frame keeps its own.
    if (g_diag_interp && g_smooth_scope && motion != 0 && whole == static_cast<float>(psp_f2i(g_smooth_time))) {
        std::memcpy(rdram + ((motion + 0x1C) & 0x07FFFFFFu), &g_smooth_time, 4);
    }
}

// PSPRECOMP_P3_ANIM_PROBE=<dir> (diagnostic): when <dir>/probe.trigger exists, the next model
// evaluated in scope with a fractional clock is evaluated three times — playheads whole (a),
// whole + 1 (b), and the current smooth mode (mid: the two-sample blend, or the interpolate-flag
// path with PSPRECOMP_P3_ANIM_SMOOTH_DIAG=interp) — and each bone block is written to
// <dir>/probe_<n>_{a,b,mid}.bin. The model keeps the mid result.
std::string g_probe_dir;
int g_probe_seq = 0;

// The trigger file may hold a minimum bone count and a skip count ("10 2": the third model in
// scope with >= 10 bones and a fractional clock); default "0 0" = the first such model.
int g_probe_min = -1;
int g_probe_skip = 0;

bool probe_due(uint32_t count, float alpha) {
    static int calls = 0;
    if (!g_probe_dir.empty() && g_probe_min < 0 && (++calls & 255) == 0) {
        const std::string trig = g_probe_dir + "/probe.trigger";
        if (std::FILE* f = std::fopen(trig.c_str(), "rb")) {
            int min = 0, skip = 0;
            if (std::fscanf(f, "%d %d", &min, &skip) < 1) min = 0;
            std::fclose(f);
            std::remove(trig.c_str());
            g_probe_min = min;
            g_probe_skip = skip;
        }
    }
    if (g_probe_min < 0 || static_cast<int>(count) < g_probe_min || !(alpha > 0.0f && alpha < 1.0f)) return false;
    if (g_probe_skip-- > 0) return false;
    g_probe_min = -1;
    return true;
}

void probe_dump(uint8_t* rdram, uint32_t bones, uint32_t count, const char* tag) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/probe_%d_%s.bin", g_probe_dir.c_str(), g_probe_seq, tag);
    if (std::FILE* f = std::fopen(path, "wb")) {
        std::fwrite(rdram + (bones & 0x07FFFFFFu), 1, count * 0x100u, f);
        std::fclose(f);
    }
}

// Runs the evaluation with every scope motion's playhead at `t_of(i)`, flag as given.
template <typename F>
void eval_at(uint8_t* rdram, recomp_context* ctx, const recomp_context& args, F t_of, bool flag) {
    const uint32_t model = static_cast<uint32_t>(args.r[4]);
    for (int i = 0; i < g_scope_motions.n; i++) {
        const float t = t_of(i);
        std::memcpy(rdram + ((g_scope_motions.addr[i] + 0x1C) & 0x07FFFFFFu), &t, 4);
    }
    uint8_t* flags = rdram + ((model + 0x28) & 0x07FFFFFFu);
    uint32_t f0;
    std::memcpy(&f0, flags, 4);
    const uint32_t f1 = flag ? (f0 | kModelInterpolate) : (f0 & ~kModelInterpolate);
    std::memcpy(flags, &f1, 4);
    *ctx = args;
    g_pose_eval(rdram, ctx);
    std::memcpy(flags, &f0, 4);
}

// The earlier smooth mode (PSPRECOMP_P3_ANIM_SMOOTH_DIAG=interp): evaluate at the fractional
// playhead with model +0x28 bit 4 set. FUN_089DB1E0 then interpolates some channels and not
// others (the barracks Hero: rotations at the fraction, translations held at the whole frame),
// which gives poses the game never shows — the Hero rose off the ground.
void pose_eval_interp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t model = static_cast<uint32_t>(ctx->r[4]);
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

// Per-word blend kinds of one 0x100-byte GMO bone block (FUN_089DB1E0 outputs), from
// anim_probe.py over six barracks models incl. the Hero (hero-ground plan, Evidence): +0x50
// translation, +0x60 rotation quaternion, +0x70 scale (vec4 each) are blended; +0x4C (not a
// float) and the rest are kept.
constexpr uint8_t kBoneKinds[64] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// The shipped smooth mode for one GMO evaluation in a player tick/draw scope.
void pose_eval_blend(uint8_t* rdram, recomp_context* ctx, uint32_t model, uint16_t count, float alpha) {
    if (!g_smooth_scope || model == 0 || g_scope_motions.n == 0) {
        g_pose_eval(rdram, ctx);
        return;
    }
    if (g_diag_interp) {
        pose_eval_interp(rdram, ctx);
        return;
    }
    if ((rd32(rdram, model + 0x28) & kModelInterpolate) || !(alpha > 0.0f && alpha < 1.0f)) {
        g_pose_eval(rdram, ctx);  // the game interpolates this model itself, or on a whole frame
        return;
    }
    // Next whole frame of every motion the player set at its own frame; any other motion (set by
    // a nested caller at another frame) and a one-shot at its end are the same in both samples.
    const int32_t whole = psp_f2i(g_smooth_time);
    float next[8];
    bool any = false;
    for (int i = 0; i < g_scope_motions.n; i++) {
        const uint32_t m = g_scope_motions.addr[i];
        const float w = g_scope_motions.whole[i];
        next[i] = w;
        if (w != static_cast<float>(whole)) continue;
        float start, end;
        std::memcpy(&start, rdram + ((m + 0x10) & 0x07FFFFFFu), 4);
        std::memcpy(&end, rdram + ((m + 0x14) & 0x07FFFFFFu), 4);
        int16_t loop;
        std::memcpy(&loop, rdram + ((m + 0x0E) & 0x07FFFFFFu), 2);
        const int32_t n = psp_anim_next_frame(whole, start, end, loop != 0);
        if (n >= 0 && n != whole) {
            next[i] = static_cast<float>(n);
            any = true;
        }
    }
    if (!any) {
        g_pose_eval(rdram, ctx);
        return;
    }
    // Sample b (next frame) first and the original whole frame last, so everything else the
    // evaluator writes (material colours, morph weights, playheads) ends as the original left it.
    const uint32_t bones = rd32(rdram, model + 4);
    const size_t words = static_cast<size_t>(count) * 64;
    thread_local std::vector<float> sample_b, sample_a;
    sample_b.resize(words);
    sample_a.resize(words);
    const recomp_context args = *ctx;
    eval_at(rdram, ctx, args, [&](int i) { return next[i]; }, false);
    std::memcpy(sample_b.data(), rdram + (bones & 0x07FFFFFFu), words * 4);
    eval_at(rdram, ctx, args, [](int i) { return g_scope_motions.whole[i]; }, false);
    std::memcpy(sample_a.data(), rdram + (bones & 0x07FFFFFFu), words * 4);
    for (uint32_t bone = 0; bone < count; bone++) {
        psp_blend_pose(sample_a.data() + bone * 64, sample_b.data() + bone * 64, kBoneKinds, 64, alpha);
    }
    std::memcpy(rdram + (bones & 0x07FFFFFFu), sample_a.data(), words * 4);
}

void pose_eval_smooth(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t model = static_cast<uint32_t>(ctx->r[4]);
    uint16_t count = 0;
    if (model != 0) std::memcpy(&count, rdram + ((model + 0x18) & 0x07FFFFFFu), 2);
    const float alpha = g_smooth_time - static_cast<float>(psp_f2i(g_smooth_time));
    if (model != 0 && g_smooth_scope && g_scope_motions.n > 0 && probe_due(count, alpha)) {
        const uint32_t bones = rd32(rdram, model + 4);
        const recomp_context args = *ctx;
        eval_at(rdram, ctx, args, [](int i) { return g_scope_motions.whole[i]; }, false);
        probe_dump(rdram, bones, count, "a");
        eval_at(rdram, ctx, args, [](int i) { return g_scope_motions.whole[i] + 1.0f; }, false);
        probe_dump(rdram, bones, count, "b");
        *ctx = args;
        pose_eval_blend(rdram, ctx, model, count, alpha);
        probe_dump(rdram, bones, count, "mid");
        char path[512];
        std::snprintf(path, sizeof path, "%s/probe_%d.txt", g_probe_dir.c_str(), g_probe_seq++);
        if (std::FILE* f = std::fopen(path, "w")) {
            std::fprintf(f, "model=0x%08X bones=0x%08X count=%u alpha=%.4f clock=%.4f motions=%d\n",
                         model, bones, count, alpha, g_smooth_time, g_scope_motions.n);
            for (int i = 0; i < g_scope_motions.n; i++)
                std::fprintf(f, "motion=0x%08X whole=%.1f\n", g_scope_motions.addr[i], g_scope_motions.whole[i]);
            std::fclose(f);
        }
        return;
    }
    pose_eval_blend(rdram, ctx, model, count, alpha);
}

// Smooth bone attachments (same switch as above). FUN_08878D38 (a1 = bone) and FUN_08878C8C
// (bone from the model's +0x48) take a player (a0) and an out 4x4 (a2): they read the bone of the
// player's model at the whole frame (int)+0x68 through FUN_08878494 and compose it with the
// player's matrix. Units assembled from parts (a Patapon's body, eye, weapon, helmet) hang the
// parts on the bones of a baked skeleton: model class vtable 0x08A82BB0, whose bone getter
// (+0x50 = FUN_0887A988 -> FUN_08858FC0) reads one bone matrix per whole frame from a table.
// Those reads run outside the player tick/draw, so in the barracks the Patapons kept stepping
// (~10 pose changes/s at 30 and at 60 fps) while the Hero, a GMO model, was smooth. When the
// clock lies between frames, read the next frame too and blend the two matrices. The baked
// getters only read tables (+0x40 = bone count), so the second read has no side effects.
constexpr uint32_t kBoneAt = 0x08878D38u;
constexpr uint32_t kBoneNamed = 0x08878C8Cu;
constexpr uint32_t kBakedModelVtbl = 0x08A82BB0u;
FuncPtr g_bone_at = nullptr;
FuncPtr g_bone_named = nullptr;

template <FuncPtr* Orig>
void bone_smooth(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t player = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t model = player ? rd32(rdram, player + 0x44) : 0;
    const bool baked = model && rd32(rdram, model) == kBakedModelVtbl && rd32(rdram, player + 0x48) == 0 &&
                       rd32(rdram, player + 0x60) != 0;
    if (!baked) {
        (*Orig)(rdram, ctx);
        return;
    }
    const uint32_t clock_bits = rd32(rdram, player + 0x68);
    float t, start, end;
    std::memcpy(&t, &clock_bits, 4);
    const uint32_t start_bits = rd32(rdram, player + 0x6C), end_bits = rd32(rdram, player + 0x70);
    std::memcpy(&start, &start_bits, 4);
    std::memcpy(&end, &end_bits, 4);
    const int32_t frame = psp_f2i(t);
    const float alpha = t - static_cast<float>(frame);
    const int32_t next = psp_anim_next_frame(frame, start, end, rdram[(player + 0x7B) & 0x07FFFFFFu] != 0);
    if (!(alpha > 0.0f && alpha < 1.0f) || next < 0 || next == frame) {
        (*Orig)(rdram, ctx);
        return;
    }
    const uint64_t a0 = ctx->r[4], a1 = ctx->r[5], a2 = ctx->r[6];
    const uint32_t out = static_cast<uint32_t>(a2);
    float m0[16], m1[16];
    (*Orig)(rdram, ctx);
    std::memcpy(m0, rdram + (out & 0x07FFFFFFu), sizeof m0);
    const float next_t = static_cast<float>(next);
    std::memcpy(rdram + ((player + 0x68) & 0x07FFFFFFu), &next_t, 4);
    ctx->r[4] = a0;
    ctx->r[5] = a1;
    ctx->r[6] = a2;
    (*Orig)(rdram, ctx);
    std::memcpy(rdram + ((player + 0x68) & 0x07FFFFFFu), &clock_bits, 4);
    std::memcpy(m1, rdram + (out & 0x07FFFFFFu), sizeof m1);
    static const uint8_t kMatrix[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    psp_blend_pose(m0, m1, kMatrix, 16, alpha);
    std::memcpy(rdram + (out & 0x07FFFFFFu), m0, sizeof m0);
}

// Fast loading (on by default, PSPRECOMP_P3_FAST_LOAD=0 turns it off): the tip screen's loader
// takes one file per two or three frames, so a load whose reads are instant on the host still
// took 2-3 s at 59.94 Hz. FUN_08A18834 draws the "CARGANDO" icon and runs on every frame of
// a tip-screen load and never elsewhere (function census over the graphics tour: the Continue
// load and a mission load, not the boot spinner, menus, the "press any button" page or
// gameplay). While it runs, the vblank waits do not sleep and the FileThread's 10 ms queue
// polls (sceKernelDelayThreadCB(10000)) sleep 1 ms (runtime display fast-forward), so the
// loader runs at host speed: Continue went from ~2.3 s of "CARGANDO" to ~0.5-0.8 s (150-230
// vblanks at ~3 ms each). The budget lapses a few vblanks after the icon stops. The runtime
// blocks it during an ad hoc session (network timeouts count real time).
constexpr uint32_t kLoadingIconDraw = 0x08A18834u;
constexpr int kFastLoadVblanks = 8;
FuncPtr g_loading_icon_draw = nullptr;

void loading_icon_draw(uint8_t* rdram, recomp_context* ctx) {
    g_loading_icon_draw(rdram, ctx);
    psp_display_fast_forward().request(kFastLoadVblanks);
}

void patapon3_register_hooks(uint8_t* /*rdram*/) {
    size_t ach_sources = 0;
    const PspAchievementSource* ach = p3_achievement_sources(&ach_sources);
    psp_achievements_register(ach, ach_sources);  // main() starts the system (psp_achievements_run.h)
    const char* fast_load = std::getenv("PSPRECOMP_P3_FAST_LOAD");
    if (!(fast_load && fast_load[0] == '0')) {
        g_loading_icon_draw = RECOMP_LOOKUP(kLoadingIconDraw);
        psp_dispatch_register(kLoadingIconDraw, loading_icon_draw);
        std::fprintf(stderr, "[P3] fast loading: tip-screen loads skip the vblank waits\n");
    }
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
        g_bone_at = RECOMP_LOOKUP(kBoneAt);
        g_bone_named = RECOMP_LOOKUP(kBoneNamed);
        psp_dispatch_register(kBoneAt, bone_smooth<&g_bone_at>);
        psp_dispatch_register(kBoneNamed, bone_smooth<&g_bone_named>);
        const char* diag = std::getenv("PSPRECOMP_P3_ANIM_SMOOTH_DIAG");
        g_diag_interp = diag && std::strcmp(diag, "interp") == 0;
        if (g_diag_interp) std::fprintf(stderr, "[P3] smooth animation diag: interpolate-flag path\n");
        if (const char* p = std::getenv("PSPRECOMP_P3_ANIM_PROBE")) g_probe_dir = p;
        std::fprintf(stderr, "[P3] smooth animation: poses blended from two whole frames\n");
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
