# 60 fps smooth FEVER worm Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At 60 fps with smooth animation (`PSPRECOMP_PATCHES=60fps`, `PSPRECOMP_P3_ANIM_SMOOTH=1`), the FEVER worm (the red "¡FIEBRE!" worm top-left during FEVER) moves every vblank at the same real-time speed as at 30 fps, like the rest of the army now does.

**Architecture:** The smooth-animation work so far (game module `games/patapon3/runtime/hooks_main.cpp`) blends poses only inside a *player scope*: `motion_scope` wraps the player tick/draw `FUN_08878A18` / `FUN_08878B18`, stores the player's real clock (+0x68) in `g_smooth_time`, and `pose_eval_smooth` (GMO pose eval `FUN_089DBD2C`) plus `baked_draw_smooth` (baked lists, `FUN_0887A814`) blend the whole frame and the next one by the clock's fraction. Recon (below) says the worm's head and tail are a skinned GMO drawn at whole frames, so its pose evaluation most likely runs **outside** any player scope. Task 1 finds who evaluates it and where its real clock lives; Task 2 opens a scope there (same `motion_scope` idea, its own clock word) so the existing blends apply; nothing new is invented for the blend itself.

**Tech Stack:** C++20 runtime + game module (`psprecomp/`), ninja + ctest in `build/rtw`, Python 3 tools in `build/tools/` (outside the repo), debug socket on port 9998.

**Spec:** the user's request (2026-10-21, night), no separate design doc:
> "las animaciones, con lo ultimo que hiciste arreglaste muchas [...] cofres 3D, algunas banderas en los vs, animaciones en la invocacion [...] faltaria animacion smooth para el gusano de fiebre"

> "Haz el plan para el smooth del gusano por favor, recuerda usar el campo de los gigantes y el loop chaka chaka, esta noche es tuya, haz el plan para hacer clear y ejecutar el plan"

Standing rules (memory `p3-smooth-rule`, `p3-60fps`): every animation fixed for 60 fps must be smooth (move every vblank) **and** keep the 30 fps speed; the user's game on port 9999 is never touched (tests on 9998); nothing is pushed without the user's OK (next push branch `push-p3-30`).

## Global Constraints

- Only with smooth animation on: everything new installs inside the existing `if (smooth_on)` block of `patapon3_register_hooks`; 30 fps without `PSPRECOMP_P3_ANIM_SMOOTH=1` must be bit-identical (30 fps `rt_regress` PASS).
- A/B switch for the new hook: `PSPRECOMP_P3_ANIM_SMOOTH_WORM=0` turns it off (same style as `PSPRECOMP_P3_ANIM_SMOOTH_FADE` / `_BAKED`), documented in `docs/ENV_FLAGS.md`.
- Never blend texture offsets/scales (GMO channel 0x9B, GE 0x48..0x4B): they flip atlas cells (lesson of 852f70f, half-cut eyes).
- Gameplay state is not changed: any clock word written for a second evaluation is restored bit-exact right after (like `bone_smooth`).
- Test instances: port 9998 only; kill them by path (`*\build\rtw\*`), never by image name (the user may be playing from `build/e2e_portable3`).
- Disk is ~98 % full: delete the big `[DL]` dumps and copied logs you create once their numbers are in the notes.
- Commits on `patapon3-windows` in `psprecomp/`, author as configured (no Claude co-author trailer, memory `commit-attribution`), message ends with `Verified: ...`.

## What is already known (21 Oct night recon)

