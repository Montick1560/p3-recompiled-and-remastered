# Pyokojinete Hero Mode at 60 fps (second run) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** at 60 fps (`PSPRECOMP_PATCHES=60fps`), Pyokojinete's hero-mode dash crosses the field at its 30 fps speed
and moves on every vblank (no stalls, no double jumps).

**Architecture:**
- The dash moves a fixed step per game update. The user's videos (`build/pyoko/ANALISIS.md`) show ~1240 px/s at
  30 fps and ~2480 px/s at 60 fps on a 1920-px frame: exactly 2x, with uneven 0/88 px frames.
- This is the same class of bug as the rain and tailwind (`b12176b` speed + `f9c7772` smooth, code
  `weather_step_30hz` / `rain_draw_smooth` in `games/patapon3/runtime/hooks_main.cpp`). The fix follows it:
  1. find the dash's per-update step;
  2. pace that step to 30 Hz with `PacedStepTable` (`runtime/include/psp_paced_step.h`);
  3. draw it half a step ahead on the skipped vblanks (`psp_particle_nudge.h`, or `smooth_scope_run` if the dash
     is a model).
- **What changed since the first run** (ledger `.superpowers/sdd/2026-10-30-60fps-pyoko-hero/progress.md`): Task 2
  was blocked because the lab mission (El dragón oscuro Libera) had the army at a wall, so the dash barely traveled.
  - Leads kept: 09A9F620 (+0x90 steps, also 09B6BD88, 09C04EA0) ramped linearly ~11.5/vblank from 402 to 1598 once.
  - Hero-only calls seen: 088FDE7C/088FDFE8 (keyframe lerp), 0890093C, ctor 08B7CDE0.
  - Ruled out: the ribbon at 09FBDA70 (writer FUN_088FE738) and the rain pairs at 0x09C10xxx.
- Tonight the mission is **Dragón en hibernación**, an open field, played with the user's own save.

**Tech Stack:** C++20 game module, ninja + ctest (`build/v115b/rtw` first: the save is 1.1.5b; then `build/rtw`,
`build/vanilla/rtw`). Python lab tools in `build/tools/`, `build/infra/`, `build/pyoko/`. Debug socket on port 9998.
ffmpeg ddagrab for 60 fps captures (memory `p3-video-recording`).

**Spec:** the user's words.
- 30 Oct: "arreglaremos el modo héroe de pyokojinete, debido a los 60 fps, su modo héroe va rapidísimo";
  "requiero que arregles la velocidad y agregues suavidad para los 60 fps".
- 3 Nov:
  - "ya te dejé una save lista, usa la que usa mi launcher personal C:\Users\torso\Videos\patapon3-beta2\Patapon3Recomp";
  - "puedes jugar la misión Dragón en hibernación";
  - "E:\Pyoko60fps.mp4 E:\Pyoko30fps.mp4 aquí están las comparaciones, se hicieron en niveles diferentes";
  - "trabajarás toda la noche, escribe el plan y ejecútalo".
- Hero mode: PON PON CHAKA CHAKA (`OOCC----`) with perfect timing until hero mode starts, then PON PON PATA PON
  (`OOPO----`) forever.

## Global Constraints

- **The save.**
  - Copy `C:/Users/torso/Videos/patapon3-beta2/Patapon3Recomp/versions/dxd-1.1.5b/savedata` to
    `build/pyoko/save115b` and use only the copy: `TOUR_SAVE=C:/Users/torso/Documents/deco/build/pyoko/save115b`.
  - Never write into the user's folder. Before anything that sends keys, check that no launcher or game from that
    folder is running (memory `no-keys-into-open-windows`).
- **Release.** The lab runs dxd-1.1.5b:
  `P3DRIVE_EXE=../v115b/rtw/psprecomp_runtime.exe P3DRIVE_DISC0=C:/Users/torso/Documents/deco/build/v115b/disc0`.
- **Targets**, from the user's 30 fps video:
  - dash front ~1240 px/s on a 1920-px frame, i.e. ~10.5 PSP units per 30 fps update;
  - at 60 fps, each vblank moves about half that (~5 units). No 0 or double steps inside a dash;
  - the hits per dash are the same as at 30 fps.
