# 60 fps Hero Gate particles and hideout halo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At 60 fps (`PSPRECOMP_PATCHES=60fps`), two hideout effects must move at the same real-time speed as at 30 fps **and move every frame (smooth)**:
- the particles of the Hero Gate ("Heropuerta") behind the rock that covers it (inside the dark arch);
- the circular halo under the selected place on the hideout map.

**Architecture:** Same shape as the weather fix (`2026-10-21-60fps-weather.md`) and the hideout scripts fix (`2026-10-21-60fps-hideout-anims.md`):
1. Measure the 2x as a **speed**, not a period (see "What is already known").
2. Find the per-frame step with a live write watch.
3. With the 60fps set only, pace that step to 30 Hz, or halve its per-call advance.
4. Add the smooth half-step on top.

The game module (`games/patapon3/runtime/hooks_main.cpp`) owns the hooks. Generic helpers already exist in `runtime/include/`:
- `psp_paced_step.h` (`PacedStepTable`, per object, banked frame time);
- `psp_particle_nudge.h` (particle lists drawn half a step ahead);
- `psp_half_ahead.h` (a few float words of one object shown half a step ahead).

**Tech Stack:** C++20 runtime + game module (`psprecomp/`), ninja + ctest in `build/rtw`, Python 3 tools in `build/tools/` (outside the repo), debug socket on port 9998.

**Spec:** the user's request (2026-10-21), no separate design doc:
> "Siguiente arreglo de suavidad y que va rápido, en la heropuerta/herogate, sus destellos detras de la roca que los cubren van rápido, podrias agregar la suavidad y ajustar la velcoidad de animacion? así como el destello circular que tiene el escondite cuando te situas en cada uno de los lugares (cuartel, totem, fragua, etc)"

Clarifications, same day:
> "los anillos es estatico, o sea lo morado que cuelga de arriba"

> "no, me referia q eu debes ignorar eso, la heropuerta tiene sus particulas detras de la piedra que lo cubre"

Reading:
- **Out of scope:** the purple decoration hanging from the ceiling above the gate (planet, stars, sun, glow). The user says to ignore it.
- **In scope:** the Hero Gate's own particles, which show behind the dark rock (the arch) that covers the gate. They move too fast.

The circular halo is the glow under whichever place is selected (Cuartel, Tótem de equipo, Armería, Heropuerta, Fragua, Obelisco Maestro, ...). The circular halo is the glow under whichever place is selected (Cuartel, Tótem de equipo, Armería, Heropuerta, Fragua, Obelisco Maestro, ...). Standing rule:
> "toda animación que se arregle debe tener su smoothness"

## What is already known (21 Oct recon, `build/gate60/notes.md`)

- **Build state:** the current build already paces every script object to 30 Hz at 60 fps (`757d754`, `PSPRECOMP_P3_HIDEOUT_30HZ`). It also smooths script-moved 3D nodes (`c0496bb`).
- **Route:** boot with `weather_drive.py` (see Task 1); READY lands with the Cuartel selected. From there:
  - `P L` → Tótem de equipo;
  - `P L` ×2 → Armería;
  - `P L` ×3 → Heropuerta;
  - `P L` ×4 → Fragua;
  - `P R`, `P R` → Obelisco Maestro.

  Wait ~2 s after a move: the map fades for a moment.
- **Same cadence at 30 and 60, even with every 60 fps fix off.** Measured with `prop_period.py` (autocorrelation):

  | Effect | Box | 30 fps | 60 fps, `PSPRECOMP_P3_HIDEOUT_30HZ=0` |
  |---|---|---|---|
  | Halo under the Cuartel | 180 165 215 215 | 1.067 / 1.074 s | 1.067 / 1.076 s |
  | Hanging decoration (out of scope) | 160 40 270 120 | 1.070 / 1.064 s | 1.067 / 1.069 s |
  | Inside the gate's arch (Heropuerta selected) | 205 105 255 175 | 3.059 s (a noisy run gave 2.102) | not measured; current build 3.021 / 3.020 s |

  The arch box is the first guess for the particles behind the rock. Its cadence also matches, but the hook-off run is missing (Task 1 measures it). So the 2x is probably not in when things appear. It is most likely in how far each sparkle or ring moves, grows or fades per frame. The weather was the same: real-time spawn timers, a fixed step per call. **Measure a speed** (expansion px/s, drift px/s, lifetime s), not a period.
