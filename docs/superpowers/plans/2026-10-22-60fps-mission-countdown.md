# 60 fps mission countdown at its 30 fps speed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At 60 fps (`PSPRECOMP_PATCHES=60fps`) the mission-start countdown ("¡TON!" "¡CHIN!" "¡KAN!" "¡Ya!" bubbles after the dialogs) and every other frame-counted wait of the game's scripts last the same real time as at 30 fps.

**Architecture:** The game's scripts wait with `cmd_waitFrame` (`0x08838434`), which counts one frame per script run; outside the hideout the script objects run every vblank at 60 fps, so every frame wait ends in half the time. The fix scales a frame wait when it starts: a wait of N frames written for 30 fps runs for 2N script runs at 60. Scripts are not skipped or paced (pacing mission scripts broke the Arena in 4100add); only the wait counter changes. The hideout keeps its existing per-object 30 Hz pacing and is left unscaled.

**Tech Stack:** C++20 runtime + game module (`psprecomp/`), ninja + ctest in `build/rtw`, Python 3 tools in `build/tools/` (outside the repo), debug socket on port 9998.

**Spec:** the user's request (2026-10-22), no separate design doc:
> "en esa misma misión [Campos de prueba, primera misión] pasa algo que no tiene que ver con la suavidad, si no con que va muy rápido por los 60 FPS: Cuando terminan los dos primeros dialogos que se pasan con X, pasa un contador que dice TON CHIN KAN ¡VAMOS! [...] a 30 fps respeta el tiempo en segundos, con los 60 fps no, va muy rápido, crea el plan para poder hacer que vaya a su tiempo"

Standing rules (memories `p3-60fps`, `p3-smooth-rule`): 60 fps fixes keep the 30 fps speed; the user's game on port 9999 is never touched (tests on 9998); nothing is pushed without the user's OK (next push branch `push-p3-31`, which also carries 5cbf2b8 if the user approves the halo).

## Global Constraints

- Only with the `60fps` patch set: install inside the existing `if (psp_patch_set_active("60fps"))` block of `patapon3_register_hooks`, next to `PSPRECOMP_P3_HIDEOUT_30HZ`. 30 fps (with or without `PSPRECOMP_P3_ANIM_SMOOTH=1`) must be bit-identical: 30 fps `rt_regress` PASS.
- A/B switch: `PSPRECOMP_P3_SCRIPT_WAIT_30HZ=0` turns it off (same style as `_HIDEOUT_30HZ`), documented in `docs/ENV_FLAGS.md` right after `PSPRECOMP_P3_HIDEOUT_30HZ`. Install line: `[P3] 60fps: script frame waits last their 30 fps time`.
- Never skip a script run and never return a value the callers do not expect (lesson of 8fd2540 / 4100add): the hook calls the original every time and only rewrites the wait counter of the wait that just started.
- Not in the hideout while the hideout pacing is installed (`in_hideout(rdram)` and `g_script_update != nullptr`): those scripts already run at 30 Hz, so scaling there would make their waits 4x. With `PSPRECOMP_P3_HIDEOUT_30HZ=0` the hideout scripts run every vblank and get scaled like the rest.
- `cmd_waitTime` (`0x088384E0`, counts frame ticks) is real time already: do not touch it.
- Test instances: port 9998 only; kill them by path (`*\build\rtw\*`), never by image name.
- Commits on `patapon3-windows` in `psprecomp/`, author as configured ("Patapon Community"), no Claude co-author trailer, message ends with `Verified: ...`.

## What is already known (22 Oct recon, build 5cbf2b8)

