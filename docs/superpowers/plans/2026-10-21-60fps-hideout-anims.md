# 60 fps hideout animations (Silver Hoshipon, obelisk owl) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At 60 fps (`PSPRECOMP_PATCHES=60fps`) the hideout's Silver Hoshipon ("Plata Hoshipon") and the owl on top of the Master Obelisk move at the same real-time speed as at 30 fps **and move every frame (smooth)**: the speed fix alone is not done.

**Architecture:** Same shape as the weather fix (`2026-10-21-60fps-weather.md`): first measure the 2x and find the per-frame step that drives these two props; then, only with the 60fps set, pace that step to 30 Hz, or halve its per-call advance where the state is a float and the step can run every frame. The game module (`games/patapon3/runtime/hooks_main.cpp`) owns the hooks. Generic helpers already exist: `runtime/include/psp_paced_step.h` (per-object pacing with banked frame time, `PacedStepTable`) and `psp_particle_nudge.h`.

**Tech Stack:** C++20 runtime + game module (`psprecomp/`), ninja + ctest in `build/rtw`, Python 3 tools in `build/tools/` (outside the repo), debug socket on port 9998.

**Spec:** the user's requests (2026-10-21), no separate design doc:
> "Ahora arreglemos las siguientes animaciones, son del escondite así que deberían ser más rápidas de testear, por ejemplo, plata hoshipon y el búho que está arriba del obelisco, van al doble."

Follow-up, same day (a standing rule for every 60 fps fix):
> "Y no olvides agregar animaciones smooth, agreguemos esa regla, toda animación que se arregle debe tener su smoothness."

Reading: hideout animations that run 2x at 60 fps. The two named ones are examples ("por ejemplo"): any other hideout prop driven by the same step is in scope and gets fixed by the same hook. Props driven by a different step are listed in the hand-off, not fixed.

## What is already known (21 Oct, before this plan)

- Boot to the hideout with Edson's save: `TOUR_SAVE=C:/Users/torso/Documents/deco/build/save_weather PSPRECOMP_PATCHES=60fps PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/weather_drive.py <OUTDIR> 1` (READY after ~75 s, Cuartel selected). `P R`, `P R` from the Cuartel shows the "Obelisco Maestro" view: the owl is the black silhouette on top of the obelisk (screen ~240,165); Silver Hoshipon is the star with a figure inside, floating at ~278,150. Zoomed frames: `build/weather60/live/obel_zoom.png`.
- All hideout objects update whether on screen or not: a census in the obelisk view and in the far-left view gave the same function set (`build/weather60/census/h_obel.txt`, `h_left.txt`). A view diff cannot isolate them.
- GMO motion players are time-based: the motion tick `FUN_08878A18` (a0 = player, a1 = frame time in ticks, 4800/s) advances the clock +104 by `rate * FUN_08875D78(a1)`. A 2x there would need a 2x rate; check it, do not assume it.
- The script VM has two waits: `cmd_waitFrame` `0x08838434` counts down 1 per call (frames), `cmd_waitTime` `0x088384E0` counts down real ticks. The mission weather scripts use real time (weather plan evidence). A hideout actor script that loops "move / waitFrame" runs 2x at 60. This is the lead hypothesis.
- Madwig's 60fps set only removes vblank waits and halves the unit march (`game.toml` `[[patch_sets]] 60fps`).

## Global Constraints

- Work on the local branch `patapon3-windows` of `psprecomp/`. **Do not push.** The push goes in branch `push-p3-28` after the user's OK, together with the weather commits. Never push `067ac6e` (debug menu) or `f639180` (icon).
- Commits: author "Patapon Community", English messages, no `Co-Authored-By` line.
- Tests only on port **9998**. Never touch the user's game on 9999. Nothing destructive, no installs.
- Never write `DECO/savedata*` or `build/save_main/`. Use `build/save_weather` (a copy; refresh it from `build/save_main/test` if the game autosaved over it).
- The 30 fps game must not change: every new hook is registered only when `psp_patch_set_active("60fps")`, and each has an env to turn it off (`PSPRECOMP_P3_HIDEOUT_30HZ=0`).
- Never test with `PSPRECOMP_NO_AUDIO=1`. Do not build or run ctest while an instance is booting or measuring: a busy PC breaks the boot's timed key presses (seen 21 Oct).
- Run test instances from a copy of the exe (`build/rtw/psprecomp_runtime_wx.exe`, `P3DRIVE_EXE`), so `ninja` can relink `psprecomp_runtime.exe` meanwhile; delete the copy at the end.
- **Smooth rule (user, 21 Oct):** every animation this plan fixes must also move every frame at 60 fps (Task 4). Pacing to 30 Hz alone is not a finished fix. If a motion truly cannot be smoothed, the hand-off says which and why; it never ships silently at 30 Hz.
- Stuck 45 min or 3 attempts on one step: write it in the hand-off, take the step's fallback, move on.
- Talk to the user in Spanish; code, commits and docs in English.