- **Size:** the sparkles are 1-3 px at render scale 1. Use render scale 3 (`weather_drive.py OUTDIR 3`); boxes stay in 480x272 units.
- **Owner:** all hideout objects update off-screen, so a census view diff cannot isolate them. What works:
  1. find a word that follows the effect (`corr_scan.py`);
  2. watch its writer live with the debug socket command `H <hexaddr>` (`98cb73b`; `[HW-WATCH]` + `[BT] hw-watch:` lines in `build/logs/tour_rt.log`, symbolize with `build/tools/bt.py <log> <exe>`; strip leading spaces first).
- **Code reading:** a scene object's update is its vtable slot +20 (a0 = object, a1 = frame ticks, 4800/s). The scene manager `FUN_0889459c` calls it and ignores the return value. Overlay functions show as `ovMission:FUN_...` in backtraces: hook their main-image callee or caller instead (see Task 2 Step 4).

## Global Constraints

- Work on the local branch `patapon3-windows` of `psprecomp/`. **Do not push.**
  - The push goes in branch `push-p3-28` after the user's OK, together with the weather (`e548bab..9365efa`) and hideout-scripts (`98cb73b..6c50359`) commits.
  - Never push `067ac6e` (debug menu) or `f639180` (icon).
- Commits: author "Patapon Community", English messages, no `Co-Authored-By` line.
- Tests run only on port **9998**. Never touch the user's game on 9999. Nothing destructive, no installs.
- Never write `DECO/savedata*` or `build/save_main/`. Use `build/save_weather` (the driver copies it to `build/tmp_saves/<tag>`).
- The 30 fps game must not change:
  - every new hook is registered only when `psp_patch_set_active("60fps")`;
  - each hook has an env to turn it off: `PSPRECOMP_P3_HIDEOUT_FX_30HZ=0`, `PSPRECOMP_P3_HIDEOUT_FX_SMOOTH=0`.
