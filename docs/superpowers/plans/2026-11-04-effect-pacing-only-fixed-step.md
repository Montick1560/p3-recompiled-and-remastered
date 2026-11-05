# Effect Pacing Only for Fixed-Step Actors (60 fps) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** at 60 fps, arrows and javelins are smooth again (as before 9d7aa83), while Pyokojinete's ghost riders keep
their 30 fps speed and Wooyari's fire wheel keeps not flickering.

**Architecture:**
- 9d7aa83 paces every mission effect actor updated by `FUN_088D3AB4` (vtable +60, a0 = actor, a1 = dt) to 30 Hz
  with the skipped call's dt banked (`effect_update_30hz`, `PacedStepTable g_effect_paced`).
- Its smoothing (6900c01) draws half a step ahead only the parts placed through `attach_smooth`. Projectiles are not
  drawn that way, so since 9d7aa83 they update and show at 30 Hz.
- Without pacing they were smooth and at normal speed (the user, 4 Nov: "iban a velocidad normal").
- The fix:
  1. Pace only the actor types that need it: the ghost riders (2× speed unpaced, measured) and Wooyari's wheel
     (1-frame gap at its restart unpaced).
  2. Every other actor runs every vblank again, exactly as before 9d7aa83.
- The discriminator comes from recon (Task 1). The decompile of the subtree reads a per-actor descriptor at
  `a0+0x344` (`FUN_088D4ABC`, `FUN_088D6604`; `build/pyoko/d3ab4*.c`). The key must be stable across boots and
  releases: a word inside the descriptor, or the vtable, not a heap address.

**Tech Stack:** C++20 `games/patapon3/runtime/hooks_main.cpp`; lab: `build/pyoko/fixtest.sh` (1.1.5b, Pyokojinete dash
measurement), `build/wooyari/wy_run.sh` (1.1.4, Wooyari hero mode, `fire.py`); debug socket (`R`); ninja + ctest.

**Spec:** the user, 4 Nov 2026:
- "con el arreglo, efectivamente lo arreglas, pero los proyectiles como jabalinas y flechas pierden su suavidad y se
  ven a 30fps, pero en el .bat probar_pyoko_antes ... si tienen esa suavidad";
- "iban a velocidad normal, haz el plan opción 1 y dale con todo".

## Global Constraints

- **Lab.** Ports 9998/9996 only, never 9999. Run diskquiet before boots; no leftover processes; RTC fixed
  `2026-10-22T08:48:00`.
- **Hooks.** 60fps set only, guard `psp_code_matches` (unchanged). `PSPRECOMP_P3_EFFECT_30HZ=0` still turns all
  pacing off. New trace env `PSPRECOMP_P3_EFFECT_TRACE=1` (lab, documented in `docs/ENV_FLAGS.md`).
- **Releases.** The key must hold in 1.1.4, 1.1.5b and vanilla. Check per release in Task 3, or fall back to
  pacing off for an unknown release (never pace everything blindly).
- **Git.** New branch `effect-select` from `pyoko-60` (9180ef8). The `march-lag` lab commits stay out. Author
  "Patapon Community" `<patapon-community@users.noreply.github.com>`, no trailers, never push. `hooks_main.cpp`
  has CRLF line endings.
- **Budget.** 45 min or 3 attempts per step. Ledger `build/autopilot/LEDGER_effect_select_2026-11-04.md`.
- **Workflow.** Native. Spanish with the user; code and commits in English.

## Review Focus

1. **Ghost riders** still 586 ± 15 units in ~1.67 s (`fixtest.sh`), and smooth (the 6900c01 ahead draws still
   happen).
2. **Wooyari's wheel:** no gaps (`wy_run.sh` + `fire.py`) and a video for the user.
3. **Arrows and javelins** update every vblank: their actors are called once per vblank (trace).
4. **An actor type not in the list runs unpaced,** like before 9d7aa83. Pyokojinete's other effects and the cannon
   shot look as they did then.
5. **Another release** where the key does not match: nothing is paced, and one log line says so.

---

### Task 1: Recon — which word tells the types apart

