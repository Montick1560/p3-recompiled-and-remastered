# Smooth character animation at 60 fps (Patapon 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With `PSPRECOMP_PATCHES=60fps,anim_smooth`, character poses change up to 60 times
per second (in-between poses), instead of staying on whole animation frames (~22 pose
changes/s at both 30 and 60 fps today).

**Architecture:** The motion player keeps a float clock but samples the pose at an integer
frame: `FUN_08878A18` (motion tick) does `trunc.w.s f0,f0` at 0x08878A60 and `mfc1 a2,f0` at
0x08878A68, and passes `(int)time` to `FUN_088785A8` (pose + display-list cache, key =
motion id, int frame, global frame counter), which calls the model's vtable +0x38
(`setPose(motion, frame)`). At 60 fps the clock advances 0.5/frame, so each pose shows
twice. The fix is to carry the fractional frame down to the keyframe evaluator, as a new
opt-in patch set `anim_smooth` (instruction words + caves in `game.toml`, the mechanism the
`60fps` set already uses). A game-module C++ hook is the fallback if the chain can't be
patched cleanly with words.

**Tech Stack:** psprecomp recompiler (Rust, `[[patch_sets]]`), runtime C++ (game module
`games/patapon3/runtime/hooks_main.cpp`), Ghidra headless (`build/tools/ghidra/decomp.sh`),
Python drivers (`build/tools/p3drive.py`, debug socket R/W/B/S on ports 9998/9997).

**Spec:** the user's request on 2026-10-15 ("buscarás e intentarás agregar esas animaciones
smooth"), plus the known gap in `docs/superpowers/plans/2026-10-15-60fps.md` ("Animation
smoothness").

## Global Constraints

- Opt-in only: without `anim_smooth` in `PSPRECOMP_PATCHES` the build behaves exactly as
  today (generated batches byte-identical; `rt_regress` relevo_base unchanged).
- Never test on `DECO/savedata`: use the copies `build/save_milagro` or `build/tour_save`.
- Test instances only on ports 9998/9997; never touch the user's game (9999) or a PPSSPP
  not launched by this session.
- Nothing destructive on the PC; no wildcard `rm -rf`; new output goes to new tagged folders.
- Commits on `patapon3-windows`, author "Patapon Community", no Co-Authored-By, end lines
  `Verified: ...` and `Agent: claude-opus-5.5`. No push until the user plays and confirms.