- **Hooks** are active only with the 60fps set, and every hooked address is guarded by `psp_code_matches`, as
  `kMarchStepCode` is.
  - A/B switches: `PSPRECOMP_P3_HERO_30HZ=0` turns off the pacing; `PSPRECOMP_P3_HERO_SMOOTH=0` keeps the pacing
    and turns off the smoothing. Both are on by default with the 60fps set.
- **Lab.** Port 9998 only, never 9999. Run `python -I build/infra/diskquiet.py 60 900` before each boot. Never use
  the debug_menu patch set in builds.
- **Git.** Branch `pyoko-60` from `borderless-window` (c004eb4 = the u16 source, which holds every local fix).
  - Commits: author "Patapon Community", no Co-Authored-By trailer. Never push.
  - Edit C++ with the editor tool.
- **Budget.** 45 min or 3 attempts per step. Then write what blocked it in the ledger
  `build/autopilot/LEDGER_pyoko_2026-11-03.md` and go to the next lead; stop the plan only when the leads run out.

## Review Focus

1. **Shared step.** If the dash step is a generic function, normal attacks and other heroes run it too. Re-measure
   one normal attack and the cannon Hero shot (`10558c5`) with the change on and off; anything already right must
   not get slower.
2. **Hits per dash** must equal the 30 fps count. The pacing must not drop or double the hit checks.
3. **30 fps play** (no 60fps set): the hooks are not registered, and the 30 fps tour gate passes.
4. **Pause or mission end in the middle of a dash:** the dash resumes or ends cleanly (skipped time is banked in
   `psp_paced_step.h`).
5. **Other releases.** The function may sit at another address in 1.1.4 or vanilla. The code-match guard turns the
   hook off there. Report per release whether it is active, and never patch a mismatched address.

---

### Task 1: Lab route and baseline in Dragón en hibernación

Outside the repo; no commit. Evidence goes in the ledger and in `build/pyoko/`.

**Files:**
- Create: `build/pyoko/hero115.sh RUN [60]`: like `build/pyoko/hero.sh`, but with the 1.1.5b exe and disc0, the
  `save115b` copy, and the route to Dragón en hibernación (`build/pyoko/go_dragon.sh`).
- Create: `build/pyoko/go_dragon.sh RUN`: menu keys from the hideout landing to that mission. Found by screenshots
  with `build/infra/shot.sh`.
- Reuse: `build/pyoko/kymo.py`, `build/tools/rec60` / `rec_win.py` (60 fps window capture).

**Interfaces:**
- Produces:
  - the boot + route command;
  - the drum script that reaches and keeps hero mode;
  - the baselines at 30 and at 60: dash speed (px/s scaled to 1920), per-frame step list, hits per dash.

- [ ] **Step 1:** Copy the save. Boot 30 fps, find the route by screenshots, and write `go_dragon.sh`.
  Expected: a screenshot in the mission with the army on the field.
- [ ] **Step 2:** Drum into hero mode (`./go2.sh RUN "B D <seq> 1"`), capture 20 s at 60 fps of video, and run
  `kymo.py`. Do it at 30 and at 60.
  Expected: the 60 fps slope ≈ 2x the 30 fps slope; record both in the ledger.

### Task 2: Find the dash's per-update step

No commit; the lab notes go to the ledger.

**Files:**
- Create: `build/pyoko/pinkscan.py PORT DUMPS OUT.txt [lo hi]`. Same as `build/tools/corr_scan.py`, but the
  tracked centroid is the pink trail's x (the pink test of `kymo.py`) in the ground band, not bright pixels.
- Reuse: `dashscan.py`, `fastread.py`; `PSPRECOMP_HW_WATCH=<addr>` (`runtime/src/psp_backtrace.cpp`) for the
  writer's backtrace; `PSPRECOMP_STORE_WATCH=<vtable>` to catch every spawn of the object's class.

**Interfaces:**
- Produces:
  - `kHeroStep` (guest address of the per-update step), its first code words for the guard, and its argument
    (object in a0?);
  - the position word offsets in the object, and whether it keeps a previous position or a velocity;
  - whether the step runs once per vblank at 60 (call count per second at 30 vs 60).

- [ ] **Step 1:** In the open field, `pinkscan.py` during a dash, and `dashscan.py` around the 09A9F620 lead.
  Expected: float words that follow the trail's x (|corr| > 0.9), stepping ~2x per vblank at 60.