No commit of the trace yet (it is committed with Task 2 if kept).

**Files:**
- Modify (lab): `games/patapon3/runtime/hooks_main.cpp` `effect_update_30hz`. With `PSPRECOMP_P3_EFFECT_TRACE=1`,
  the first call of each new actor logs one line `[P3-EFX] new a0=… vb=… vt=*(a0) d=*(a0+0x344) d0..d7=…` (the
  first 8 descriptor words). Every 60 vblanks it also logs the per-actor call counts of the last second.

**Interfaces:**
- Produces, for Task 2:
  - the key (a field and the values for the ghost riders and the wheel);
  - proof that the arrow and javelin actors have other values;
  - each value per release.

- [ ] **Step 1:** Build `build/v115b/rtw`, then run `fixtest.sh` with `PSPRECOMP_P3_EFFECT_TRACE=1`.
  Expected: the new actors that appear at the dash (the ghosts, ~28) share one descriptor pattern.
- [ ] **Step 2:** Build `build/rtw` (1.1.4), then run `wy_run.sh wytrace PSPRECOMP_P3_EFFECT_TRACE=1`.
  Expected: wheel actors at hero mode, and arrow/javelin actors at each attack; their keys differ from the ghosts'.
- [ ] **Step 3:** Pick the most specific stable word that separates the groups.
  - Prefer, in order: a descriptor id or name word; the vtable; a code pointer in the descriptor.
  - Ledger it with the values seen in both releases.

### Task 2: Pace only the listed types

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp`:
  - `effect_update_30hz`: if the actor's key is not in `kPacedEffectKeys`, call the original every vblank;
  - its comment says which types are paced and why (numbers: ghosts 1197 → 586 u; wheel gap);
  - keep the trace env.
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_P3_EFFECT_30HZ` text, `PSPRECOMP_P3_EFFECT_TRACE`); `ARCHITECTURE.md`
  60 fps paragraph if it names "mission effect actors".
- Test: only if the key test becomes a pure helper (for example, `bool effect_paced_type(uint32_t key)`). Then
  `runtime/tests` gets a table test: the ghost and wheel values → true; arrow, javelin and 0 → false. Write it
  first, watch it fail.

**Interfaces:**
- Consumes: Task 1's key and values.

- [ ] **Step 1:** If a helper is written, its failing test first. Run ctest and watch it fail.
- [ ] **Step 2:** Implement.
- [ ] **Step 3:** `ninja && ctest --output-on-failure` in `build/v115b/rtw` and `build/rtw`.
  Expected: all pass.
- [ ] **Step 4:** Lab checks with the trace on.
  - `fixtest.sh` (1.1.5b): the ghosts paced (one call per 2 vblanks); 586 ± 15 u in ~1.67 s.
  - `wy_run.sh` (1.1.4): wheel paced; arrow/javelin actors once per vblank; `fire.py` 0 gaps.
- [ ] **Step 5:** Commit "fix(p3-60fps): pace only ghost riders and Wooyari's wheel; arrows and javelins smooth again".

### Task 3: Releases, gate, user kit

- [ ] **Step 1:** Rebuild `build/vanilla/rtw`; ctest in all three trees. Ledger whether the key matches per release
  (the trace, or a run where the key is read).
- [ ] **Step 2:** 30 fps tour against `build/tour/mask_new`.
  Expected: PASS, or only `19_mission_prep` 0.54 %.
- [ ] **Step 3:** Video "ANTES / ARREGLADO" of arrows and javelins at 60 fps (Wooyari run: a slow-motion crop
  around a volley). Send it with SendUserFile.
- [ ] **Step 4:** `.bat` files for the user:
  - the existing `build/pyoko/probar_pyoko_{arreglado,antes}.bat` now run the new exe;
  - a new `build/wooyari/probar_wooyari_proyectiles.bat` (1.1.4, copy of `save_wooyari`, port 9996);
  - `QUE_PROBAR.md` updated.
- [ ] **Step 5:** CHANGELOG u17 line; memory (`p3-pyoko-hero`, `p3-wooyari-hm`); hand-off; report in Spanish.