## Review Focus

1. **Scripted props that wait in frames on purpose next to time-based ones** (dialogue, the Hoshipon's reaction when spoken to, building menus opening): if the fix paces `cmd_waitFrame` for every script, menus and talk sequences in the hideout must keep their 30 fps timing too, not become slow. Task 3 Step 5 checks one menu open and one talk.
2. **Missions use the same script VM:** a global `waitFrame` fix also changes mission scripts at 60 fps (FEVER worm, boss patterns, cutscene timing). Expected: they move like at 30 fps (they were 2x too, or used `waitTime`). Task 3 Step 5 runs the Campo de los Gigantes mission for 2 minutes and checks the weather rates stay 29.7 / 29.8 (`wrain.py`, `wribbon.py`) and nothing waits twice as long as at 30.
3. **Game slower than 60 on its own** (two vblanks per frame): a paced step then runs every frame; the props must not drop to half speed. Covered by `VblankPacer` / `PacedStepTable` semantics; Task 3 Step 4 measures with the frame log on.
4. **The three releases:** hooked functions must be byte-identical in DxD 1.1.4, 1.1.5b and vanilla generated code (`build/p3_output`, `build/v115b/p3_output`, `build/vanilla/p3_output`). Task 3 Step 1.
5. **Leaving and re-entering the hideout** (mission and back, title and back): stale pacing state must not make the first frames jump. Covered by `PacedStepTable::kStaleVblanks`; check once in Task 4 Step 1.

---

### Task 1: Measure the 2x (30 vs 60) for both props

Outside the repo: no commit. Evidence in `build/hideout60/notes.md`.

**Files:**
- Create: `build/tools/prop_period.py PORT SECONDS X0 Y0 X1 Y1 [label]` (outside the repo)

**Interfaces:**
- Produces: the motion period (s) of each prop at 30 and 60 fps; the screen boxes `OWL_BOX` and `STAR_BOX` used by Tasks 2-4.

- [ ] **Step 1: Write `prop_period.py`**

Capture the box as fast as the debug socket allows (reuse `gfx_tour.RtSide.frame()` like `build/tools/anim_rate.py`), turn each capture into a vector (grayscale, box pixels), and estimate the motion period from the autocorrelation of the mean-subtracted sequence, with time stamps from `time.perf_counter()` (the period is the first autocorrelation peak after the first zero crossing). Print `label period_s=<p> captures_per_s=<c>`.

- [ ] **Step 2: Measure at 60**

Boot as in "What is already known", go to the obelisk view, then run:
- owl: `python build/tools/prop_period.py 9998 12 225 150 260 185 owl60`
- star: `python build/tools/prop_period.py 9998 12 255 120 305 175 star60`

Adjust the boxes from a full screenshot first (`build/tools/wshot.py`). Each box should hold one prop and no other moving thing.

Expected: a stable period for each prop (three runs within 10 %).

- [ ] **Step 3: Measure at 30** (same boot without `PSPRECOMP_PATCHES`).

Expected: 30 fps periods ≈ 2x the 60 fps ones. If a prop is not 2x, record it and drop it from scope (tell the user).

### Task 2: Find the step that moves them

Outside the repo, except the diagnostic, which is committed if it is new.

**Interfaces:**
- Consumes: `OWL_BOX` and `STAR_BOX`, and the 30 / 60 periods from Task 1.
- Produces, in `build/hideout60/notes.md`:
  - the step function(s) (guest address);
  - what each step advances per call (`waitFrame` counter, float phase, motion rate);
  - one of the fix shapes **S1 / S2 / S3** below, per prop;
  - for Task 4: the guest words that hold each prop's drawn state (address or object offset, float or int) and the function that draws it.

Check the leads in this order and stop at the first that explains both props:

- [ ] **Step 1: Script lead (`cmd_waitFrame`)**

Run with `PSPRECOMP_FUNC_CENSUS=1` and take `python build/tools/wcensus.py snap hide60 10` in the hideout.

Expected: `08838434` (cmd_waitFrame) at ≈ N×60/s at 60 fps and N×30/s at 30 fps, which means scripts are waiting in frames.

To tie it to the props, use `PSPRECOMP_FUNC_ARGS=08838434` (a1 = the script context). Then freeze one script context: with the debug socket `W`, hold its wait counter (the word `0x0883BF98` reads; read that function to find the offset). If the owl or the star stops moving, that script drives it.

- [ ] **Step 2: Motion lead (GMO)**

With `PSPRECOMP_FUNC_ARGS=08878A18`, list the players. For each player, read +104 (clock) twice, 0.5 s apart; read also the rate the vtable +40 method returns.

If a player's clock rate per second is 2x at 60 vs 30, that player is a 2x prop: find its model by the motion length. Keep `PSPRECOMP_FUNC_ARGS_MAX` low (≤ 50): logging every call slows the game to ~15 fps and makes a1 read 0x140.

- [ ] **Step 3: Memory lead (fallback if 1 and 2 explain nothing)**

1. Dump the heap 20 times over one 60 fps period (`build/tools/memscan.py`, region 0x09A00000-0x0A000000).
2. Keep the float words that oscillate with the measured period.
3. Run `PSPRECOMP_FUNC_WATCH` on the writer: write-watch the word by finding stores at its offset in `build/p3_output/generated` and `build/ovl/ovAzito_output/generated`.

- [ ] **Step 4: Classify the fix shape**

| Shape | When | Fix in Task 3 |
|---|---|---|
| **S1** | the props' scripts wait with `cmd_waitFrame` (counter −1 per call) | hook `0x08838434`: with the 60fps set, the counter goes down once per two vblanks per script context (`PacedStepTable` keyed by the context, a1; `dt` unused). On a not-due call, return without touching the context. Read `0x0883BF2C` / `0x0883BF98` first: the hook must leave the VM in the same state as a "still waiting" call. |
| **S2** | a per-object update with a1 = frame ticks that moves the prop by a fixed step per call | pace it like the weather: `PacedStepTable`, `weather_step_30hz` pattern |
| **S3** | a float phase or offset advanced by a constant per call, all else time-based | run every frame and halve the constant for the call (pattern `ribbon_step_smooth`): already smooth |

Also record whether the step lives in the main image or the `OL_Azito` overlay. Overlay functions go through the bank dispatch: check `psp_dispatch_register` on an overlay address (grep `psp_dispatch_register` and `bank` in `runtime/src`) before planning a hook there. If it cannot be hooked, hook its main-image caller instead.

- [ ] **Step 5: Commit any new diagnostic**

A reusable diagnostic written for this (for example a `[HIDEOUT]` trace behind `PSPRECOMP_P3_HIDEOUT_TRACE=1`) gets an `ENV_FLAGS.md` entry and its own commit. Throwaway probes stay in `build/tools`.

### Task 3: Pace the step (fix)

**Files:**
- Modify: `psprecomp/games/patapon3/runtime/hooks_main.cpp` (new section after the weather section; registration inside the `psp_patch_set_active("60fps")` block)
- Test: `psprecomp/runtime/tests/test_paced_step.cpp`, only if `PacedStepTable` needs a new behaviour. S1 uses it with `dt` ignored, which needs nothing new.
- Modify: `psprecomp/docs/ENV_FLAGS.md` (`PSPRECOMP_P3_HIDEOUT_30HZ`), `psprecomp/ARCHITECTURE.md` (the 60 fps paragraph: one clause), `psprecomp/docs/superpowers/plans/2026-10-15-60fps.md` ("Known gaps": hideout props)

**Interfaces:**
- Consumes: Task 2's step address(es) and shape.
- Produces:
  - the hook(s);
  - env `PSPRECOMP_P3_HIDEOUT_30HZ` (`0` = off; default on with the 60fps set);
  - the log line `[P3] 60fps: hideout animations paced to 30 Hz`.

- [ ] **Step 1: Same code in the three releases.** Diff each hooked function's body in `build/p3_output`, `build/v115b/p3_output` and `build/vanilla/p3_output`. For an overlay function, diff the bank outputs. Expected: identical. If one differs, register the hook only where it matches and ledger a ruling.

- [ ] **Step 2: RED.** At 60 with `PSPRECOMP_P3_HIDEOUT_30HZ=0`, run `prop_period.py` for the owl and the star. Expected: half the 30 fps periods (Task 1 numbers).

- [ ] **Step 3: Implement** the shape from Task 2's table, with a comment block in the file's style:
  - what runs per frame;
  - the addresses and offsets;
  - why pacing or halving is right.

- [ ] **Step 4: GREEN.** Build, copy to `psprecomp_runtime_wx.exe`, boot at 60 with `PSPRECOMP_FRAMELOG` on and run `prop_period.py`.

  Expected: both periods within 10 % of 30 fps, three runs each. S1/S2 now move at the right speed but only every second frame; Task 4 makes them smooth. S3 already moves every frame, so Task 4 only measures it.

- [ ] **Step 5: Review Focus 1 and 2.**
  - Hideout: open the Cuartel menu and close it; talk to the Hoshipon. Both must open and answer as fast as at 30 fps (screenshots 0.5 s apart; same frame count to the open menu ± 1 shot).
  - Mission: run `wsession.sh build/hideout60/mission` (Campo de los Gigantes, defend drums) for 2 minutes. `wrain.py 3 B` must give 29.7 ± 3 and `wribbon.py 5` 29.8 ± 3. FEVER must still be reached and held.

- [ ] **Step 6: ctest and rt_regress.**
  1. Build everything, then run ctest. Expected: 94/94 (or 94 + new tests).
  2. Run the 30 fps tour `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/hideout_new`.
  3. Compare it against the deployed exe's tour `build/tour/weather_new`: `python -I build/tools/rt_regress.py build/tour/weather_new build/tour/hideout_new`. Expected: PASS.

- [ ] **Step 7: Docs and commit.**

```bash
git add games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md ARCHITECTURE.md docs/superpowers/plans/2026-10-15-60fps.md
git commit -m "fix(p3): hideout animations keep their 30 fps speed at 60 fps"
```

### Task 4: Smooth (move every frame at the 30 fps speed)

Required by the smooth rule. For S3 this task only adds the measurement. For S1 / S2 the props must also move on the frames where the paced step did not run.

**Files:**
- Create: `psprecomp/runtime/include/psp_half_ahead.h` (generic, pure; next to `psp_particle_nudge.h`)
- Create: `psprecomp/runtime/tests/test_half_ahead.cpp`, plus a CMake block next to `test_particle_nudge` in `runtime/CMakeLists.txt`
- Modify: `psprecomp/games/patapon3/runtime/hooks_main.cpp` (the Task 3 section)
- Modify: `psprecomp/docs/ENV_FLAGS.md` (`PSPRECOMP_P3_HIDEOUT_SMOOTH`), `psprecomp/ARCHITECTURE.md` (same 60 fps clause)

**Interfaces:**
- Consumes:
  - Task 3's hook and its `PacedStepTable`; `skipped(obj)` tells a draw that the object's step did not run this frame;
  - Task 2's notes: which guest float words hold each prop's drawn state (position, rotation, scale, and the sprite or motion frame if it is a float).
- Produces:
  - in `psp_half_ahead.h`: `struct HalfAhead`; `void psp_half_ahead_record(HalfAhead* h, const uint8_t* rdram, uint32_t base, const uint32_t* offs, int n)`; `int psp_half_ahead_apply(HalfAhead* h, uint8_t* rdram, float max_jump)`; `void psp_half_ahead_restore(const HalfAhead& h, uint8_t* rdram)`;
  - env `PSPRECOMP_P3_HIDEOUT_SMOOTH` (`0` = plain pacing; default on with `PSPRECOMP_P3_HIDEOUT_30HZ`);
  - the log line `[P3] 60fps: smooth hideout animations`.

`HalfAhead` keeps, per watched word, the values after the last two runs of the step (prev, cur). On a frame whose step was skipped, `apply` writes `cur + (cur - prev) / 2` into guest memory for the draw: half a step ahead, the same idea as the rain. `restore` puts `cur` back bit-exact after the draw. `apply` leaves a word alone if:
- `|cur - prev| > max_jump` (a teleport, a loop restart or a new state is never slid across the screen);
- either value is not finite;
- the word was written by someone else since `record`, i.e. it no longer equals `cur`.

`apply` returns how many words it moved.

- [ ] **Step 1: Write the failing test `test_half_ahead.cpp`** (same `CHECK` style as `test_particle_nudge.cpp`, fake rdram at 0x08000000 + offset):
  - record 10.0 then 12.0, apply → 13.0; restore → 12.0 bit-exact;
  - record 10.0 then 8.0 → apply 7.0;
  - record 0.0 then 300.0 with max_jump 50 → apply leaves 300.0;
  - record 1.0 then NaN → apply leaves it;
  - record 5.0 then 7.0, guest writes 9.0, apply → stays 9.0, and restore does not touch it;
  - one record only (no prev) → apply moves nothing.

- [ ] **Step 2: Run it.** `ninja -C build/rtw test_half_ahead`. Expected: compile FAIL, `psp_half_ahead.h` not found.

- [ ] **Step 3: Implement `psp_half_ahead.h`** with a header comment in the style of `psp_particle_nudge.h`. Up to 16 words per `HalfAhead`; that is enough for a transform.

- [ ] **Step 4: Run it.** Expected: PASS.

- [ ] **Step 5: Wire it into the hooks.**
  - S1 / S2: after each run of the paced step, `record` the prop's words.
  - Hook the prop's draw (the function that reads those words to build its display list; find it with `PSPRECOMP_FUNC_WATCH` on the step's object or the draw-log `D`). On frames where `skipped(obj)`: `apply`, call the draw, `restore`.
  - `max_jump` per word: 4x the largest per-step change measured in Task 2, as a named constant with that origin in a comment.
  - If the drawn state is not a float (an integer sprite-frame index), it cannot be half-stepped. Stop and record it for the hand-off (smooth rule).
  - With `PSPRECOMP_P3_HIDEOUT_TRACE=1` (or the weather trace, if Task 2 added none), print every ~2 s the draws per second and how many of them were half a step ahead (pattern: `weather_count`).

