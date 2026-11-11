# Cyclops Stone Throw Distance at 60 fps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** at 60 fps the stone the cyclops throws in "Caza al cíclope (parte II)" travels the same distance as at
30 fps, and moves smoothly.

**Architecture:**
- Two known ways a 60 fps throw goes wrong in this port:
  1. **Launch from the blended pose** (Cannon Hero shot, 24 Oct, 10558c5): logic read bone/part matrices of the
     smooth (in-between) pose, so the launch angle/position differed. The fix kept exact matrices for logic.
  2. **Fixed step per update** (Pyokojinete ghosts, 4 Nov): the object moves a constant per call while its life or
     gravity counts frame time, so at 60 calls/s the path changes. Fix: pace that update to 30 Hz with banked time
     (PacedStepTable) + half-ahead draw for smoothness.
- Decisive A/B first: 60 fps with `PSPRECOMP_P3_ANIM_SMOOTH=0`. Distance right -> cause 1; still wrong -> cause 2
  (or another per-frame step), found by tracing the stone object.
- Measurement = throw distance: landing x minus launch x of the stone, from a 60 Hz window capture (ddagrab), the
  effect probe/trace for the stone's effect names, and the stone object's memory once found.

**Tech Stack:** fx-probe branch (build/wt_fx, from u23), builds build/rtw_fx (1.1.4); lab tools build/tools/gfx_tour.py
(drums with BEAT_GAP), build/rec60/rec.py, build/fx60; Ghidra (ghidra-p3 EBOOT, decomp_title.sh mission overlay).

**Spec:** the user, 10 Nov 2026: "debes arreglar la distancia que recorre la piedra del gigante cuando este la lanza,
a 30 fps se ve bien y recorre bien la distancia, a 60fps no, en la sonda de 30fps tiene que jugar caza al cíclope
parte 2 y solo avanza hasta que te ataque el cíclope, en 60fps igual, es solo atacar hasta que te empiece a atacar y
así observes sus funciones". Heroes prepared by the user: their save copy (build/plata/savedata_sonda, backed up as
build/save_ciclope at 07:17 data).

## Global Constraints
- Lab only on ports 9997/9998, never 9999 or build/rtw; saves are copies (TOUR_SAVE dirs copied per launch).
- Fix must keep 30 fps unchanged and be smooth at 60. Commits as Patapon Community on fx-probe; no push.
- Ask nothing mid-run; rulings go to build/autopilot/LEDGER_cyclops_2026-11-10.md.

## Review Focus
1. Fixing the throw must not change the cyclops' other attacks or other giants' throws at 30 fps.
2. A pacing fix must not make the stone step at 30 Hz (smooth rule).
3. Logic/visual split (cause 1) must not desync online play (both peers run the same logic).

---

### Task 1: Route + reproduction
- [ ] From the hideout with build/save_ciclope: Obelisco -> world map -> the area with "Caza al cíclope (parte II)"
  -> deploy; script build/fx60/cy_go.sh RUN FPS [ENV...] (two.py A or gfx_tour, port 9997/9998), march until the
  cyclops attacks (PATAPATA, then PONPON when in range), record 40 s (rec.py) at 30 and 60.
- [ ] Measure each throw: launch x, landing x (frame of FF_ROCK_IMPACT / the stone's last position), distance in
  screen px at the same camera scale. Expected: 60 != 30 (the user's report).

### Task 2: A/B the cause
- [ ] 60 fps with PSPRECOMP_P3_ANIM_SMOOTH=0. Same measurement. Equal to 30 -> cause 1 (Task 3a); else Task 3b.

### Task 3a (blended pose)
- [ ] Find which bone/part the throw reads (FUNC_ARGS on bone_smooth / attach paths during the throw, host backtrace
  like the cannon), make that read exact (logic) while the draw stays smooth. Test with the existing smooth-anim tests.

### Task 3b (per-update step)
- [ ] Find the stone's update (function census diff around a throw, write-watch on its position), confirm a fixed
  step per call, pace it to 30 Hz with banked dt + half-ahead draw (pattern: effect_update_30hz / attach_smooth).
  RED test for the pacing selector, GREEN, ctest.

### Task 4: Verify + hand-off
- [ ] 30 / 60 / 60-fixed distances within 5 % of 30; stone moves every frame at 60 (frame diff); ctest 137/137;
  rt_regress 30 fps unaffected (no 30 fps code path changed).
- [ ] build/plata/probar_ciclope_antes.bat / _arreglado.bat (save copy) + QUE_PROBAR_CICLOPE.md; ledger; memory.
