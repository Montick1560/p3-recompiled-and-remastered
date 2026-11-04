# Pyokojinete Ghost Dash Half Step at 60 fps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** at 60 fps (`PSPRECOMP_PATCHES=60fps`), Pyokojinete's hero-mode ghost riders travel at their 30 fps speed
over their 30 fps distance, moving on every vblank.

**Architecture:**
- The previous plan's lab work (`2026-11-03-60fps-pyoko-hero.md`, ledger `build/autopilot/LEDGER_pyoko_2026-11-03.md`)
  proved the cause:
  - the ghost objects' per-object method `FUN_088D3AB4` advances the dash by a fixed step per call;
  - at 60 fps it is called on every vblank (101 calls over 1.68 s, part x 274 → 1471, +11.6 per vblank);
  - at 30 fps it is called ~50 times (226 → 810 in 1.71 s);
  - pacing it to 30 Hz gave the right speed (355/s), but 101 calls over 3.4 s: the dash's life counts calls and is
    already doubled at 60.
- So the fix keeps every call and **halves the per-call step**. That gives the same life in vblanks, the 30 fps
  distance, and motion on every vblank. It is the `ribbon_step_smooth` pattern, so smoothness comes for free.
- Task 1 finds where that step lives inside `FUN_088D3AB4`'s subtree (decompile, then confirm with a lab probe).
  Task 2 hooks it in the game module.

**Tech Stack:** C++20 game module `games/patapon3/runtime/hooks_main.cpp`; ninja + ctest in `build/v115b/rtw`
(then `build/rtw`, `build/vanilla/rtw`); Ghidra headless (`build/tools/ghidra/decomp.sh`); lab scripts in
`build/pyoko/` (`hero115.sh`, `go_dragon.sh`, `labtest.sh`, `multiread.py`, `steps.py`).

**Spec:**
- The user, 30 Oct: "arreglaremos el modo héroe de pyokojinete, debido a los 60 fps, su modo héroe va rapidísimo";
  "requiero que arregles la velocidad y agregues suavidad para los 60 fps".
- 4 Nov: "Dale con todo, ármate un plan y ejecútalo, nativo"; "Cuando termines, grábame un video a 60fps".

## Global Constraints

- **Targets** (lab, 1.1.5b, Dragón en hibernación, first hero activation of a fresh boot), measured with
  `steps.py` on part x `0x09A8D4E0`:
  - 60 fps fixed: dash ≈ 584 units (±10 %) in ≈ 1.7 s, i.e. ≈ 342 units/s;
  - about 100 changes per dash, so it moves on every vblank, with steps ≈ 5.8 (half of 11.6).
- **Hooks.**
  - Active only with the 60fps set.
  - Every hooked address is guarded by `psp_code_matches`, as `kMarchStepCode` is.
  - A/B switch: `PSPRECOMP_P3_GHOST_HALF=0`.
  - Log line: `[P3] 60fps: hero ghost riders step half per vblank`.
- **Scope.** Only the hero ghost objects: `FUN_088D3AB4` is also the method of other effect objects (~120 calls/s
  outside hero mode). Their behavior must not change; Task 1 finds the discriminator.
- **Lab.** Port 9998 only. One measurement per fresh boot (later activations use other pool slots). Run
  `diskquiet` before each boot (it is in `hero115.sh`).
- **Git.** Branch `pyoko-60` (c004eb4).
  - Author "Patapon Community", no Co-Authored-By trailer. Never push.
  - The `PSPRECOMP_P3_LAB_PACE` hook from `build/pyoko/wip_actor_scripts_and_lab_pace.patch` may be applied for
    probing, but is never committed.
- **Budget.** 45 min or 3 attempts per step. Ledger every ruling in `build/autopilot/LEDGER_pyoko_2026-11-03.md`.

## Review Focus

1. **Other effect objects through `FUN_088D3AB4`.**
   - Re-measure one normal Edson charge (non-hero, ~900–1000 px/s in `k30a`) with the fix on and off; it must match.
   - Take a mission screenshot sanity check: the stage, the army and the drum HUD are present.
2. **Hits per dash.** Count the damage numbers per dash at 30 fps and at fixed 60 fps on the dragon; they must be
   equal (±1).
3. **30 fps play** (no 60fps set): no hook registered. The 30 fps tour gate passes.
4. **Releases.** 1.1.4 and vanilla may have other code at the address; the guard turns the hook off there. Report
   per release.
