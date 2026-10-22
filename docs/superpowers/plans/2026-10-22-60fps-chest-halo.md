# 60 fps smooth reward halo (chest room) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At 60 fps with smooth animation (`PSPRECOMP_PATCHES=60fps`, smooth on by default with the set), the glowing halo behind the reward when a chest is opened in the chest room changes every vblank at the same real-time speed as at 30 fps.

**Architecture:** Same family as the FEVER worm (cd07d87). The halo is a **baked model** (per-frame recorded GE list: texture scale/offset, alpha `0x58`, world matrix `0x3A/0x3B`, then CALLs to its parts). `baked_draw_smooth` (`FUN_0887A814`) already blends lists f/f+1 into the ring at `0x0F000000` and substitutes the CALL target, but only inside a smooth scope (`g_smooth_scope`, opened by `smooth_scope_call` around a motion player tick/draw). Recon shows the halo's CALL target is the game's own list, not the ring: its draw runs outside any scope. Task 1 finds the function that draws it and where its clock lives; Task 2 opens the scope there with the existing helpers; the blend itself is not new.

**Tech Stack:** C++20 runtime + game module (`psprecomp/`), ninja + ctest in `build/rtw`, Python 3 tools in `build/tools/` (outside the repo), debug socket on port 9998.

**Spec:** the user's request (2026-10-22, morning), no separate design doc:
> "crea la tarea para ahora hacer smooth el halo que sale cuando abres tus recompensas en la sala de cofres, para esto deberias ir a los campos de pruebas en el mapa (la primera sección de todo) y seleccionar la primera mision y solo hacer chaka chaka, ahi siempre te darán un cofre de madera, abres el cofre y saldrá el halo"

Standing rules (memories `p3-smooth-rule`, `p3-60fps`): every animation fixed for 60 fps must be smooth (move every vblank) **and** keep the 30 fps speed; the user's game on port 9999 is never touched (tests on 9998); nothing is pushed without the user's OK (next push branch `push-p3-31`).

## Global Constraints

- Only with smooth animation on: everything new installs inside the existing `if (smooth_on)` block of `patapon3_register_hooks`; 30 fps without `PSPRECOMP_P3_ANIM_SMOOTH=1` must be bit-identical (30 fps `rt_regress` PASS).
- A/B switch for the new hook: `PSPRECOMP_P3_ANIM_SMOOTH_REWARD=0` turns it off (same style as `PSPRECOMP_P3_ANIM_SMOOTH_WORM`), documented in `docs/ENV_FLAGS.md` right after `_WORM`.
- Never blend texture offsets/scales (GE `0x48..0x4B`, GMO channel `0x9B`): they flip atlas cells (852f70f). `list_call_smooth` already keeps them; do not change that.
- Gameplay state is not changed: the hook only reads clocks; any word written for a second evaluation is restored bit-exact right after.
- If the drawer lives in an overlay (address `>= 0x08ABB180`), hook it like `overlay_motion_scope`: resolve the loaded bank's function per call with `psp_on_lookup_miss(addr)` and open the scope only when the first code words at the address match the function you decompiled (DxD 1.1.4, 1.1.5b and vanilla may load different banks there; check all three, overlay **file names are rotated in DxD**, go by overlay id at `0x08ABB180`).
- Test instances: port 9998 only; kill them by path (`*\build\rtw\*`), never by image name (the user may be playing from `build/e2e_portable3`).
- Disk is nearly full: delete big `[DL]` dumps, bursts and copied logs once their numbers are in the notes.
- Commits on `patapon3-windows` in `psprecomp/`, author as configured, no Claude co-author trailer (memory `commit-attribution`), message ends with `Verified: ...`.

## What is already known (22 Oct recon, build cd07d87, 60 fps + smooth)