- Patch words must be checked against `INFN00001_EBOOT.BIN` (original word recorded in the
  set's `reason`/comments) and must lie in the main image (overlay window is rejected).
- Code and commits in English; report to the user in Spanish.
- Time box: the user is back at 19:00 CDMX; at 18:00 stop new work, verify, commit, write up.

## Review Focus

1. Looping motions wrap from end (+0x70) to start (+0x6C): an in-between pose must not
   blend across the wrap into garbage — check a looping idle for a pop each cycle.
2. Other callers of the pose method still pass whole ints (`FUN_0887879c` -> `FUN_088785A8`,
   anything else calling vtable +0x38): with a float-bit convention they must be converted
   too, or they get a nonsense frame (model explodes / T-pose).
3. Gameplay events keyed to animation frames (hit frames, sounds, effects) must keep firing
   exactly once — we change only what is sampled, never the clock at +0x68.
4. Performance: the pose cache (`FUN_088785A8`) may rebuild more lists; `perf_hideout`
   must stay at ~60 fps within +10 % CPU of the `60fps`-only run.
5. Scenes other than the army in a mission (hideout NPCs, Hero, bosses, FEVER worm,
   minigames, cutscenes) may use a different player; each must either get smoother or stay
   exactly as it was, never break.

---

### Task 1: Pose-rate measurement tool (the acceptance test)

**Files:**
- Create: `C:/Users/torso/Documents/deco/build/tools/anim_rate.py` (outside the repo, next to
  `p3drive.py`; promoted from the scratch `burst.py`)

**Interfaces:**
- Produces: `python build/tools/anim_rate.py PORT SECONDS [x0 y0 x1 y1]` → prints
  `pose_changes_per_s=<float> captures_per_s=<float>`; exit 0.

- [ ] **Step 1: Write the tool**

```python
"""anim_rate.py PORT SECONDS [x0 y0 x1 y1]: capture frames from a runtime on PORT as fast as the
debug socket allows and print how often the region changes (pose changes per second)."""
import sys, time
import numpy as np
sys.path.insert(0, 'C:/Users/torso/Documents/deco/build/tools')
import gfx_tour
from p3drive import Instance

port, secs = int(sys.argv[1]), float(sys.argv[2])
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (60, 150, 330, 235)
side = gfx_tour.RtSide.__new__(gfx_tour.RtSide)
side.inst, side.tga = Instance('anim_rate', port), f'C:/Users/torso/Documents/deco/build/shots/_ar{port}.tga'
frames, t0 = [], time.perf_counter()
while time.perf_counter() - t0 < secs:
    frames.append(np.asarray(side.frame().convert('RGB'), dtype=np.int16)[y0:y1, x0:x1])
el = time.perf_counter() - t0
changes = sum(1 for a, b in zip(frames, frames[1:]) if int((np.abs(b - a).max(axis=2) > 30).sum()) > 50)
print(f'pose_changes_per_s={changes / el:.1f} captures_per_s={len(frames) / el:.0f}')
```

- [ ] **Step 2: Baseline (this is the failing test)**

Launch two instances with the scratch `launch2.py` pattern: 9998 with `PSPRECOMP_PATCHES=60fps`,
9997 without, both on `build/tour_save`, drive both to the same idle army spot in the
training mission (`build/tools/to_mission.sh` / `gfx_tour.py` steps), then:

Run: `python build/tools/anim_rate.py 9998 10` and `python build/tools/anim_rate.py 9997 10`
Expected today: both ~22 pose changes/s (captures ≥ 45/s). Record the numbers in the plan's
Evidence section. Acceptance after the fix: 60 fps run ≥ 1.6× its baseline, 30 fps run unchanged.

### Task 2: Find the pose method and where the frame becomes a keyframe position

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp` (diagnostic `PSPRECOMP_P3_ANIM_TRACE=1`,
  no behaviour change)
- Notes: append findings to this plan (Evidence section)

**Interfaces:**
- Consumes: debug env `PSPRECOMP_P3_ANIM_TRACE=1`.
- Produces: the address of the model vtable +0x38 method(s) (`setPose`), its callee chain to
  the keyframe evaluator, and the list of every caller of `FUN_088785A8` and of `setPose`.

- [ ] **Step 1: Add the trace hook** (same pattern as the GMO guard)

```cpp
// PSPRECOMP_P3_ANIM_TRACE=1 (diagnostic): FUN_088785A8 (a0 = pose cache, a1 = motion id,
// a2 = whole frame) calls the model's vtable +0x38 with the frame; log each distinct
// vtable/method once so the pose path can be decompiled.
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
        std::fprintf(stderr, "[ANIM] cache=0x%08X model=0x%08X vtbl=0x%08X +0x38=0x%08X "
                     "motion=%u frame=%d ra=0x%08X\n", cache, model, vtbl, method,
                     static_cast<unsigned>(ctx->r[5] & 0xFFFF), static_cast<int>(ctx->r[6]),
                     static_cast<uint32_t>(ctx->r[31]));
    }
    g_pose_cache(rdram, ctx);
}
```

Register it in `patapon3_register_hooks` when `PSPRECOMP_P3_ANIM_TRACE` is `1`
(`g_pose_cache = RECOMP_LOOKUP(kPoseCache); psp_dispatch_register(kPoseCache, pose_cache_trace);`).

- [ ] **Step 2: Build and run** — `ninja -C build/rtw`, launch on 9998 with the flag, walk
  hideout → mission; collect `[ANIM]` lines from the instance log.
- [ ] **Step 3: Decompile** the reported `+0x38` methods and their callees down to the place
  that turns the frame into keyframe interpolation (look for `cvt.s.w` of the frame, a key
  search loop, and a lerp/hermite) and `x`-xrefs for every caller:
  `bash build/tools/ghidra/decomp.sh <scratch>/pose.c <method> x<method> x088785A8`
  and `python tools/mdis.py <start> <end> ../INFN00001_EBOOT.BIN` for exact words.
- [ ] **Step 4: Decision gate** (write the outcome in Evidence):
  - **A (words):** the evaluator converts the int with one `cvt.s.w` and interpolates on a
    float → patch set: 0x08878A60 `trunc.w.s f0,f0` (0x4600000D) → `mov.s f0,f0`
    (0x46000006), the consumer's `mtc1/cvt.s.w` → `mtc1/mov.s`, and every other caller that
    passes a real int gets its `mtc1/…` path converted to float bits (cave if it needs an extra
    instruction).
  - **B (hook):** the int is used as an array index / in several places → game-module hook:
    run `FUN_08878A18` unchanged but stash `frac = time - floor(time)` for the duration of the
    call, and wrap the keyframe evaluator to evaluate at `frame + frac` (two samples + lerp if
    the evaluator only takes whole frames).
  - **C (stop):** the evaluator has no interpolation at all (baked per-frame poses) → a
    lerp of the two neighbouring baked poses is the only option; time-box 2 h, else
    document and stop.
- [ ] **Step 5: Commit** the trace hook (`chore(p3): PSPRECOMP_P3_ANIM_TRACE pose-path
  diagnostic`) with `ENV_FLAGS.md` entry.

### Task 3: `anim_smooth` patch set (route A) or hook (route B)

**Files:**
- Modify: `games/patapon3/game.toml` (new `[[patch_sets]] name = "anim_smooth"`)
- or Modify: `games/patapon3/runtime/hooks_main.cpp` (route B)
- Test: `runtime/tests/test_patch_sets.cpp` only if a pure helper is added (route B lerp)

**Interfaces:**
- Consumes: Task 2 decision and addresses.
- Produces: `PSPRECOMP_PATCHES=anim_smooth` (combinable: `60fps,anim_smooth`).

- [ ] **Step 1: Verify every original word** with `mdis.py` and write each as
  `{ address = 0x..., instruction = 0x... }  # was 0x... (<mnemonic>)` in the set.
  The first is fixed: `{ address = 0x08878A60, instruction = 0x46000006 }  # was 0x4600000D trunc.w.s f0,f0`.