- Never test with `PSPRECOMP_NO_AUDIO=1`.
- Do not build or run ctest while an instance is booting or measuring.
- Run test instances from a copy of the exe: `build/rtw/psprecomp_runtime_wx.exe`, set with `P3DRIVE_EXE`. Delete the copy at the end.
- If a boot ends with `hideout landing FAILED` (seen once in 5 on 21 Oct: stuck on the DxD credits' terms dialog), stop it (`echo Q >> OUTDIR/cmd.txt`) and boot again once before suspecting a change.
- `rt_regress` base: tour the **deployed** exe the same day (`build/e2e_portable3/game/dxd-1.1.4/psprecomp_runtime.exe` copied to `build/rtw/psprecomp_runtime_base.exe`, `P3DRIVE_EXE`). A connected/disconnected controller changes the help-bar glyphs between days.
- **Smooth rule (user, 21 Oct):** every effect this plan fixes must also move every frame at 60 fps (Task 4).
  - Pacing to 30 Hz alone is not a finished fix.
  - If one cannot be smoothed, the hand-off says which one and why. It never ships silently at 30 Hz.
- Stuck 45 min or 3 attempts on one step: write it in the hand-off, take the step's fallback, move on.
- Talk to the user in Spanish; code, commits and docs in English.

## Review Focus

1. **Every place's halo** ("cada uno de los lugares"): the fix must hold for the halo under each place, not only the Cuartel. Task 3 Step 4 measures the halo at the Cuartel, Tótem and Fragua.
2. **The same effect class elsewhere:** the sparkle or halo class may also run in missions, the world map or menus. Paced there too, it must keep its 30 fps speed. Task 3 Step 5 runs the weather mission (`wsession.sh`) and checks rain 29.7 ± 3 / wind 29.8 ± 3 steps/s, FEVER held, and one screenshot sheet of any sparkle effect seen.
3. **Moving the cursor between places:** a halo is (re)started on each move. The first frames after a move must not jump or flash (stale pacing or half-step state). `PacedStepTable::kStaleVblanks` covers the pacing; Task 4 Step 6 checks 8 shots 0.1 s apart right after `P L`.
4. **The game slower than 60 on its own** (two vblanks per frame): a paced step then runs every frame, and nothing is drawn half a step ahead. This is covered by `PacedStepTable` and the vblank checks. Task 4 Step 6 reads the trace while the map scrolls (the heaviest hideout moment).
5. **The three releases:** hooked functions must be byte-identical in DxD 1.1.4, 1.1.5b and vanilla generated code (`build/p3_output`, `build/v115b/p3_output`, `build/vanilla/p3_output`; overlay banks in `build/ovl/*_output`). Task 3 Step 1.

---

### Task 1: Measure the 2x as a speed (30 vs 60)

Outside the repo, no commit. Evidence goes in `build/gate60/notes.md`.

**Files:**
- Create: `build/tools/ring_speed.py PORT SECONDS CX CY R_MAX [label]` (outside the repo)
- Create: `build/tools/sparkle_life.py PORT SECONDS X0 Y0 X1 Y1 [label]` (outside the repo)

**Interfaces:**
- Produces, used by Tasks 2-4:
  - `HALO_CENTER` (cx, cy, r_max) for the Cuartel, Tótem and Fragua;
  - `GATE_BOX`, the box of the gate particles behind its rock (inside the arch);
  - the 30 / 60-hook-off / 60-current speeds of each effect.

- [ ] **Step 1: Write `ring_speed.py`**
  - Capture frames as fast as the socket allows. Reuse the `gfx_tour.RtSide.frame()` pattern from `prop_period.py`.
  - For each capture, build the radial mean-brightness profile around (cx, cy) in 1-unit rings up to r_max, then subtract the per-radius median over all captures.
  - Track the brightest ring (the wave front) from capture to capture. Keep only steps where it moves outward by less than r_max / 4.
  - Print `label front_speed=<median units/s> fronts=<n> captures_per_s=<c>`.
  - If the halo has no outward wave (it only pulses in place), print `front_speed=nan`. Then also print `rise_s`, the median time from the 20 % to the 80 % brightness level of the mean ring brightness. That is the speed of a pulse.

- [ ] **Step 2: Write `sparkle_life.py`**
  - Per capture, take the bright spots in the box: pixels at least 40 brighter than the box's per-pixel median over all captures. Group them into 4-connected blobs of at most 30 px at scale 3.
  - Link each blob to the nearest blob of the previous capture within 6 units.
  - Print the median track lifetime (s), the median drift speed (units/s), and the number of tracks.
  - Print `label life_s=<l> drift=<d> tracks=<n> captures_per_s=<c>`.

- [ ] **Step 3: Measure at 30, at 60 with hooks off, and at 60 current.** Three boots at render scale 3, each with:

  ```
  P3DRIVE_EXE=psprecomp_runtime_wx.exe TOUR_SAVE=C:/Users/torso/Documents/deco/build/save_weather PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 PSPRECOMP_FUNC_CENSUS=1 python -I build/tools/weather_drive.py <OUTDIR> 3
  ```

  The second boot adds `PSPRECOMP_PATCHES=60fps PSPRECOMP_P3_HIDEOUT_30HZ=0`; the third adds `PSPRECOMP_PATCHES=60fps`.

  In each boot:
  1. Take a full screenshot (`wshot.py`) and choose the halo centre and the gate box from it. The gate box covers the particles behind the gate's rock (start from 205 105 255 175 and fit it to where they show). It must avoid the halo rings around the selected gate and the hanging decoration above (out of scope).
  2. Run each tool 3 times, 12 s each: the halo at the Cuartel, Tótem and Fragua, and the gate particles with the Heropuerta selected.
  3. Save a 16-frame sheet (`prop_sheet.py ... 0.05`) of each, for the hand-off.

  Expected: three runs within 10 %, and at least one quantity about 2x at 60-hook-off against 30. If the current build already matches 30 (the scripts fix covered it), only the smooth task remains for that effect: say so in the notes.

  If neither effect shows any 2x speed, stop. Write the measurements and sheets in the hand-off and ask the user which motion they see as fast. Do not guess a fix.

### Task 2: Find the step that moves them

Outside the repo, except a new diagnostic, which gets its own commit.

**Interfaces:**
- Consumes: Task 1's boxes, centres and speeds.
- Produces, in `build/gate60/notes.md`, per effect:
  - the step function (guest address, main image or overlay);
  - what it advances per call (float offset, scale, alpha, sprite frame, particle list);
  - the fix shape (S1 / S2 / S3 below);
  - for Task 4: the guest words that hold the drawn state, with offsets and types, and the function that draws it.

- [ ] **Step 1: Find a word that follows the effect.** Use `corr_scan.py` (60 fps, hooks off, scale 1, 6-10 dumps of `09800000-09D00000`). Its centroid is "pixels with mean > 200". For a dimmer effect, copy it to `corr_scan2.py` with a brightness threshold argument; do not change the old tool.
  - For the halo, correlate with the ring brightness or radius series from `ring_speed.py`. A small change: also write the series to a CSV and pass it in.
  - For the gate particles, one particle's centroid is too short-lived. Correlate with the gate box's total particle brightness instead, or look for words that step by a constant every frame (2 dumps one frame apart are not possible; use 20 dumps and keep words whose per-dump change is about constant × elapsed frames).

  Expected: a float word whose value follows the effect, with |corr| > 0.9.

- [ ] **Step 2: Watch its writer live.**
  1. Send `H <word>`, wait 2 s, and collect the `[BT] hw-watch:` lines.
  2. Symbolize them with `bt.py`.
  3. Read the writer and walk up the chain until a function with a0 = object and a1 = frame ticks, or one that adds a constant per call.

  Expected: a chain like the weather (`slot +20 update -> per-particle step`) or a draw-time animation (texture / UV offset advanced per call).

- [ ] **Step 3: Classify the fix shape.**

  | Shape | When | Fix in Task 3 |
  |---|---|---|
  | **S1** | moved by a script (`FUN_0883C0B0` in the chain) | already paced since `757d754`. Re-measure with the current build; if it is still 2x, the script runs from a different VM entry. Find it and add it to the paced set |
  | **S2** | an object update (a0 = object, a1 = frame ticks) that moves state by a fixed step per call | `PacedStepTable` per object, pattern `weather_step_30hz` |
  | **S3** | a float phase / offset / scale advanced by a constant per call, everything else time-based | run every frame with the constant halved for the call, pattern `ribbon_step_smooth`. This is already smooth |

- [ ] **Step 4: Overlay check.** If the step is `ovMission:` / an overlay bank function, grep `runtime/src/psp_dispatch.cpp` for how bank functions dispatch, and decide whether `psp_dispatch_register` reaches it. If it does not, hook the nearest main-image function in the chain whose arguments identify the object. Write that function in the notes.

- [ ] **Step 5: Commit any new reusable diagnostic.** A tracer behind `PSPRECOMP_P3_HIDEOUT_FX_TRACE=1` gets an `ENV_FLAGS.md` entry and its own commit. Throwaway probes stay in `build/tools`.

### Task 3: Speed fix

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp`: a new section after the "60 fps scripts" section, registered inside `psp_patch_set_active("60fps")`.
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_P3_HIDEOUT_FX_30HZ`; also the `PSPRECOMP_PATCHES` paragraph's list).
- Modify: `ARCHITECTURE.md`, one clause in the 60 fps paragraph.
- Modify: `docs/superpowers/plans/2026-10-15-60fps.md` ("Known gaps": a Hero Gate / halo bullet).

**Interfaces:**
- Consumes: Task 2's addresses and shape per effect.
- Produces:
  - the hook(s);
  - env `PSPRECOMP_P3_HIDEOUT_FX_30HZ` (`0` = off; default on with the 60fps set);
  - the log line `[P3] 60fps: hideout effects paced to 30 Hz (Hero Gate particles, place halo)`;
  - for S2: `PacedStepTable g_fx_paced[...]` (Task 4 reads `skipped(obj)`).

- [ ] **Step 1: Same code in the three releases.** Diff each hooked function's body in the three outputs (the `awk '/\* 0xADDR \*/'..'^}'` + `md5sum` recipe in the hideout plan's evidence). Expected: identical. If one differs, register only where it matches and ledger a ruling.