- **Repro (scripted, ~8 min):** the graphics tour already plays the first training mission and stops in the chest room:
  ```
  cd build/tools
  TOUR_RT_PORT=9998 PSPRECOMP_PATCHES=60fps PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 \
    python gfx_tour.py --only rt --to reward_chest --keep --out ../tour/<name>
  ```
  It ends with the wooden chest on screen (label "Nvl"); then open it from Python:
  `Instance('x', 9998).press('X', wait=0)` (`p3drive.py`). The halo stays up (pulsing) until the next X. The tour's mission uses PATAPON/PONPON instead of the user's CHAKA CHAKA; it is the same mission and also gives the wooden chest. If the tour fails (60 fps tours sometimes boot in German or miss the DxD menu), rerun; manual fallback = the user's route (map, first section "Campos de prueba", first mission, CHAKA CHAKA until the timer ends).
- **The halo steps at 60 fps:** full-res captures of the halo core (480x272 box x 200..280, y 60..140; `rawgrab.grab`) at ~63 captures/s for 6 s: the mean brightness changes **15.0 times/s**, intervals between changes p10/p50/p90 = 48/68/79 ms, i.e. each value holds 4 vblanks (`228.3 228.3 224.8 224.8 220.3 220.3 220.3 215.2 ...`). `step_hist.py 9998 8 140 10 340 180` gives `rough=2.00 held=0.77`. Pulse period roughly 0.45 s (not yet compared with 30 fps).
- **What it is (`[DL]` dump `D 2`, 493 draws):** the main list CALLs a per-frame recorded list at `0x097DE8C0`: `0x48 0x3F8000, 0x49 0x3F8000, 0x4A 0, 0x4B 0, 0x58 0x00002D, 0x3A 0, 0x3B` x12 (uniform scale `0x3F7AB5` = 0.979, the pulse), then CALLs to the part lists (texture `0x001EEE00`, `n=6`, `vt=0x000183`, `thr=0`, material alpha 0x2D/0x89: three concentric discs centered at x 240, y ~104, each split in two halves). Same structure for the light rays (`ta=0x001F0000`, 8 wedges) and the beam (`ta=0x001F9500`). That is the baked layout (`FUN_0887A814` -> `FUN_08858CC8` -> `FUN_0883DC8C` BASE+CALL); with a blend the CALL would point into the ring `0x0F000000`, so **`baked_draw_smooth` ran with `g_smooth_scope == false`** (or the next frame was missing).
- **Smooth machinery today (`games/patapon3/runtime/hooks_main.cpp`):** `smooth_scope_call(rdram, ctx, orig, overlay_player)` (clock = a0 +0x68), `motion_scope<&g_motion_tick/_draw>` on `0x08878A18`/`0x08878B18`, `overlay_motion_scope` on `0x08B72338` (mission overlay id 3, code-word check `kOverlayMotionDrawCode`), `scope_clock` (per-model clock for an overlay player), `baked_draw_smooth` on `0x0887A814` (blends when `g_smooth_scope`, `frame == (int)t`, fraction in (0,1)), `list_call_smooth` on `0x0883DC8C`, `baked_bone_smooth` on `0x0887A988`.
- **Lesson from the worm:** `[DL]` dumps slow the game to a few lists/s, so list-to-list comparisons say nothing about time-based animation. Measure with screen captures (above) or an in-hook trace (frame history per model).

## Review Focus

