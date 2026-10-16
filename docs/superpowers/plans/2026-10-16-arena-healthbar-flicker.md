# Arena del Valor: flickering health bars — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended: one
> diagnosis task decides the branch, then one small fix) or superpowers:subagent-driven-development
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written 2026-10-16 (Opus 5.5) for the next session. Nothing implemented yet.

**Goal:** the health bars over units (allies and enemies) in the Arena del Valor stop flickering
and stay on screen every frame, as on PPSSPP.

**Architecture:** diagnosis first (Task 1), because the cause is not proven. Task 1 reproduces the
flicker on a test instance, finds the PRIMs that draw the bars and tells which branch applies. The
leading suspect, found while writing this plan, is face culling: the runtime enables `GL_CULL_FACE`
for every PRIM (`runtime/src/psp_ge_draw.cpp:1076`), but PPSSPP never culls `GE_PRIM_RECTANGLES`
(sprites), clear-mode draws, points or lines
(`DECO/tools/ppsspp-src/GPU/GLES/StateMappingGLES.cpp:215`,
`cullEnable = !gstate.isModeClear() && prim != GE_PRIM_RECTANGLES && prim > GE_PRIM_LINE_STRIP && gstate.isCullEnabled();`,
same in the Vulkan and D3D11 backends). A sprite expanded to two triangles gets its winding from the
order of its two corners, so a bar whose corners come in a different order on some frames would be
culled on those frames and drawn on the others. Task 2 is that fix (TDD); the "If Task 1 rules
out culling" section lists the next checks if the evidence says otherwise.

**Tech Stack:** C++17 runtime (OpenGL 3.3), CTest unit tests, Python tools in `DECO/build/tools`
(`gfx_tour.py`, `tstep.py`, `rt_regress.py`, `gelist.py`), PPSSPP 1.20.4 as the oracle.

**Spec:** user, 2026-10-16: *"vamos a arreglar un bug en la arena del valor, las health bars de los
personajes (amigos o enemigos) titilan, escribe el plan para empezarlo en una nueva sesion"*.
Answers given the same day: it flickers with **both** `jugar.bat` (30 fps) and `jugar_60fps.bat`
(so it is not the 60 fps frame blend); the arena is reached **from the master obelisk**; the bars
**appear and disappear** (visible on one frame, gone on another), not a colour/size change. No
separate spec file.

`DECO` = `C:/Users/torso/Documents/deco`. Repo = `DECO/psprecomp`, branch `patapon3-windows`.

## State when this plan was written

- `patapon3-windows` = `308fb56` (fog cleanup), local only for the icon commit `f639180`; `p3/main` =
  `0f84950` (same content minus the icon). `ctest --test-dir DECO/build/rtw`: 70/70.
- Patapon 3 does **not** install the degenerate-matrix NDC fallback (`ge_vertex_set_degenerate_fallback`
  is only called from `games/patapon/runtime/hooks_ge.cpp`), so the "composed MVP collapses prim"
  path is not a suspect for P3.
- The `[DL]` PRIM line (`drawlog_prim`, `runtime/src/psp_ge_draw.cpp:~531`) has no cull fields yet;
  Task 1 adds them.
- No tour reaches the arena yet. `build/tools/gfx_tour.json` goes hideout → `hideout_obelisco`
  (`R`) → `worldmap` (`X`, settle 4); the arena route from the world map is unknown and Task 1
  records it.
- Known tool quirks: `gelist.py rt` reads port 9999 (hard-coded), so on a test instance use the
  tour's serve `{"gelist": 1}` job, which reads through the tour's own socket. `dlseq.py` and
  `dlbbox.py` look for `[DL] prim=` but the lines start with `[DL] i=`: they find nothing until
  fixed (outside the repo; fix them in Task 1 if you need them, see Step 5).

## Global Constraints

- Test instances only on **port 9998** (`TOUR_RT_PORT=9998`). Never touch the user's game (port 9999)
  or `DECO/savedata`; use copies (`build/save_arena`, created in Task 1).