- [ ] **Step 6: Measure.**
  - Speed: `prop_period.py` periods are still within 10 % of 30 fps (smooth on).
  - Smooth: the trace shows draws ≈ 60/s per prop and half a step ahead ≈ 30/s (S1/S2). For S3: steps ≈ 60/s.
  - Four consecutive screenshots of each box: no prop jumps back, slides across or doubles.
  - If speed is off by more than 10 % or a prop slides, set the smooth default to off, ledger it and tell the user. Do not hide it.

- [ ] **Step 7: ctest, docs, commit.** Expected: ctest all pass.

```bash
git add runtime/include/psp_half_ahead.h runtime/tests/test_half_ahead.cpp runtime/CMakeLists.txt games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md ARCHITECTURE.md
git commit -m "feat(p3): hideout animations move smoothly at 60 fps"
```

### Task 5: Deploy and hand-off

**Files:**
- Deploy: the three exes into `build/e2e_portable3/game/<release>/` and `build/e2e_portable/game/<release>/`, after a backup into `build/backup_pre_hideout_<date>/<portable>/<release>/` (same as `build/backup_pre_weather_2026-10-21`). Releases `dxd-1.1.4` (build/rtw), `dxd-1.1.5b` (build/v115b/rtw), `vanilla-eu` (build/vanilla/rtw). Rebuild the variants first: `P3_VARIANT=v115b bash games/patapon3/scripts/p3.sh build`, then the same with `vanilla`.
- Create: `build/autopilot/EN_QUE_NOS_QUEDAMOS_<date>.md` (Spanish)
- Modify: memory `p3-60fps.md` and its `MEMORY.md` line