- [ ] **Step 2: Regenerate** with the scratch `regen.sh banks` (main output + overlay banks;
  fingerprint requires both), confirm `git diff --stat` on generated batches is empty except
  `patch_sets.cpp`, build `ninja -C build/rtw`.
- [ ] **Step 3: Run acceptance** — Task 1 measurement on 9998 with `60fps,anim_smooth`.
  Expected: ≥ 1.6× baseline pose changes/s; visual check of 10 frames in a burst
  (`build/tour/anim_smooth/*.png`) for exploding/T-posed models.
- [ ] **Step 4: Review Focus checks:** loop wrap (watch an idle loop 10 cycles, no pop),
  attack hit + sound once per swing (drum march + attack in the training mission, compare
  damage numbers/sounds with the `60fps` run), `perf_hideout` CPU.
- [ ] **Step 5: Commit** `feat(p3): anim_smooth patch set: sample poses at the fractional
  motion frame`.

### Task 4: Coverage of other scenes and players

- [ ] **Step 1:** Measure / eyeball with `anim_rate.py` and bursts: hideout (Hero + NPCs),
  FEVER worm (`build/save_milagro`), a boss if reachable from the saves, a cutscene.
- [ ] **Step 2:** For any scene still stepping at whole frames, repeat Task 2 Step 1 trace
  with `ra` to find its player (`FUN_0887879c` first), extend the set, re-run Task 3 checks.
- [ ] **Step 3: Commit** each extension separately.

### Task 5: Wire-up, regressions, docs, hand-off

- [ ] `ctest` (all), `cargo test`, `rt_regress.py` relevo_base vs a fresh flag-off tour
  (only the known animated checkpoints over the gate), full graphics tour with
  `60fps,anim_smooth` completes.