- **Repro:** `build/tools/wsession.sh OUTDIR` (Campo de los Gigantes, the rain mission; drum loop `DRUMSEQ` default `CCPO----` = CHAKA CHAKA PATA PON). Env for the current build: `P3DRIVE_EXE=psprecomp_runtime.exe PSPRECOMP_PATCHES=60fps PSPRECOMP_P3_ANIM_SMOOTH=1 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`. READY + route take ~150 s; `drum.log` prints only every 10th command. FEVER comes **and goes** (load stalls break the combo; 8 shots over a minute showed FEVER twice: `build/worm60/fever_cycle.png`). End: `echo Q >> OUTDIR/cmd.txt`, then kill the `wdrum.py` python (filter its command line on `python.exe` + `wdrum.py`, not on text that your own shell line contains).
- **Catching FEVER:** `python -I build/tools/fever_wait.py OUT.log [lists]` polls until the red worm is up, sends `D <lists>`, and writes that part of `build/logs/tour_rt.log` to OUT.log. Analyze with `python -I build/tools/dlgroup.py OUT.log -80 140 35 95` (draws over the worm, grouped by texture/vertex count/vertex type, `x` = changed since the previous list).
- **Finding (`build/worm60/fever_dl.log`, 9 lists in FEVER, current build):**
  - combo worm outside FEVER (strip `ta=0x0018F940 n=6144 vt=0x00019C`, its eyes) already changes every vblank;
  - in FEVER, the parts with texture `0x0011DA00` and **skinned** vertex type `vt=0x01C783` / `0x000183` around the head (x 121..156, y 45..78) and the tail (x -43..-22) show `pos xxx....x`: unchanged for 4 lists in a row while other worm parts (`n=6`/`n=10` `vt=0x01C783`) change in every list. Four vblanks at 60 fps = a 15 keys/s motion drawn at whole frames, the pattern the halo / Hero Gate had before 30e9ecd.
  - 9 lists is a short sample: a hold of 4 could also be a still stretch of the motion. Task 1 confirms on a longer dump and at 30 fps.