- [ ] **Step 2: RED.** At 60 with `PSPRECOMP_P3_HIDEOUT_FX_30HZ=0`, rerun Task 1's tools for the affected effects. Expected: the Task 1 60-hook-off speeds (2x).

- [ ] **Step 3: Implement** the shape from Task 2, with a comment block in the file's style:
  - what runs per frame;
  - the addresses and offsets;
  - why pacing or halving is right.

- [ ] **Step 4: GREEN.**
  1. Build, copy to `psprecomp_runtime_wx.exe`, boot at 60 (scale 3).
  2. Rerun the tools: halo at the Cuartel, Tótem and Fragua (Review Focus 1); gate particles at the Heropuerta.

  Expected: every speed within 10 % of 30 fps, three runs each.

- [ ] **Step 5: Review Focus 2.**
  1. Run `PSPRECOMP_PATCHES=60fps PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 PSPRECOMP_FUNC_ARGS=089010DC,08902EE0 sh build/tools/wsession.sh C:/Users/torso/Documents/deco/build/gate60/mission`.
  2. Measure with `wrain.py 3 B` (retry every 8 s until it rains) and `wribbon.py 5`.
  3. Stop with `wstop.sh`.

  Expected: rain 29.7 ± 3 and wind 29.8 ± 3 steps/s, FEVER held.