- [ ] **Step 1: Review Focus 5.** Go from the hideout to a mission and back. Expected: the first seconds in the hideout show no jump of the owl or the star (screenshots 0.2 s apart).

- [ ] **Step 2: Variants boot.** For `v115b` and `vanilla`, run `PSPRECOMP_PATCHES=60fps python -I build/tools/movie_check.py <variant> hide_<variant>`. Expected: `MOVIE OK` and the `[P3] 60fps: hideout animations paced to 30 Hz` line in `build/logs/hide_<variant>.log`.

- [ ] **Step 3: Deploy with backup.** `md5sum` must match between the source exe and both portable copies for each release.

- [ ] **Step 4: Hand-off and memory.** Write the hand-off in Spanish:
  - what was wrong and what changed;
  - periods at 30 / 60 before / 60 after;
  - how to try it: the launcher at 60 fps, look at the obelisk;
  - smooth status per prop (moves every frame, or why not);
  - other hideout props seen at 2x and not fixed, if any;
  - "nada subido; va en push-p3-28 con lo del clima tras tu OK".

## Evidence (fill in during execution)

- Task 1: obelisk view, render scale 1. STAR_BOX 268 122 308 176, OWL_BOX 226 155 254 186
  (silhouette, `DARK=50`; the glow behind it pulses every 5.3 s), owl eyes 233 163 247 174
  (`BRIGHT=170`). Star: 30 fps 4.349 / 4.629 / 4.407 s, 60 fps 2.180 / 2.183 / 2.188 s (2x).
  Owl: bob 1.001-1.005 s and eyes 1.054-1.061 s at both 30 and 60: **not 2x**, dropped from
  scope (no other owl event in 40 s timelines at either rate).