- **Smooth scope today:** `motion_scope<&g_motion_tick>` / `<&g_motion_draw>` on `0x08878A18` / `0x08878B18` (clock = player +0x68), `playhead_set_smooth` on `0x089DAF74` (records each motion's whole frame in `g_scope_motions`), `pose_eval_smooth` on `0x089DBD2C` (blends only when `g_smooth_scope && g_scope_motions.n > 0`), `bone_smooth` on `0x08878D38` / `0x08878C8C`, `baked_draw_smooth` on `0x0887A814` (needs `g_smooth_scope`).
- **Tools that worked tonight:** `[DL]` dumps + `dlgroup.py`; `PSPRECOMP_FUNC_ARGS=<hex,...>` (a0..a3 + v0; re-arm with socket `A`; `ra=` is always 0 in recompiled code); `PSPRECOMP_FUNC_WATCH` (its `from` is the last ring entry, **not** the caller); a temporary game-module trace hook gated on `psp_display_elapsed_us() > T` to skip the boot. Keep temporary traces out of commits.

## Review Focus

1. FEVER ending / starting mid-blend: the worm's motion switches (combo worm -> FEVER worm -> gone); the blend must not mix two different motions (expect: blend only when both samples come from the same motion and the next frame exists, else whole frame — the `next < 0` rule of `bone_smooth`).
2. Loop wrap of the worm's idle: the frame after the last one is the loop start, not last+1 (use `psp_anim_next_frame` with the motion's start/end/loop like `bone_smooth`).
3. Speed: the blend must not change how fast the worm moves (cycle period at 60 equal to 30 fps within 5 %).
4. Other users of the same evaluation path (FEVER fire, "¡FIEBRE!" letters, invocation, VS flags the user praised): must not break or start stepping; check the Arena/VS route and the invocation if the hook sits on a shared function.
5. Pause menu over FEVER (the user's first screenshot today): the worm frozen behind the pause box must stay still (clock not advancing => fraction constant => no drift).

---

### Task 1: Find who evaluates the FEVER worm's pose and where its clock lives

**Files:**
- Create (outside the repo): `build/worm60/notes.md` (numbers, addresses, decision)
- Temporary (not committed): a trace hook in `games/patapon3/runtime/hooks_main.cpp`

**Interfaces:**
- Produces for Task 2: (a) the guest function `W` that evaluates/draws the worm outside a player scope (address, a0 meaning); (b) where the worm's real clock is (`obj + off`, float frames) and its start/end/loop words if any; (c) the baseline numbers for Task 2's check.

- [ ] **Step 1: Baseline on a longer sample.** Run `wsession.sh build/worm60/base` with the env above, then `fever_wait.py build/worm60/base30.log 30` (30 lists). Run `dlgroup.py ... -80 140 35 95`. Record in `notes.md`, per head/tail group (`ta=0x0011DA00`, `vt=0x01C783`/`0x000183`, the `pos ....` column): changed transitions / all transitions. Expected: about 1 in 4 (15 keys/s). Also run once at 30 fps (unset `PSPRECOMP_PATCHES`; set `PSPRECOMP_P3_ANIM_SMOOTH=0`): expected about 1 in 2 (15 keys/s at 30). If 60 fps already shows ~every list changing, stop and ask the user what exactly steps (lesson of the Hero Gate: speed vs smoothness).
- [ ] **Step 2: Is the pose evaluated outside the scope?** Add a temporary trace in `pose_eval_smooth` and `baked_draw_smooth`: when `!g_smooth_scope` (after 120 s uptime, first 400 lines), print `model`, `rd32(model)` (vtable), the bone count (+0x18 u16) and the motion words the evaluator reads. Rebuild to a copy exe (`cp rtw/psprecomp_runtime.exe rtw/psprecomp_runtime_wormtrace.exe`, restore the normal one), run the repro with `P3DRIVE_EXE=psprecomp_runtime_wormtrace.exe`, catch FEVER. Expected: a GMO model with `vtbl=0x08A82B38` (GMO class) evaluated with `scope=0` only while FEVER is up. Note its model address.
- [ ] **Step 3: Who calls it, and with which clock.** With `PSPRECOMP_FUNC_ARGS=0x089DBD2C,0x089DAF74` (re-arm with socket `A` during FEVER) list the calls whose a0 is that model (pose eval) and the playhead sets (`0x089DAF74`: a0 = motion, f12 = whole frame). Then find the float that holds the real (fractional) time: candidates are words of the object that owns the model which advance by 0.5 per vblank at 60 fps (`build/tools/word_series.py` / `corr_scan.py` over the owner, or a socket `H <addr>` watch on the motion's playhead word to get the writer, `[BT] hw-watch:` lines, `build/tools/bt.py`). Identify the function `W` that runs the worm's motion per frame (it calls the playhead set and then the pose eval) and the clock offset.
- [ ] **Step 4: Decide and write it down** in `notes.md` (one of):
  - **A.** `W` is a player-like tick/draw with its clock at a fixed offset -> Task 2 wraps `W` in a scope with that offset.
  - **B.** the playhead is set with whole frames from an integer counter (no fractional clock anywhere) -> Task 2 builds the fraction from vblanks since the counter last stepped (per motion; `psp_vblank_index`), like `psp_half_ahead.h` does for positions.
  - **C.** something else (2D sprite list, baked list outside scope, ...) -> stop, write what you found, and keep the Hero Gate lesson: confirm with the user before building a new mechanism.
- [ ] **Step 5: Remove the temporary trace** (`git checkout -- games/patapon3/runtime/hooks_main.cpp`), rebuild, check the deployed hash equals `build/rtw/psprecomp_runtime.exe` again or is restored.

### Task 2: Smooth scope for the worm's motion

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp` (new scope hook next to `motion_scope`; registration in the `smooth_on` block)
- Modify: `docs/ENV_FLAGS.md` (new `PSPRECOMP_P3_ANIM_SMOOTH_WORM` entry after `PSPRECOMP_P3_ANIM_SMOOTH_BAKED`), `ARCHITECTURE.md` (one clause in the smooth-animation sentence), `docs/superpowers/plans/2026-10-15-60fps.md` (one bullet)
- Test: `runtime/tests/test_anim_blend.cpp` only if a new pure helper is added (branch B: the vblank fraction)

**Interfaces:**
- Consumes: Task 1's `W` (address), clock offset (A) or step counter (B); existing `g_smooth_scope`, `g_smooth_time`, `g_scope_motions`, `pose_eval_smooth`, `baked_draw_smooth`, `psp_anim_next_frame`.
- Produces: `void worm_scope(uint8_t* rdram, recomp_context* ctx)` registered on `W`; for branch B a pure `float psp_step_fraction(int64_t vb_now, int64_t vb_step, int64_t period)` in `runtime/include/psp_anim_blend.h` returning the fraction in [0, 1) of the current step (period = vblanks per key step).

- [ ] **Step 1 (branch B only): failing test** in `runtime/tests/test_anim_blend.cpp`:
  - `psp_step_fraction(10, 10, 4) == 0.0f` (the vblank the counter stepped);
  - `psp_step_fraction(12, 10, 4) == 0.5f`;
  - `psp_step_fraction(13, 10, 4) == 0.75f`;
  - `psp_step_fraction(20, 10, 4) == 0.0f` (a stale step: more than one period late shows the whole frame, never extrapolates).
  Run `ninja -C build/rtw test_anim_blend && build/rtw/test_anim_blend.exe`: compile FAIL.
- [ ] **Step 2: Implement.** Branch A: `worm_scope` = `motion_scope` with the worm's clock offset (factor the offset into a template parameter of `motion_scope` rather than copying it: `template <FuncPtr* Orig, uint32_t ClockOff = 0x68>`). Branch B: implement `psp_step_fraction`, then `worm_scope` sets `g_smooth_scope` and `g_smooth_time = whole + psp_step_fraction(...)` around the original call. In both, the blends themselves stay the existing ones. Register under `smooth_on`, gated by `PSPRECOMP_P3_ANIM_SMOOTH_WORM` (`'0'` = off), printing `[P3] smooth animation: FEVER worm motion blended too`.
- [ ] **Step 3: Build and unit tests.** `ninja -C build/rtw` then `cd build/rtw && ctest`: expected `100% tests passed` (96 + any new).
- [ ] **Step 4: Smooth check (GREEN).** Same as Task 1 Step 1 with the new exe: head/tail groups change in at least 26 of 29 transitions (art swaps and the motion switch excepted). With `PSPRECOMP_P3_ANIM_SMOOTH_WORM=0`: back to the baseline (~1 in 4).
- [ ] **Step 5: Speed check.** `build/tools/prop_period.py 9998 12 0 40 160 90 worm60` during FEVER (render scale 1) at 60 with the fix and at 30: periods within 5 %. FEVER may drop during the 12 s: retry until `fever_wait`-style screenshots before and after both show FEVER.
- [ ] **Step 6: Review Focus checks.**
  1. Zoomed burst of 8 frames at FEVER start and end (`weather_drive` `S` every 0.1 s): no worm drawn half in two motions, no detached part.
  2. Pause during FEVER (`P START 1` in `cmd.txt`, then 3 screenshots 0.5 s apart): the worm does not move.
  3. Arena route at 60 fps (`gfx_tour.py --only rt --checkpoints tools/gfx_arena.json`, `TOUR_SAVE=build/save_arena`): talk ends, clock runs, drum HUD up, combo (the 4100add check).
  4. Full 60 fps tour with and without `PSPRECOMP_P3_ANIM_SMOOTH_WORM=0`: same checkpoints reached; inspect march/attack/combo frames by eye (60 fps tours differ in animation phase, the boot can pick German, and the DxD menu sometimes fails to open: rerun before blaming the change).
- [ ] **Step 7: Gates.** 30 fps tour + `python -I build/tools/rt_regress.py build/tour/baked30 build/tour/<new>`: `RT_REGRESS PASS` (the hook is not installed at 30 without smooth).
- [ ] **Step 8: Docs, commit.**

```bash
git add games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md ARCHITECTURE.md docs/superpowers/plans/2026-10-15-60fps.md docs/superpowers/plans/2026-10-22-60fps-fever-worm.md
git commit -m "feat(p3): the FEVER worm moves smoothly at 60 fps"
```
(plus `runtime/include/psp_anim_blend.h` and `runtime/tests/test_anim_blend.cpp` in branch B). Message body: cause (who evaluated the worm outside a scope, clock), fix, the before/after transition counts, speed, `Verified:` line.

### Task 3: Deploy and hand off (no push)

**Files:**
- Create: `build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-10-22.md` (Spanish, for the user)

- [ ] **Step 1: Variants.** `ninja -C build/vanilla/rtw psprecomp_runtime` and `ninja -C build/v115b/rtw psprecomp_runtime`: `Linking` with no error.
- [ ] **Step 2: Deploy with backup** to `build/e2e_portable` and `build/e2e_portable3` (`game/dxd-1.1.4` <- `rtw`, `game/dxd-1.1.5b` <- `v115b/rtw`, `game/vanilla-eu` <- `vanilla/rtw`), backup first to `build/backup_pre_worm_2026-10-22/<portable>/<variant>/`. If a copy fails because the exe is in use, the user is playing: leave the backup, skip that copy and say so in the hand-off. md5 of each deployed exe = its build.
- [ ] **Step 3: Hand-off** `EN_QUE_NOS_QUEDAMOS_2026-10-22.md`: qué pasaba (who drew the worm at whole frames), qué cambió, before/after table, cómo probarlo (launcher at 60 + smooth, Campo de los Gigantes, chaka chaka to FEVER), A/B flag, deployed + backup path, "nada subido; va en push-p3-30 tras tu OK". If Task 1 ended in branch C, the hand-off is the finding and the question instead.
- [ ] **Step 4: Memory.** Update `p3-60fps.md` and its `MEMORY.md` line (local commit hash, not pushed, next push branch `push-p3-30`).