- [ ] **Step 6: ctest and rt_regress.**
  1. Run `ctest --test-dir build/rtw -j8`. Expected: 95/95, or 95 + new tests.
  2. Tour the new build at 30: `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/gate_new`.
  3. Tour the deployed exe the same way (Global Constraints) into `build/tour/gate_base`.
  4. Compare: `python -I build/tools/rt_regress.py build/tour/gate_base build/tour/gate_new`. Expected: `RT_REGRESS PASS`.

- [ ] **Step 7: Docs and commit.**

```bash
git add games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md ARCHITECTURE.md docs/superpowers/plans/2026-10-15-60fps.md
git commit -m "fix(p3): Hero Gate particles and place halo keep their 30 fps speed at 60 fps"
```

### Task 4: Smooth (move every frame at the 30 fps speed)

Required by the smooth rule. For S3 this task only measures. For S1 / S2, the effect must also move on the frames where its step did not run.

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp` (the Task 3 section).
- Test: a new test only if a helper gains behaviour (`runtime/tests/test_half_ahead.cpp`, `test_particle_nudge.cpp`).
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_P3_HIDEOUT_FX_SMOOTH`, `PSPRECOMP_P3_HIDEOUT_FX_TRACE`) and `ARCHITECTURE.md` (same clause).

**Interfaces:**
- Consumes: Task 3's `skipped(obj)`, and Task 2's drawn-state words and draw function.
- Uses, unchanged:
  - `psp_particle_nudge(rdram, head, pos_off, vel_off, k, max_nodes, &undo)` / `psp_particle_restore(rdram, undo)`, for a particle list with a per-particle velocity;
  - `psp_half_ahead_record(&h, rdram, base, offs, n)` / `psp_half_ahead_apply(&h, rdram, max_jump)` / `psp_half_ahead_restore(h, rdram)`, for a few float words per object (scale, alpha, offset).
- Produces:
  - env `PSPRECOMP_P3_HIDEOUT_FX_SMOOTH` (`0` = plain pacing; default on);
  - env `PSPRECOMP_P3_HIDEOUT_FX_TRACE=1`: every ~2 s, the draws per second and how many were half a step ahead, per effect (pattern `hideout_count`);
  - the log line `[P3] 60fps: smooth hideout effects (Hero Gate particles, place halo)`.

- [ ] **Step 1: Choose the helper per effect.**
  - Particles with a velocity: `psp_particle_nudge`, k = 0.5.
  - Per-object float words: `psp_half_ahead`.
  - A per-call float advance: S3 halving, already smooth.
  - A state that is an integer sprite-frame index cannot be half-stepped. Stop, and record which effect and why for the hand-off (smooth rule).