- Always `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00` for tours and `rt_regress`.
- Never set `PSPRECOMP_NO_AUDIO=1`. Nothing destructive on the PC, no installs, no system security
  changes (never `CMAKE_TLS_VERIFY=0`). Ask the user before closing any PPSSPP or runtime they
  opened (check the port and start time first; kill only your own instances, by PID).
- `gfx_tour.py` with both sides kills every `PPSSPPWindows64.exe`: before a two-sided run, check that
  the user has no PPSSPP open (`tasklist | grep -i ppsspp`); if they do, ask.
- Never edit `build/p3_output/generated/*.cpp`; no game-specific code in `runtime/src` (the cull rule
  is PPSSPP's generic GE behaviour, so it belongs in `runtime/`).
- The reference is PPSSPP in `DECO/tools/ppsspp-src/`. Do not invent GE behaviour.
- Commits: author "Patapon Community" (the repo's configured identity; do not touch git config),
  **no `Co-Authored-By`**, English, one commit per task, ending with `Verified: ...` and `Agent: ...`
  lines. Talk to the user in Spanish.
- **No push** until the user says so. **Never push `f639180`** (local exe icon).
- Line endings: `runtime/` sources and `DEBUGGING.md` are CRLF in the working tree (index LF,
  `core.autocrlf=true`). Use the Edit tool; `sed -i` from Git Bash rewrites the file as LF.
- Build: `cmake --build DECO/build/rtw` builds the user's play exe and the unit tests. Before
  rebuilding, check nothing listens on 9999 (`netstat -ano | grep ":9999 .*LISTEN"`); ask if it does.
- Stuck 45 min or 3 attempts on one step: revert that step, write what you saw in the plan's Task 1
  findings, and ask the user.

## Review Focus

1. **Bars and units still drawn, not only bars:** the cull change must not make back faces of real
   3D models appear (only RECTANGLES, clear mode, points and lines lose culling; triangles, strips
   and fans keep it). Pinned by `cull_kept_for_triangles` in Task 2.
2. **Clear-mode draws:** the runtime clears through its own path (`psp_ge_draw.cpp:~754`), but the
   helper must still say "no cull" for `clear_mode` so a future caller cannot cull a clear.
3. **Through-mode sprites (2D UI):** PPSSPP does not exempt through mode from culling for
   triangles; only the prim type and clear mode matter. Do not add a through-mode exemption.
4. **Every frame, not most frames:** the arena check counts the bar on N consecutive captures and
   requires all of them (PPSSPP's count is the reference), not "it looks better".
5. **Menus and hideout unchanged:** `rt_regress` against a fresh baseline taken at the start of the
   session (baselines drift with animation phase; `fog_after` failed a byte-identical build).

---

### Task 1: Reproduce, find the bar PRIMs, decide the branch

No production code except a debug-log field. Deliverable: a "Task 1 findings" section appended to
this plan with the evidence, the bar PRIM signature and the branch (A = culling, B = other).

**Files:**
- Modify: `runtime/src/psp_ge_draw.cpp` (`drawlog_prim`, ~line 531: add `cull=` to the PRIM line)
- Modify: `DEBUGGING.md` (`[DL]` section, ~line 407: one sentence on the new field)
- Create (outside the repo): `DECO/build/tools/gfx_arena.json`, `DECO/build/save_arena/` (save copy)

**Interfaces:**
- Produces: `[DL]` PRIM lines ending in `... fog=%d f1=%g f2=%g fc=0x%06X cull=%d/%u` (enable, face);
  the bar PRIM signature (prim type, vtype, texture address, vertex count, through or not) and the
  arena route in `gfx_arena.json`, used by Task 2's verification.

- [ ] **Step 1: Fresh baseline.** With nothing on 9998:
  `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/arena_base`
  (from `DECO`, ~5 min). Expected: the log ends with `31 after_reward` and the out dir.
- [ ] **Step 2: Save copy.** `cp -r DECO/savedata DECO/build/save_arena` (read-only use of the user's
  save; the tours copy it again into `tmp_saves`). Expected: `build/save_arena/UCES01421_DATA01`.
- [ ] **Step 3: Cull state in the draw log.** In `drawlog_prim` append to the format string, after
  `"fog=%d f1=%g f2=%g fc=0x%06X"`, the text `" cull=%d/%u"`, and to the arguments, after
  `s.fog_color & 0xFFFFFF`, the pair `s.cull_enable ? 1 : 0, s.cull_face`. In `DEBUGGING.md`'s
  `[DL]` section add: "`cull=<enable>/<face>` is GE_CMD_CULLFACEENABLE / GE_CMD_CULL (0 = CW,
  1 = CCW) as the game set them; the GL cull actually applied also depends on the prim type (see
  `psp_ge_cull.h`)." (Drop the last clause if branch B is taken.) Build; `ctest` 70/70.
- [ ] **Step 4: Route to the arena.** Write `build/tools/gfx_arena.json`:
  ```json
  [
   {"name": "hideout_obelisco", "keys": "R,R"},
   {"name": "worldmap", "keys": "X", "settle": 4}
  ]
  ```
  Run `TOUR_SAVE=C:/Users/torso/Documents/deco/build/save_arena TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --checkpoints build/tools/gfx_arena.json --out C:/Users/torso/Documents/deco/build/tour/arena_rt --serve`
  in the background, then drive the world map with `python -I build/tools/tstep.py <name> <keys>`
  (one move per call; it prints the PNG to look at) until the Arena del Valor is selected, entered
  and a fight with several units is on screen. Append each step that worked to `gfx_arena.json`
  (`{"name": "arena_<what>", "keys": "...", "settle": N}`). If the arena is not found within three
  tries, stop and ask the user for the exact presses (it may need an unlock their save has, or a
  menu inside the obelisk). End the serve with `touch build/tour/cmd/STOP`; close the runtime by
  the PID the tour log or `netstat -ano | grep ":9998 .*LISTEN"` shows.
- [ ] **Step 5: See the flicker on the runtime and on PPSSPP.** Rerun the tour with the finished
  `gfx_arena.json`, **both sides** (no `--only`; check no user PPSSPP first), `--serve`, out
  `build/tour/arena_both`. Drop this hook as `build/tour/cmd/arena_seq.py.txt` (it captures 40
  frames per side as fast as each side allows):
  ```python
  import time
  for name, side in sides.items():
      t0 = time.time()
      for k in range(40):
          side.frame().save(f'{out}/arena_seq_{name}_{k:02d}.png')
      print(name, 'frames', 40, 'secs', round(time.time() - t0, 1))
  ```
  Look at a contact sheet (`python -I build/tools/sheet.py` or a PIL grid like the fog work's
  `fc_t3_sheet.png`). Expected: on `rt` some frames show the bars and some do not; on `pp` every
  frame shows them. Write both counts into the findings. If `pp` also flickers, stop and tell the
  user: that is the game's own behaviour, not a port bug.
- [ ] **Step 6: Find the bar PRIMs.** While the fight is on screen (serve still running), send
  `D 1` through the runtime socket several times (hook: `sides['rt'].inst.send('D 1')` with 0.3 s
  between sends, 20 sends) and read `build/logs/tour_rt.log`. Find the bars: their PRIMs sit over
  the units, small `bbox`, usually `prim=6` (RECTANGLES, 2 vertices per sprite) and a texture or a
  flat colour. Confirm by draw skip: `X <lo> <hi>` on the socket removes those list indices; a
  screenshot without the bars and with everything else proves the range (then send bare `X`).
  Record the signature: `prim`, `vt`, `ta`, `thr`, `n`, `cull=` and the first two `[DL]   v`
  positions of each bar PRIM, on at least 5 lists.
- [ ] **Step 7: Decide the branch.** For each logged bar PRIM compute the expanded rectangle's
  winding from its two corners: `s = (x1 - x0) * (y1 - y0)` from the two `pos=` values
  (`pos` is clip space; divide x, y by w first). Branch **A** if the bar PRIMs are `prim=6` with
  `cull=1/...` and `s` changes sign between lists where the bar shows and lists where it does not
  (or is always the culled sign on the frames that miss it). Branch **B** otherwise. Write the
  findings (counts from Step 5, signature, the sign table, the branch) as `## Task 1 findings` at
  the end of this plan.
- [ ] **Step 8: Commit** `chore(ge): print the cull state in [DL] draw-log lines` (the `drawlog_prim`
  and `DEBUGGING.md` changes plus the findings section in this plan), ending with `Verified:`
  (build, ctest 70/70, the `[DL]` line seen with `cull=`) and `Agent:` lines.

### Task 2 (branch A): no face culling for sprites, clear mode, points and lines (TDD)

**Files:**
- Create: `runtime/include/psp_ge_cull.h`
- Create: `runtime/tests/test_ge_cull.cpp`
- Modify: `runtime/CMakeLists.txt` (after the `test_ge_fog` block, ~line 688-691)
- Modify: `runtime/src/psp_ge_draw.cpp:1075-1081` (the "Cull face" block in the draw path)
- Modify: `DEBUGGING.md` (only if Step 3 of Task 1 dropped the `psp_ge_cull.h` clause)

**Interfaces:**
- Consumes: Task 1's bar signature and `gfx_arena.json` (verification only).
- Produces: `inline bool ge_want_cull(bool cull_enable, bool clear_mode, int prim_type);` in
  `psp_ge_cull.h` (header-only, includes `psp_ge_constants.h` for `GePrimType`).

- [ ] **Step 1: Write the failing test** `runtime/tests/test_ge_cull.cpp`:
  ```cpp
  // test_ge_cull.cpp -- GE face culling rule against PPSSPP (GPU/GLES/StateMappingGLES.cpp:215:
  // cullEnable = !isModeClear() && prim != GE_PRIM_RECTANGLES && prim > GE_PRIM_LINE_STRIP && isCullEnabled()).
  #include "psp_ge_cull.h"
  #include <cstdio>

  static int g_fail = 0;
  #define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

  static void cull_kept_for_triangles() {
      CHECK(ge_want_cull(true, false, GE_PRIM_TRIANGLES));
      CHECK(ge_want_cull(true, false, GE_PRIM_TRIANGLE_STRIP));
      CHECK(ge_want_cull(true, false, GE_PRIM_TRIANGLE_FAN));
  }
  static void no_cull_for_rectangles() {
      CHECK(!ge_want_cull(true, false, GE_PRIM_RECTANGLES));   // the arena health bars
  }
  static void no_cull_for_points_and_lines() {
      CHECK(!ge_want_cull(true, false, GE_PRIM_POINTS));
      CHECK(!ge_want_cull(true, false, GE_PRIM_LINES));
      CHECK(!ge_want_cull(true, false, GE_PRIM_LINE_STRIP));
  }
  static void no_cull_in_clear_mode() {
      CHECK(!ge_want_cull(true, true, GE_PRIM_TRIANGLES));
      CHECK(!ge_want_cull(true, true, GE_PRIM_RECTANGLES));
  }
  static void no_cull_when_disabled() {
      CHECK(!ge_want_cull(false, false, GE_PRIM_TRIANGLES));
  }

  int main() {
      cull_kept_for_triangles();
      no_cull_for_rectangles();
      no_cull_for_points_and_lines();
      no_cull_in_clear_mode();
      no_cull_when_disabled();
      if (g_fail) { std::printf("test_ge_cull: %d FAIL\n", g_fail); return 1; }
      std::printf("test_ge_cull: PASS\n");
      return 0;
  }
  ```
  Wire it in `runtime/CMakeLists.txt` right after the `add_test(NAME ge_fog_tests ...)` line:
  ```cmake
  # GE face-culling rule (PPSSPP StateMappingGLES). Header-only.
  add_executable(test_ge_cull tests/test_ge_cull.cpp)
  target_include_directories(test_ge_cull PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(test_ge_cull PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
  add_test(NAME ge_cull_tests COMMAND test_ge_cull)
  ```
  And the header with the **current (wrong) rule**, so the test runs and fails on behaviour, not on a
  missing symbol — `runtime/include/psp_ge_cull.h`:
  ```cpp
  // psp_ge_cull.h -- when the GE's face culling applies (PPSSPP StateMappingGLES.cpp:215).
  #pragma once
  #include "psp_ge_constants.h"

  // GE_CMD_CULLFACEENABLE is ignored in clear mode and for RECTANGLES (sprites), points and lines.
  inline bool ge_want_cull(bool cull_enable, bool clear_mode, int prim_type) {
      (void)clear_mode;
      (void)prim_type;
      return cull_enable;   // runtime behaviour before this fix: cull every prim
  }
  ```
- [ ] **Step 2: Run it.** `cmake --build DECO/build/rtw --target test_ge_cull && DECO/build/rtw/test_ge_cull.exe`.
  Expected: FAIL lines for `no_cull_for_rectangles`, `no_cull_for_points_and_lines` and
  `no_cull_in_clear_mode`; `test_ge_cull: 6 FAIL`. If it passes, the test is wrong.
- [ ] **Step 3: Implement the PPSSPP rule** (replace the function body):
  ```cpp
  inline bool ge_want_cull(bool cull_enable, bool clear_mode, int prim_type) {
      return cull_enable && !clear_mode && prim_type != GE_PRIM_RECTANGLES &&
             prim_type > GE_PRIM_LINE_STRIP;
  }
  ```
- [ ] **Step 4: Run it.** Same command. Expected: `test_ge_cull: PASS`.
- [ ] **Step 5: Use it in the draw path.** In `runtime/src/psp_ge_draw.cpp` add
  `#include "psp_ge_cull.h"` next to the other `psp_ge_*.h` includes and replace the block
  ```cpp
      // Cull face
      gl_state_set_cap(GL_CULL_FACE, state.cull_enable);
      if (state.cull_enable) {
  ```
  with
  ```cpp
      // Cull face (never for sprites, points, lines or clear mode; PPSSPP StateMappingGLES)
      const bool cull = ge_want_cull(state.cull_enable, state.clear_mode, prim_type);
      gl_state_set_cap(GL_CULL_FACE, cull);
      if (cull) {
  ```
  (the `glCullFace(...)` call and closing brace stay). Check `prim_type` is the GE prim type at that
  point (the parameter of `ge_draw_prim`, before `map_prim_type`), not the GL one.
- [ ] **Step 6: Build and test.** `cmake --build DECO/build/rtw`: no new warnings.
  `ctest --test-dir DECO/build/rtw`: 71/71.
- [ ] **Step 7: Arena check.** Rerun Task 1 Step 5 (both sides, `gfx_arena.json`, hook
  `arena_seq.py.txt`), out `build/tour/arena_fix`. Expected: the bars are on all 40 `rt` frames,
  as on `pp`. Also re-log `D 1` once: the bar PRIMs still print `cull=1/...` (the game's register),
  they are simply no longer culled. Put a before/after/PPSSPP side-by-side PNG in
  `build/tour/arena_fix/comparacion.png`.
- [ ] **Step 8: Regression.** `rt_regress` with the plain tour:
  `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/arena_after`,
  then `python -I build/tools/rt_regress.py build/tour/arena_base build/tour/arena_after`.
  Expected: `RT_REGRESS PASS` (21 gated checkpoints ≤ 0.5 % struct). A failure on hideout
  animation alone: rerun once, as the script says, and compare the two runs with each other
  before blaming the change. Also look at the mission checkpoints (`25_march` .. `27_attack_combo`)
  by eye: units, Hero and enemies must look the same (no back faces showing).
- [ ] **Step 9: Commit** `fix(ge): no face culling for sprites, points, lines or clear mode`
  (header, test, CMake, draw path), ending with `Verified:` (test RED→GREEN, ctest 71/71, arena
  40/40 frames with bars vs PPSSPP 40/40, rt_regress PASS) and `Agent:` lines.

### If Task 1 rules out culling (branch B)

Do not write a fix yet. Use superpowers:systematic-debugging with the Task 1 evidence and check, in
this order (each is one comparison of a "bar shown" list against a "bar missing" list):

1. **Is the bar PRIM in the missing list at all?** If the `[DL]` lines of a missing frame have no
   PRIM with the bar signature, walk the guest's list (`{"gelist": 1}` serve job on both sides).
   Absent on `rt` but present on `pp`: the game logic decided not to draw it (timing/parity input;
   compare the guest state the game reads, e.g. vcount from `sceDisplayGetVcount`). Present in the
   guest list but no `[DL]` line: the runtime dropped or cut the list (list stall/abort; look for
   `[GE]` warnings at that time).
2. **Depth:** compare `zt`, `zf`, `zw` and the vertex z of the bar with what is under it
   (`[DL]` fields). Equal z with `zf` GEQUAL/LESS-type functions flickers with tiny z changes.
3. **Alpha test / blend:** `at`, `atv`, `blend`, `bm`, `fa`, `fb2` and the vertex alpha (`rgba=`).
4. **Range culling and clipping:** `ge_vertex_range_culled` (`psp_ge_vertex.h:64`) and vertices
   with `w <= 0` dropped by the rectangle expansion; compare with PPSSPP's `DrawEngineCommon`.
5. **Texture:** same `ta`/`tf`/`clut` on both frames but different pixels: the texture cache served a
   stale or transparent copy (`texdump.py`, `PSPRECOMP_TEXTURES` off).

Write the finding into `## Task 1 findings`, then add a Task 2B to this plan in the same
format as Task 2 (failing test first, built from the logged values), and execute it.

## Finish

- Final whole-branch review (fresh reviewer on the most capable model) over the task commits.
- Rebuild the play exe (`cmake --build DECO/build/rtw`) so the user's game has the fix.
- Tell the user (in Spanish) what changed, the gates, and ask them to play the arena with
  `jugar.bat` **and** `jugar_60fps.bat`. Do not push before their OK.

## Pushing (only when the user asks)

`p3/main` does not contain `f639180`, but local `patapon3-windows` does, so a plain push is refused
(non-fast-forward) and must never be forced. Push the same way as on 2026-10-16:

1. `git fetch p3`; `git worktree add -b push-p3-N .claude/worktrees/wt-push-N p3/main`
   (pick an unused N; `push-p3` and `push-p3-2` exist).
2. In the worktree: `git cherry-pick <first new commit>^..<last new commit>`.
3. `git diff push-p3-N patapon3-windows` must equal the icon commit: compare only the changed lines,
   `diff <(git diff push-p3-N patapon3-windows | grep "^[-+]") <(git show --format= f639180 | grep "^[-+]")`
   (empty output; context lines differ because the icon commit is older).
4. Verification build of the worktree in `DECO/build/rt_push<N>` with the `build/rtw` cache values
   **plus** `-DSDL2_DIR=C:/Users/torso/Documents/deco/tools/SDL2-2.32.10/x86_64-w64-mingw32/lib/cmake/SDL2`,
   `-DFETCHCONTENT_SOURCE_DIR_ZLIB=C:/Users/torso/Documents/deco/tools/zlib-1.3.1` and
   `-DPSPRECOMP_ALLOW_STALE=ON` (the worktree's CRLF crates trip the emitter fingerprint; confirm
   `git diff p3/main push-p3-N -- crates/` is empty first), `-G Ninja -DCMAKE_BUILD_TYPE=Release
   -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DPSPRECOMP_GAME=patapon3`,
   `PSPRECOMP_BANK_DIRS`, `PSPRECOMP_FFMPEG_DIR`, `PSPRECOMP_OUTPUT_DIR` as in `build/rtw/CMakeCache.txt`
   (no `PSPRECOMP_ICON`). `ctest` there: one fewer than local (no icon test).
5. `git push p3 push-p3-N:main`, then `git worktree remove .claude/worktrees/wt-push-N`.