- Task 2: lead 1 alone did not explain the star (freezing the waitFrame counters of the 4
  contexts seen left it moving). Found by correlation (`build/tools/corr_scan.py`: star world x
  1181..1223 in RAM) and a live hardware write watch (new debug socket `H <addr>`, 98cb73b):
  the star's position is a script variable written by cmd add FUN_08837D18 and get-actor-pos
  FUN_08911444, set into the 3D node by setPos FUN_08911588 -> FUN_0887F1E4 -> matrix builder
  FUN_0888E7FC (node pos +192/+196/+200 -> matrix +48/+52/+56), all inside the script VM run
  FUN_0883C0B0 <- FUN_0883C088 <- FUN_08909C7C = script object update (slot +20, a1 = frame
  ticks, return value unused). Shape **S2**, main image. 4 script objects in the hideout
  (120 runs/s at 30, 240 at 60).
- Task 3: FUN_08909C7C and FUN_0888E7FC byte-identical in the three releases. RED (60,
  `PSPRECOMP_P3_HIDEOUT_30HZ=0`) star 2.180 / 2.211 / 2.200 s; GREEN 4.443 / 4.342 / 4.379 s.
  Census at 60: script runs 120.0/s, waitFrame 210.9/s (30 fps: 120.0 / 210.9). Focus 1: the
  Hoshipon's talk bubble and typewriter dialogue show the same text at the same capture times
  at 30 and 60. Focus 2: Campo de los Gigantes at 60: 3 script objects (90.7 runs/s), rain
  29.7 steps/s, wind ribbons 29.8, FEVER held. ctest 94/94. rt_regress: PASS against the
  deployed exe's tour taken the same day (21 gated); against `weather_new` one checkpoint
  (19_mission_prep, 0.54 %) differs only in the help-bar glyphs (the user's controller was
  disconnected; the deployed exe shows the same 0.59 %).
- Task 4: smoothing at the node matrix builder (psp_half_ahead.h): obelisk view 10
  script-moved nodes, 599.4 matrix builds/s, 30.0/s half a step ahead (the star), largest step
  1.30 units. Star with smooth on 4.419 / 4.402 / 4.422 s; consecutive shots: one star, no
  jump. ctest 95/95.
- Task 5: Focus 5: mission -> cancel -> results -> hideout: the star moves on without a jump
  and the counters return to 30/s half a step ahead. v115b and vanilla build and boot at 60
  (MOVIE OK, both hook lines in the log). Deployed to e2e_portable3 and e2e_portable (md5
  dxd-1.1.4 64dc6d9ed295, dxd-1.1.5b 6f0775217d60, vanilla-eu a7a5d91d902c), backup in
  build/backup_pre_hideout_2026-10-21. One of five 60 fps boots stopped on the DxD credits'
  terms dialog (the boot driver's timed keys; the retry landed in 73 s).