- **Repro (scripted, ~4 min):** `cd build/tools; TOUR_RT_PORT=9998 PSPRECOMP_PATCHES=60fps PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python gfx_tour.py --only rt --to mission_bubble2 --keep --out ../tour/<name>`. It stops on "¡Veamos cuánto podéis avanzar antes de que acabe el tiempo!"; X shows "¡Preparaos!"; the next X starts the countdown. (The tour's "two dialogs" are the PATAPATA hint and ¡Veamos...; ¡Preparaos! is the third.) 30 fps: same command without `PSPRECOMP_PATCHES`, with `PSPRECOMP_P3_ANIM_SMOOTH=0`.
- **Measure:** `python build/count60/count_cap.py 9998 9 <out>` presses X and logs every change of the bubble box (x 230..350, y 95..170 at 480x272) with its time, plus a time-stamped contact sheet `<out>.png`. Countdown bubbles appear at:
  - 30 fps: 0.25, 0.75, 1.24, 1.76 s (one per ~0.50 s, the beat);
  - 60 fps: 0.19, 0.44, 0.67, 0.93 s (one per ~0.245 s): **2x**.
  - The hint bubbles during the march ("PATAPATA PATAPON..." / "¡Veamos...!") alternate every **~4 s at 30** and **every 2.0 s at 60**: also 2x (same mechanism, fixed by the same change).
  - The mission clock (0:55 ...) runs in real time at both rates.
- **Mechanism:** function census over 2 s windows (`PSPRECOMP_FUNC_CENSUS=1`, socket `C`) at 60 fps in the mission: script object update `FUN_08909C7C` = VM entry `FUN_0883C088` = VM run `FUN_0883C0B0` = 360 calls/s before X (6 script objects x 60/s, every VM run comes from the script objects), `cmd_waitFrame 0x08838434` ~540 calls/s, `cmd_waitTime 0x088384E0` never called. `PSPRECOMP_FUNC_ARGS=0x08838434` after re-arm (`A`) + X: six VMs (a1 = 0x0963B3E0, 0x09C34260, 0x09C33A00, 0x09C331A0, 0x09639AC0, 0x0942A640) waiting 70+ runs each in 2 s.
- **cmd_waitFrame (`FUN_08838434(a0, vm)`), decompiled:** block `b = vm + rd32(vm + 0x360) * 0x118`; `b + 0x118` = "started" flag, `b + 0x120` = "command still running" (set by the VM when the command starts, cleared by the command when done), `b + 0x124` = frames left. First run (`started == 0`): `left = N` (argument via `FUN_0883C5E4(vm,0,1,4)`), `started = 1`; then every run, including the first: `if (left < 1) { left = 0; running = 0; } else left--`. So a wait of N frames ends on run N+1 and the first run leaves `left = N - 1` with `running != 0` (for N >= 1).
- Hideout: scripts are paced per object to 30 Hz (4100add, `script_update_30hz`, only while overlay id 2 is loaded); missions are overlay id 3 and unpaced.

## Review Focus

1. Other mission scripts' frame waits change too (tutorial bubbles, boss/enemy scripts, mission-end sequence, FEVER): they must now match 30 fps timing, and nothing may hang (a wait that never ends) — full 60 fps tour 32/32, mission-end -> chest-room delay compared with 30 fps.
2. Arena del Valor at 60 fps (the 4100add check): talk ends, clock runs, drum HUD.
3. Hideout at 60 fps: scripts still paced once (Silver Hoshipon star float ~4.4 s as in 6c50359, not 8.8 s); with `PSPRECOMP_P3_HIDEOUT_30HZ=0`, the wait scaling alone gives the same ~4.4 s.
4. A wait of 0 or 1 frame: N = 0 ends on the first run unscaled; N = 1 lasts 2 runs at 60 (one 30 fps frame).
5. 30 fps default and 30 fps + smooth: bit-identical (hook not installed).

---

### Task 1: Frame waits last their 30 fps time

**Files:**
- Create: `runtime/include/psp_frame_wait.h` (pure helper)
- Create: `runtime/tests/test_frame_wait.cpp`; Modify: `runtime/CMakeLists.txt` (test target `test_frame_wait`, test `frame_wait_tests`, same block style as `test_code_match`)
- Modify: `games/patapon3/runtime/hooks_main.cpp` (hook next to `script_update_30hz`; registration in the `60fps` block after the hideout pacing)
- Modify: `docs/ENV_FLAGS.md`, `ARCHITECTURE.md` (one clause where the hideout script pacing is described), `docs/superpowers/plans/2026-10-15-60fps.md` (one bullet), this plan (Evidence section)

**Interfaces:**
- Produces: `inline int32_t psp_frame_wait_left(int32_t left_after_first_run, int32_t runs_per_frame)` in `psp_frame_wait.h`: the frames-left value to store after a wait's first run so that a wait written as N frames (first run left `N - 1`) lasts `N * runs_per_frame` runs: returns `(left + 1) * runs_per_frame - 1`; returns `left` unchanged when `left < 0` or `runs_per_frame <= 1`.
- Produces: `void wait_frame_30hz(uint8_t* rdram, recomp_context* ctx)` registered on `0x08838434` (`kWaitFrame`), `FuncPtr g_wait_frame`.

- [ ] **Step 1: Baseline (RED measurement).** Repro at 60 and 30 (above), `count_cap.py` on the countdown and 12 s more for the hint bubbles: record bubble times and the hint period in `build/count60/notes.md`. Expected: countdown step ~0.245 s at 60 vs ~0.50 s at 30; hints 2.0 s vs ~4 s.
- [ ] **Step 2: Write the failing unit test** `runtime/tests/test_frame_wait.cpp` (CHECK macro style of `test_code_match.cpp`):
  - `psp_frame_wait_left(14, 2) == 29` (N = 15: 30 runs);
  - `psp_frame_wait_left(0, 2) == 1` (N = 1: 2 runs);
  - `psp_frame_wait_left(59, 2) == 119`;
  - `psp_frame_wait_left(14, 1) == 14` (30 fps: unchanged);
  - `psp_frame_wait_left(-1, 2) == -1` (defensive: never invent a wait).
- [ ] **Step 3: Run it** — `ninja -C build/rtw test_frame_wait`: compile FAIL (`psp_frame_wait.h` not found).
- [ ] **Step 4: Implement `psp_frame_wait_left`** in `runtime/include/psp_frame_wait.h` (header comment: a frame-counted wait written for 30 fps; generic, no game data).
- [ ] **Step 5: Run it** — `ninja -C build/rtw test_frame_wait && build/rtw/test_frame_wait.exe`: `frame_wait: all tests passed`.
- [ ] **Step 6: Implement `wait_frame_30hz`.** Read `vm = a1`, `b = vm + rd32(vm + 0x360) * 0x118`, `started = rd32(b + 0x118)`; call `g_wait_frame`; then, only if `started == 0`, `rd32(b + 0x120) != 0` (still waiting) and not (`g_script_update != nullptr && in_hideout(rdram)`), write `psp_frame_wait_left(rd32(b + 0x124), 2)` to `b + 0x124`. Re-read `rd32(vm + 0x360)` is not needed (the command cannot change the VM frame). Comment block above it in the style of the hideout pacing comment: what was 2x (countdown bubbles 0.245 s vs 0.50 s, hints 2 s vs 4 s), why not pace the scripts (4100add), the field layout. Register in the `60fps` block, gated by `PSPRECOMP_P3_SCRIPT_WAIT_30HZ` (`'0'` = off), printing the install line.
- [ ] **Step 7: Build and unit tests.** `ninja -C build/rtw` then `cd build/rtw && ctest`: `100% tests passed` (97 + 1).
- [ ] **Step 8: GREEN measurement.** Repro at 60 with the new exe: countdown bubbles one per 0.50 s ± 5 % (same as 30 fps), hint bubbles every ~4 s ± 5 %; with `PSPRECOMP_P3_SCRIPT_WAIT_30HZ=0`: back to 0.245 s / 2 s. Save both contact sheets.
- [ ] **Step 9: Review Focus checks.**
  1. Full 60 fps tour (`gfx_tour.py --only rt`): 32/32; time from `mission_clear` to `reward_chest` in the tour log vs a 30 fps tour (within a few seconds; it was 25 s at both rates before, note if it changes) — and look at the mission frames.
  2. Arena route at 60 fps (`TOUR_SAVE=build/save_arena`, `--checkpoints gfx_arena.json --keep`), then X a few times: talk over, clock runs, drum HUD.
  3. Hideout at 60: `python build/tools/prop_period.py` on the Silver Hoshipon as in `2026-10-21-60fps-hideout-anims.md` (float ~4.4 s); then once with `PSPRECOMP_P3_HIDEOUT_30HZ=0` (expected ~4.4 s from the wait scaling alone; if not, the star's motion is per-run steps, which this plan does not cover: write it down).
  4. Covered by the unit test (N = 0 is ended by the original before the hook writes anything: `running == 0`).
- [ ] **Step 10: Gates.** 30 fps tour + `python -I build/tools/rt_regress.py build/tour/baked30 build/tour/<new>`: `RT_REGRESS PASS`.
- [ ] **Step 11: Docs, commit.**

```bash
git add runtime/include/psp_frame_wait.h runtime/tests/test_frame_wait.cpp runtime/CMakeLists.txt games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md ARCHITECTURE.md docs/superpowers/plans/2026-10-15-60fps.md docs/superpowers/plans/2026-10-22-60fps-mission-countdown.md
git commit -m "fix(p3): script frame waits keep their 30 fps length at 60 fps"
```
Message body: the countdown and hints were 2x, cmd_waitFrame counts script runs, the scaling, why not pacing, before/after times, `Verified:` line.

### Task 2: Deploy and hand off (no push)

**Files:**
- Create: `build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-10-22c.md` (Spanish, for the user)

- [ ] **Step 1: Variants.** `ninja -C build/vanilla/rtw psprecomp_runtime` and `ninja -C build/v115b/rtw psprecomp_runtime`: link with no error.
- [ ] **Step 2: Deploy with backup** to `build/e2e_portable` and `build/e2e_portable3` (`game/dxd-1.1.4` <- `rtw`, `game/dxd-1.1.5b` <- `v115b/rtw`, `game/vanilla-eu` <- `vanilla/rtw`), backup first to `build/backup_pre_waits_2026-10-22/<portable>/<variant>/`. If an exe is in use the user is playing: skip that copy and say so. `cmp` each deployed exe with its build.
- [ ] **Step 3: Hand-off** in Spanish: qué pasaba (las esperas de los scripts cuentan frames), qué cambió, tabla antes/después (cuenta atrás y pistas), cómo probarlo (Campos de prueba, primera misión, pasar los diálogos con X, mirar ¡TON! ¡CHIN! ¡KAN! ¡Ya!), flag A/B, qué más puede notar distinto (otras esperas de misión ahora al tiempo de 30 fps), desplegado + backup, "nada subido; va en push-p3-31 tras tu OK".
- [ ] **Step 4: Memory.** Update `p3-60fps.md` and its `MEMORY.md` line (local commit hash, not pushed, next push branch).

## Evidence (22 Oct, executed inline)

| Measure (Campos de prueba, first mission) | 30 fps | 60 fps before | 60 fps with the fix | fix off (`_SCRIPT_WAIT_30HZ=0`) |
|---|---|---|---|---|
| Countdown bubbles TON / CHIN / KAN / Ya | 0.25 / 0.75 / 1.24 / 1.76 s | 0.19 / 0.44 / 0.67 / 0.93 s | 0.22 / 0.72 / 1.22 / 1.71 s | 0.20 / 0.44 / 0.68 / 0.95 s |
| March hint bubbles | ~4 s apart | 2.0 s | ~4 s (4.2, 8.2, 12.2) | 2.0 s |
| Mission clock at tour checkpoints 24 / 26 / 27 / 28 | 0:41 / 0:31 / 0:21 / 0:17 | 0:55 / 0:40 / 0:30 / 0:15 | 0:41 / 0:31 / 0:20 / 0:16 | — |
| Time-up -> MISION COMPLETADA / chest room | ~13 s / ~18 s | — | ~13 s / ~17 s (1 s sampling) | — |

- Unit test `frame_wait_tests`: compile FAIL before the header, then `frame_wait: all tests passed`; ctest 98/98.
- Full 60 fps tour (`tour/wait60full`): 32/32, mission and reward frames normal.
- Arena del Valor at 60 (`tour/arena60w`): talk over, clock 4:27 -> 4:22 in 5 s, drum HUD, battle running.
- Hideout at 60, Silver Hoshipon float: 4.36 s with the hideout pacing (not 8.8: the hideout is not scaled twice);
  with `PSPRECOMP_P3_HIDEOUT_30HZ=0` the wait scaling alone gives 4.35 / 4.42 s (2.2 s unpaced on 21 Oct).
- Step 1 baseline was the recon measurement on the same code (5cbf2b8; bf3ac0f is docs only).
- Notes, scripts and contact sheets: `build/count60/` (outside the repo).