5. **Pause during a dash.** The game pauses (no calls), so nothing is halved twice; check one pause in the lab.

---

### Task 1: Locate the per-call step

No commit; findings go in the ledger.

**Files:**
- Create: `build/pyoko/d3ab4.c` (decompile of `FUN_088D3AB4` and its callees, from
  `bash build/tools/ghidra/decomp.sh build/pyoko/d3ab4.c 088D3AB4 088D4ABC 088D6604 088D674C 088D4724`)
- Reuse: `build/pyoko/wip_actor_scripts_and_lab_pace.patch` (`git apply`, lab only) and `labtest.sh`.

**Interfaces:**
- Produces, for Task 2:
  - the guest function and the object word(s) that hold the per-call step: velocity or step value, its offset, and
    the object (a0 of what);
  - the discriminator for hero ghost objects (vtable word, type id or flag), checked on 2 non-ghost objects;
  - the code words for the guard.

- [ ] **Step 1:** Decompile and read: find `pos += v` / `v += a` / `pos = f(counter)` on the object.
  Expected: a candidate store per call into the object chain of `a0`.
- [ ] **Step 2:** Lab probe: apply the WIP patch, and with `PSPRECOMP_P3_LAB_PACE` bisect `FUN_088D3AB4`'s callees
  (`labtest.sh`), to find the narrowest function whose pacing still gives ≈ 355/s.
  Expected: one callee or the object word named in Step 1.
- [ ] **Step 3:** Read the discriminator on a ghost object and on 2 other objects passed to `FUN_088D3AB4`
  (`FUNC_ARGS` re-armed with `A` in and out of hero mode).
  Expected: a word that differs.

### Task 2: Halve the step

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp`: a `ghost_step_half` wrapper of the Task 1 function, registered
  in the 60fps block with its guard, the env switch and the log line. Its comment cites the measurements above.
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_P3_GHOST_HALF`) and the 60 fps section of `ARCHITECTURE.md`.
- Test: a pure helper with a unit test in `runtime/tests/`, only if the halving needs logic beyond one line (for
  example, a velocity integrated per call that must be scaled and restored). Otherwise no new test; ctest must stay
  green.

**Interfaces:**
- Consumes: Task 1's function, offsets, discriminator and code words.

- [ ] **Step 1:** Implement, with the A/B switch and the guard.
- [ ] **Step 2:** `ninja && ctest --output-on-failure` in `build/v115b/rtw`.
  Expected: all pass.
- [ ] **Step 3:** `labtest.sh` style run at 60 fps with the fix, then at 60 fps with `PSPRECOMP_P3_GHOST_HALF=0`.
  Expected:
  - fixed: ≈ 584 units, ≈ 342 units/s, ~100 changes;
  - off: 1197 units, 711 units/s.
- [ ] **Step 3b:** Review Focus 1 (normal charge plus screenshot) and 2 (hits per dash).
  Expected: unchanged, and equal hits.
- [ ] **Step 4:** Commit: "fix(p3-60fps): Pyokojinete's hero ghost riders keep their 30 fps speed".

### Task 3: Releases, gate, video, hand-off

**Files:**
- Create:
  - `build/pyoko/probar_pyoko.bat` (the user's folder setup copied to `build/pyoko/prueba`, with the 60fps set);
  - `build/pyoko/QUE_PROBAR.md`, in Spanish.
- Create: the video `build/pyoko/pyoko60_fix.mp4`. It is a 60 fps capture of the fixed dash, recorded with
  `build/rec60/rec_win.py`, with the window at scale 2 so it is readable. A second clip with the fix off goes next
  to it.
- Modify:
  - `build/beta/CHANGELOG.md` "Actualización 17 (en preparación)";
  - memory `p3-pyoko-hero.md` and `MEMORY.md`;
  - ledger and `build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-11-04.md`.

- [ ] **Step 1:** Rebuild `build/rtw` and `build/vanilla/rtw`, then boot each briefly and grep the log line. Report
  whether the guard matched.
- [ ] **Step 2:** 30 fps gate: `TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out
  C:/Users/torso/Documents/deco/build/tour/pyoko60`, then `rt_regress.py` against a same-hour base.
  Expected: `RT_REGRESS PASS`.
- [ ] **Step 3:** Record both videos, publish them to the user (artifact or file), and report the before/after
  numbers.