- [ ] **Step 2:** Read the object around the best word: header words, vtable, and the same offsets at the other
  0x90-strided copies.
  Expected: a struct layout (x, previous x or velocity, life/timer).
- [ ] **Step 3:** Boot with `PSPRECOMP_HW_WATCH=<x word>` and resolve the backtrace to guest functions
  (`build/tools/bt.py`, as for the cannon).
  Expected: the guest function that adds the step. Confirm by call counts per second (30 vs 60 = 1:2).

### Task 3: Pace the dash step to 30 Hz

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp`: a `hero_step_30hz` wrapper registered like
  `weather_step_30hz` (60fps set + `psp_code_matches`), plus a log line
  `[P3] 60fps: Pyokojinete hero dash paced to 30 Hz`.
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_P3_HERO_30HZ`) and the 60 fps paragraph of `ARCHITECTURE.md`.
- Test: no new unit test if `PacedStepTable` is used as is (`runtime/tests/test_paced_step.cpp` covers it).

**Interfaces:**
- Consumes: Task 2's `kHeroStep`, its code words, and the object argument.
- Produces: `g_hero_paced` (a `PacedStepTable` keyed by the object) and `skipped(obj)` for Task 4.

- [ ] **Step 1:** Implement and register the wrapper.
- [ ] **Step 2:** `ninja && ctest --output-on-failure` in `build/v115b/rtw`.
  Expected: all pass.
- [ ] **Step 3:** Task 1 route at 60.
  Expected: slope within 5 % of the 30 fps one; hits per dash equal to 30 fps; the `[P3]` line in the log.
- [ ] **Step 4:** Commit "fix(p3-60fps): Pyokojinete's hero dash keeps its 30 fps speed".

### Task 4: Smooth the dash at 60 fps

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp`: `hero_draw_smooth` wraps the dash's draw.
  - On a vblank whose step was skipped, it nudges the position half a step ahead, draws, and restores
    (`psp_particle_nudge` / `psp_particle_restore`).
  - A jump larger than one 30 fps step (spawn, end of the dash) is drawn as is.
  - If the dash is a model drawn through the motion player, use `smooth_scope_run` (as for the FEVER worm) instead,
    and note the choice in the ledger.
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_P3_HERO_SMOOTH`).

**Interfaces:**
- Consumes: Task 2's layout and Task 3's `skipped(obj)`.

- [ ] **Step 1:** Implement and register it (60fps set only).
- [ ] **Step 2:** `ninja && ctest --output-on-failure`.
  Expected: all pass.
- [ ] **Step 3:** Task 1 route at 60 with a 60 fps capture.
  Expected:
  - ≈1240 px/s;
  - per-frame steps of ~21 px at 1080p, none 0 or ≥ 60 inside a dash;
  - hits per dash unchanged.
- [ ] **Step 4:** Commit "feat(p3-60fps): Pyokojinete's hero dash moves smoothly at 60 fps".

### Task 5: Checks, other releases, user test

**Files:**
- Create:
  - `build/pyoko/probar_pyoko.bat`: a copy of the user's save folder setup in a separate folder, with the 60fps set,
    plus one variant with `PSPRECOMP_P3_HERO_30HZ=0`;
  - `build/pyoko/QUE_PROBAR.md`, in Spanish: what to look at.
- Modify: `build/beta/CHANGELOG.md` "Actualización 17 (en preparación)".
- Modify memories: `p3-pyoko-hero.md`, and `MEMORY.md`.

- [ ] **Step 1:** Review Focus 1 (one normal attack, on/off) and 4 (pause in the middle of a dash, then resume).
- [ ] **Step 2:** Rebuild `build/rtw` and `build/vanilla/rtw` (`ninja && ctest`). Check whether the guard matched in
  each from the `[P3]` line of a short boot.
- [ ] **Step 3:** 30 fps gate.
  - Run `TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out
    C:/Users/torso/Documents/deco/build/tour/pyoko60`.
  - Then `python -I build/tools/rt_regress.py <same-hour base> build/tour/pyoko60`. The base must be shot at the
    same time of day, because the hideout sky follows the clock.
  - Expected: `RT_REGRESS PASS`.
- [ ] **Step 4:** Write the hand-off `build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-11-04.md`. Wait for the user's play
  test before any merge, packaging or push.