1. Chest opening sequence: the lid/chest motion, the item popping up and the halo appearing start mid-blend; a blend must only mix frames f and f+1 of the same model and motion (the existing `baked_draw_smooth` rule), never two different motions when the sequence switches.
2. Speed: the pulse period at 60 fps equals 30 fps within 5 %.
3. Other chests and rewards drawn by the same function: silver/gold chests, rare-item rewards, the post-mission results; nothing breaks, no part detaches (the rays and beam must move with the discs).
4. Other screens that might load the same overlay bank or reach the same drawer (hideout, missions, Arena): unchanged; check the Arena route (the 4100add check) and a full 60 fps tour.
5. 30 fps: default 30 fps bit-identical (`rt_regress` PASS); 30 fps + smooth (the user's `jugar.bat`) still fine in the chest room.

---

### Task 1: Find who draws the halo outside the scope, and its clock

**Files:**
- Create (outside the repo): `build/chest60/notes.md` (numbers, addresses, decision)
- Temporary (not committed): a trace in `games/patapon3/runtime/hooks_main.cpp`

**Interfaces:**
- Produces for Task 2: (a) the guest function `R` that draws the halo (address, overlay id/bank if any, a0 meaning, first 7 code words); (b) the clock word(s) it draws at (`a0 + off`, float frames) and the start/end/loop words if any; (c) baseline numbers (changes/s, hold intervals, pulse period at 30 and 60).

- [ ] **Step 1: Baseline at 60 and 30.** Repro above, open the chest, then the 6 s capture of the core box (changes/s, interval percentiles, the value series) and `step_hist.py 9998 8 140 10 340 180 halo`. Same at 30 fps (unset `PSPRECOMP_PATCHES`, `PSPRECOMP_P3_ANIM_SMOOTH=0`): expected ~15 changes/s (2 vblanks per key). Pulse period at both from the series' autocorrelation. Write all numbers in `notes.md`. If 30 fps shows a different period than 60, the halo also has a speed bug: write it down and fix speed first (pacing, like the weather) before smoothness.
- [ ] **Step 2: Trace the baked draw outside the scope.** Temporary trace in `baked_draw_smooth` (copy the WORMTRACE pattern from `build/worm60/apply_trace.py`: per-model entry with draw count, in/out of scope, 16-frame history; report every 4 s after 100 s uptime). Build a copy exe (`cp build/rtw/psprecomp_runtime.exe build/rtw/psprecomp_runtime_halotrace.exe` after building the trace, then rebuild the normal one), run the repro with `P3DRIVE_EXE=psprecomp_runtime_halotrace.exe` (the tour launches through `p3drive.py`, which reads it); never overwrite the deployed exes. Expected: models drawn only while the halo is up, `out` = all draws, frame history holding each frame 4 vblanks.
- [ ] **Step 3: Who calls it.** `PSPRECOMP_FUNC_ARGS=0x0887A814` (re-arm with socket `A` after the chest opens) gives a0 = model, a1 = motion, a2 = frame for the halo models. Find the caller: `PSPRECOMP_OVL_ARGS` for overlay functions, the overlay id at `0x08ABB180` in the chest room, then decompile up the chain (Ghidra on the loaded bank, `build/ovl/*.elf`; the worm's chain was gauge -> layout items -> item player draw). Identify `R` (the function that runs per frame per halo object and passes `(int)clock` to the baked draw) and the clock offset(s). Check whether `R` is an exact or extended copy of `0x08878B18` / `0x08B72338` (same pattern as the worm) or something else.
- [ ] **Step 4: Decide and write it down** in `notes.md` (one of):
  - **A.** `R` is a motion-player-like draw with its clock at a0 +0x68 -> Task 2 registers a scope hook on `R` with `smooth_scope_call` (and the code-word check if `R` is in an overlay).
  - **B.** `R` draws several models at different clocks (like the worm's +0xA0/+0xA8) -> Task 2 also extends `scope_clock` with `R`'s offsets (generalize it by player kind, not by adding another hard-coded `if`).
  - **C.** no fractional clock (integer frame counter only) -> stop and write the finding; ask before building a vblank-fraction mechanism.
- [ ] **Step 5: Remove the temporary trace** (`git checkout -- games/patapon3/runtime/hooks_main.cpp`), rebuild, delete the trace exe and big logs.

### Task 2: Smooth scope for the halo's drawer

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp` (new hook next to `overlay_motion_scope`; registration inside `smooth_on`)
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_P3_ANIM_SMOOTH_REWARD`), `ARCHITECTURE.md` (one clause in the smooth-animation sentence), `docs/superpowers/plans/2026-10-15-60fps.md` (one bullet), this plan (Evidence section)
- Test: `runtime/tests/` only if a pure helper is added (e.g. a shared code-word matcher used by both overlay hooks)

**Interfaces:**
- Consumes: Task 1's `R`, its bank/code words and clock offsets; existing `smooth_scope_call`, `scope_clock`, `baked_draw_smooth`, `psp_on_lookup_miss`.
- Produces: `void reward_scope(uint8_t* rdram, recomp_context* ctx)` registered on `R`, printing `[P3] smooth animation: reward halo blended too` when installed.

- [ ] **Step 1: Failing check (RED).** With the current exe the Task 1 baseline is the failing measurement (15 changes/s at 60). If a pure helper is factored out (e.g. `bool code_matches(const uint8_t* rdram, uint32_t addr, const uint32_t* words, size_t n)` shared with `overlay_motion_scope`), write its unit test first: match, one word off, and an address whose bank is not loaded (all zero) -> false; run `ninja -C build/rtw && cd build/rtw && ctest -R <test>`: compile FAIL.
- [ ] **Step 2: Implement** `reward_scope` per Task 1's branch (A: `smooth_scope_call(rdram, ctx, orig, player)`; B: also per-model clocks). Register under `smooth_on`, gated by `PSPRECOMP_P3_ANIM_SMOOTH_REWARD` (`'0'` = off).
- [ ] **Step 3: Build and unit tests.** `ninja -C build/rtw` then `cd build/rtw && ctest`: `100% tests passed` (96 + any new).
- [ ] **Step 4: Smooth check (GREEN).** Repro, open the chest, 6 s core capture: at least 50 changes/s with captures at >= 60/s, hold intervals p50 <= 20 ms; `step_hist` rough well below 2.0 (halo/gate went 1.55 -> 0.48). With `PSPRECOMP_P3_ANIM_SMOOTH_REWARD=0`: back to ~15 changes/s.
- [ ] **Step 5: Speed check.** Pulse period at 60 with the fix vs 30 fps (Task 1 Step 1): within 5 %.
- [ ] **Step 6: Review Focus checks.**
  1. Burst of 30 full-res frames right after pressing X (chest opening -> item -> halo), montage at 2x: no part drawn half in two motions, rays/beam/discs together.
  2. Note in the hand-off which other chests were seen (the tour save only gives the wooden one; say so if the others were not checked).
  3. Arena route at 60 fps (`gfx_tour.py --only rt --checkpoints tools/gfx_arena.json`, `TOUR_SAVE=build/save_arena`): talk ends, clock runs, drum HUD; full 60 fps tour with and without `_REWARD=0`: same checkpoints reached (32/32).
  4. 30 fps + `PSPRECOMP_P3_ANIM_SMOOTH=1`: chest room capture fine.
- [ ] **Step 7: Gates.** 30 fps tour + `python -I build/tools/rt_regress.py build/tour/baked30 build/tour/<new>`: `RT_REGRESS PASS`.
- [ ] **Step 8: Docs, commit.**

```bash
git add games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md ARCHITECTURE.md docs/superpowers/plans/2026-10-15-60fps.md docs/superpowers/plans/2026-10-22-60fps-chest-halo.md
git commit -m "feat(p3): the chest-room reward halo moves smoothly at 60 fps"
```
Message body: who drew the halo outside the scope, its clock, the fix, before/after changes/s, speed, `Verified:` line.

### Task 3: Deploy and hand off (no push)

**Files:**
- Create: `build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-10-22b.md` (Spanish, for the user)

- [ ] **Step 1: Variants.** `ninja -C build/vanilla/rtw psprecomp_runtime` and `ninja -C build/v115b/rtw psprecomp_runtime`: link with no error.
- [ ] **Step 2: Deploy with backup** to `build/e2e_portable` and `build/e2e_portable3` (`game/dxd-1.1.4` <- `rtw`, `game/dxd-1.1.5b` <- `v115b/rtw`, `game/vanilla-eu` <- `vanilla/rtw`), backup first to `build/backup_pre_reward_2026-10-22/<portable>/<variant>/`. If an exe is in use the user is playing: skip that copy and say so. `cmp` each deployed exe with its build.
- [ ] **Step 3: Hand-off** in Spanish: qué pasaba, qué cambió, tabla antes/después, cómo probarlo (Campos de prueba, primera misión, CHAKA CHAKA, abrir el cofre), flag A/B, desplegado + backup, "nada subido; va en push-p3-31 tras tu OK".
- [ ] **Step 4: Memory.** Update `p3-60fps.md` and its `MEMORY.md` line (local commit hash, not pushed, next push branch `push-p3-31`).

---

## Evidence (22 Oct 2026, executed)

- **Task 1 (branch B).** The `[HT]` baked-draw trace was inconclusive (its 128-model table filled during the tour,
  before the chest room). Found instead: the main list CALLs consecutive frame lists of one baked motion (0x140
  apart), so socket `H` on that CALL word + the host backtrace gave the chain `FUN_0883DC8C <- FUN_0887A814 <-
  ovTitle FUN_08B4D1A8 <- ovTitle FUN_08B4CB28 <- FUN_08B4A3F0 <- FUN_08B47320 ...` (mission overlay id 3, bank
  ovTitle; same first code words in DxD 1.1.4, 1.1.5b and vanilla). `FUN_08B4CB28(object, matrix)` draws two
  motion-player blocks of the reward object, +0x10 (item: model +0x70, clock +0x78) and +0x90 (halo: model +0xF0,
  clock +0xF8, motion 2, 31 frame lists, end 30, loop), at `(int)clock` through `FUN_08B4D1A8` -> model vtable
  +0x3C. `object+0x68` is not a clock. Halo clock: +0.25/vblank at 60 fps, +0.5/frame at 30, loop 2.0 s both,
  pulse 0.666 s (60) / 0.668 s (30): speed already right. Notes: build/chest60/notes.md.
- **Task 2.** `reward_scope` on the overlay address (resolve per call + code-word check, factored into
  `runtime/include/psp_code_match.h` with `code_match_tests`); the scope's own clock is 0.0 (a whole frame, so nothing
  else in it blends) and `scope_clock` now reads a per-kind table (`ScopeKind`: model word -> clock word) shared
  with the worm; `baked_bone_smooth` stays worm-only. A loop-wrap draft was dropped: frame + 1 always exists.
- Halo core box, 6 s full-res captures at ~58/s, 60 fps + smooth:

  | run | changes/s | held pairs >= 17 ms apart | step_hist rough / held | pulse |
  |---|---|---|---|---|
  | before (6e3f021) | 15.0 | 0.57 | 1.94 / 0.77 | 0.666 s |
  | fix | **46.8** | **0.01** | **1.27 / 0.30** | **0.668 s** |
  | fix + `_REWARD=0` | 15.0 | — | 2.00 / 0.77 | 0.668 s |
  | 30 fps, smooth off | 14.8 | 0.56 | 1.98 / 0.78 | 0.668 s |
  | 30 fps + smooth, fix | 29.8 | 0.00 (>= 34 ms) | 1.71 / 0.54 | 0.667 s |

  Ruling on Step 4's ">= 50 changes/s, p50 <= 20 ms": not reachable at this capture rate (later captures come
  ~11 ms apart, two grabs per vblank, which caps changes/s at ~47 of 58); among captures at least one vblank apart
  only 1 % repeat (57 % before), i.e. the halo changes every vblank. Speed within 0.3 % of 30 fps.
- Review Focus: 1. 30-frame burst from X (0.79 s): lid opens, item rises, halo grows, nothing detached; discs, rays
  and beam move together. 2. Only the wooden chest was seen (twice: "Alas de Hielo", "Arco de Fuego"); silver/gold
  chests and rare items not checked. 3./4. Full 60 fps tour with and without `_REWARD=0`: 32/32 both; Arena route at
  60 fps: talk ends, clock runs 4:26 -> 4:21, drum HUD. 5. 30 fps + smooth: halo changes every game frame.
- Gates: ctest 97/97; 30 fps tour `build/tour/reward30` vs `baked30`: RT_REGRESS PASS (21 gated).