- [ ] **Step 2: If a helper needs a new behaviour, write its failing test first** (same `CHECK` style). Run `ninja -C build/rtw test_<name>` and run it. Expected: FAIL. Implement, then run again. Expected: PASS. If no helper changes, skip this step and say so in the ledger.

- [ ] **Step 3: Wire it.**
  1. Hook the effect's draw, or the matrix/state build the draw reads.
  2. On a frame where `skipped(obj)` (S2), or one vblank after the recorded run (the `node_matrix_smooth` pattern), apply, call the original, restore.
  3. Set `max_jump` per word to 4x the largest per-step change seen in Task 2, as a named constant with that origin in a comment.

- [ ] **Step 4: Trace.** Add the `PSPRECOMP_P3_HIDEOUT_FX_TRACE` counters.

- [ ] **Step 5: Build and boot at 60 (scale 3) with the trace on.**

- [ ] **Step 6: Measure.**
  - **Speed:** every Task 3 Step 4 measurement is still within 10 % of 30 fps (smooth on).
  - **Smooth:** the trace shows about 60 draws/s per effect object, and about 30/s half a step ahead (S1/S2). For S3, steps are about 60/s.
  - **Look:** 4 consecutive screenshots of each box show no jump back, no slide and no doubled sparkle.
  - **Review Focus 3:** 8 shots 0.1 s apart right after `P L` from the Cuartel show no flash or jump in the new place's halo.
  - **Review Focus 4:** the trace counters while pressing `P L` / `P R` repeatedly (map scrolling) show no half-step frames when draws drop below 60/s.
  - If the speed is off by more than 10 %, or something slides: set that effect's smooth default to off, ledger it and tell the user.

- [ ] **Step 7: ctest, docs, commit.** Expected: ctest all pass.

```bash
git add games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md ARCHITECTURE.md runtime/include runtime/tests runtime/CMakeLists.txt
git commit -m "feat(p3): Hero Gate particles and place halo move smoothly at 60 fps"
```

### Task 5: Variants, deploy, hand-off

**Files:**
- Deploy: the three exes into `build/e2e_portable3/game/<release>/` and `build/e2e_portable/game/<release>/`, after a backup into `build/backup_pre_herogate_2026-10-21/<portable>/<release>/` (same as `build/backup_pre_hideout_2026-10-21`).
  - Releases: `dxd-1.1.4` (`build/rtw`), `dxd-1.1.5b` (`build/v115b/rtw`), `vanilla-eu` (`build/vanilla/rtw`).
  - First check that no `psprecomp_runtime` process runs from those folders (`tasklist`).
- Create: `build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-10-21d.md` (Spanish).
- Modify: memory `p3-60fps.md` and its `MEMORY.md` line.

- [ ] **Step 1: Variants.**
  1. Run `P3_VARIANT=v115b bash games/patapon3/scripts/p3.sh build`, then the same with `vanilla` (rc 0 each).
  2. For each variant, run `PSPRECOMP_PATCHES=60fps python -I build/tools/movie_check.py <variant> fx_<variant>`.

  Expected: `MOVIE OK`, and both new log lines in `build/logs/fx_<variant>.log`.

- [ ] **Step 2: Deploy with backup.** `md5sum` of the source exe and both portable copies must match for each release.

- [ ] **Step 3: Fill this plan's Evidence section** and commit it: `docs: 60 fps Hero Gate / halo plan evidence`.

- [ ] **Step 4: Hand-off and memory.** The hand-off, in Spanish, says:
  - what was wrong and what changed;
  - a table of speeds: 30 / 60 before / 60 after;
  - how to try it: the launcher at 60 fps, select the Heropuerta and the other places;
  - smooth status per effect (moves every frame, or why not);
  - anything seen at 2x and not fixed;
  - "nada subido; va en push-p3-28 con lo del clima y el escondite tras tu OK".

## Evidence (fill in during execution)

- Task 1: boxes/centres; speeds 30 / 60-off / 60-current per effect; sheets.
- Task 2: word, writer chain, step address, shape, drawn-state words.
- Task 3: RED / GREEN speeds (3 places + gate), Review Focus 2, ctest, rt_regress.
- Task 4: helper per effect, trace counters, speeds with smooth on, Review Focus 3-4.
- Task 5: variants, deploy hashes.