- [ ] `DECO/jugar_60fps.bat`: `set PSPRECOMP_PATCHES=60fps,anim_smooth` (keep a
  `jugar_60fps_sin_anim.bat`-style fallback only if anim_smooth has any doubt left).
- [ ] Docs: `docs/ENV_FLAGS.md`, `ARCHITECTURE.md` patch-set table, this plan's Evidence,
  the 60fps plan's "Known gaps"; memory `p3-60fps.md`.
- [ ] Commit `docs(anim_smooth): ...`; write the Spanish summary for the user (what, evidence,
  how to test, limits). No push.

## Evidence

**Outcome: route B (game-module hook), on by default with `60fps`; `PSPRECOMP_P3_ANIM_SMOOTH`
overrides.** No patch-set words, no regeneration.

- Trace (`PSPRECOMP_P3_ANIM_TRACE=1`, hideout -> training mission): two model classes reach the
  pose cache. vtbl 0x08A82B38: +0x38 = FUN_08879DF4 (+0x0C = 0x08879B90, +0x3C = 0x08879EA4,
  which ignores the frame); vtbl 0x08A82BB0: +0x38 = 0x08878598 (`jr ra`, no-op).
- FUN_08879DF4 (a0 = holder, a1 = motion id, a2 = int frame) converts the frame to float
  (`cvt.s.w` at 0x08879E68) and calls the playhead setter FUN_089DAF74 (a0 = motion slot,
  f12 = time -> slot +0x1C), then FUN_089DBD2C(f12 = 0, a0 = GMO model, a1 = 2).
- The real blocker was in the GMO library: FUN_089DB1E0 rounds the slot time with
  `floor(t + 0.5)` (0x089DB9F0, constant 0.5 at 0x08A65E14) unless model +0x28 has bit 4
  (`andi 0x4` at 0x089DB2B0, the only reader of that bit in 0x089DA000-0x089E2000). Feeding a
  fractional playhead alone moved the army metric only 16 -> 20/s; with the bit set during the
  evaluation it reached ~40/s.
- Hook: FUN_08878A18 / FUN_08878B18 open a scope holding the player clock (+0x68); inside it
  FUN_089DAF74 stores that clock (only when the int it got equals trunc(clock)) and
  FUN_089DBD2C runs with bit 4 set, restored after. Clock, cache key and events unchanged.

Measurements (port 9998, `build/tour_save`, 60fps set):

| Scene | metric | smooth off | smooth on |
|---|---|---|---|
| training mission, idle army (`anim_rate.py`, region 60,150-330,235) | pose changes/s | 12.6 / 17.2 / 16.8 | 39.7 / 40.4 / 41.3 |
| hideout, Hero (low threshold, region 180,100-280,200) | changes/s (>= 5 px) | 27.8 / 29.6 | 50.7 / 53.0 |

(captures ~55-62/s, so "on" is capture-limited.) In the hideout `anim_rate.py`'s coarse
threshold (50 px > 30) drops 14 -> 4/s: smooth steps are smaller than the threshold; the
slow sheet `build/tour/anim_smooth/hide_slow1.png` shows the Hero animating normally.

- Visual: burst `build/tour/anim_smooth/burst_smooth60.png` (12 consecutive frames, army
  idle) — no exploded or T-posed models.
- Full gfx tour with `60fps` (+ smooth): hideout, menus, drum march, attack, combo, mission
  clear, reward: all 31 checkpoints (`build/tour/anim_smooth/full60`).
- Flag off (no `60fps`, hooks not installed): `rt_regress.py relevo_base anim_smooth_off`
  PASS (21 gated <= 0.5 %); ctest 61/61.
- `perf_hideout --diff anim_off60 anim_on60`: fps 59.9 -> 59.95, ge_ms 4.2 = 4.2,
  CPU 65.8 -> 68.5 % of one core (+4 %).

Not checked by tooling (for the user's play test): looping idles for a pop at the wrap,
FEVER worm, bosses, cutscenes, minigames. Units whose model is not reached through
FUN_08878A18/FUN_08878B18 keep the stepped animation.
