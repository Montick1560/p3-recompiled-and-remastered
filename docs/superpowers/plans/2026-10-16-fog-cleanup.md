# GE fog cleanup (four deferred minors) — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended: small,
> independent tasks) or superpowers:subagent-driven-development to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

Written 2026-10-16 (Opus 5.5) for the next session. Nothing implemented yet.

**Goal:** close the four minors the final review of `2026-10-16-hideout-white-flash.md` deferred.
None of them changes what the player sees today; they remove a duplicate, a latent stale-value
hazard and two debugging blind spots, so later GE work does not trip on them.

**Architecture:** four independent, small tasks in the GE code (`runtime/src/psp_ge.cpp`,
`psp_ge_vertex.cpp`, `psp_ge_draw.cpp`). Tasks 1 and 4 are refactors whose gate is "the build, the
unit tests and `rt_regress` are unchanged"; Task 2 is a real (latent) bug fixed by TDD; Task 3 is a
debug-log addition checked by reading a log.

**Tech Stack:** C++17 runtime, CTest unit tests, `rt_regress.py` in `DECO/build/tools`.

**Spec:** user, 2026-10-16: *"vale, dale a esas mejoras, pero arma el plan para hacer clear y
empezarlo en otra sesión"*, i.e. the four items listed as "Pendientes menores" at the end of the
white-flash work (the review's deferred minors, quoted in each task). No separate spec file.

`DECO` = `C:/Users/torso/Documents/deco`. Repo = `DECO/psprecomp`, branch `patapon3-windows`.

## State when this plan was written

- Fog shipped: pushed to `p3/main` on 2026-10-16 together with the fast-exit work, **without** the
  local exe-icon commit `f639180`. Local `patapon3-windows` still has `f639180` below those commits,
  so its history differs from `p3/main` (same content plus the icon). See "Pushing" below.
- `ctest --test-dir DECO/build/rtw`: 70/70 on `patapon3-windows`. `test_ge_vertex`: 197 checks.
- `rt_regress` baseline that already includes fog: `fog_after` (from the white-flash Task 4 gate).
  Use it as the reference for every task here; all 21 gated checkpoints must stay at 0.00%.

## Global Constraints

- Test instances only on **port 9998** (`TOUR_RT_PORT=9998`). Never touch the user's game (port 9999)
  or `DECO/savedata`; use copies (`build/tour_save`).
- Always `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00` for tours and `rt_regress`.
- Never set `PSPRECOMP_NO_AUDIO=1`. Nothing destructive on the PC, no installs, no system security
  changes. Ask the user before closing any PPSSPP or runtime they opened (check the port and the start
  time before killing anything; kill your own instances by PID).
- Never edit `build/p3_output/generated/*.cpp`; no game-specific code in `runtime/src`.
- Commits: author "Patapon Community" (the repo's configured identity; do not touch git config),
  **no `Co-Authored-By`**, English, one commit per task, ending with `Verified: ...` and `Agent: ...`
  lines. Talk to the user in Spanish.
- **No push** until the user says so. **Never push `f639180`** (local exe icon).
- Line endings: `runtime/` sources and `DEBUGGING.md` are CRLF on this checkout (`crates/` is LF);
  prefer the Edit tool over shell rewrites, which can flip a whole file's endings.
- Build: `cmake --build DECO/build/rtw` builds the user's play exe (`build/rtw/psprecomp_runtime.exe`)
  and the unit tests. Before rebuilding, check that no `psprecomp_runtime.exe` is listening on 9999
  (the user playing); ask if one is.

## Review Focus

1. **Task 1 masking:** `data_to_float` shifts `data << 8` without masking; `ge_float24` masks to 24
   bits first. Identical only because the dispatcher already masks (`psp_ge.cpp:201`,
   `data = word & 0x00FFFFFF`). Any other caller of `data_to_float` must also pass 24-bit data.
2. **Task 2:** every field of `DecodedVertex` must be fresh after `ge_decode_vertices`, not only `fog`
   (the test pins `fog`, the fix makes all of them fresh).
3. **Task 4:** the attribute offsets passed to `glVertexAttribPointer` must come from `offsetof`, so a
   future field reorder cannot desync the shader inputs silently.

---

### Task 1: One float24 decoder

Review minor: *"`ge_float24` duplicates `data_to_float` in psp_ge.cpp."*

**Files:** `runtime/src/psp_ge.cpp` (`data_to_float` at ~line 74, ~25 call sites),
`runtime/include/psp_ge_fog.h` (`ge_float24`, the copy to keep).

- [ ] **Step 1:** confirm every `data_to_float(` caller gets the masked `data` from the dispatch loop
  (`grep -n "data_to_float(" runtime/src/psp_ge.cpp`; all are inside the `switch` fed by line 201).
  If one is not, stop and ledger a ruling (masking would change its value).
- [ ] **Step 2:** delete `data_to_float` and replace each call with `ge_float24(data)`
  (`psp_ge_fog.h` is already included by `psp_ge.cpp` for `ge_fog_coef`; check the include). Consider
  moving `ge_float24` out of the fog header into a small `psp_ge_float24.h` only if another file
  needs it; otherwise leave it where it is (one header, no churn).
- [ ] **Step 3:** build. Expected: no warnings added.
- [ ] **Step 4:** `ctest --test-dir DECO/build/rtw`. Expected: 70/70 (no test change; `fog_tests`
  already pins `ge_float24(0x3F8000) == 1.0`).
- [ ] **Step 5:** `rt_regress` vs `fog_after`. Expected: PASS, 21/21 at 0.00% (bone matrices,
  viewport, texture scale and lights all go through this decoder, so the menus and hideout are a
  wide check).
- [ ] **Step 6:** commit `refactor(ge): one float24 decoder (ge_float24)`.

### Task 2: Fresh vertices from `ge_decode_vertices` (TDD)

Review minor: *"`ge_decode_vertices` calls `resize` without `clear()`, a latent stale-fog hazard."*

Today every caller passes a fresh local vector, so nothing breaks. But `out.resize(count)` on a reused
vector keeps the old elements, and `ge_transform_vertices` only writes `fog` when fog is on and the
draw is not on the NDC fallback path, so a reused vector would carry a previous draw's fog factor.

**Files:** `runtime/src/psp_ge_vertex.cpp:281` (`out.resize(count);`),
`runtime/tests/test_ge_vertex.cpp` (new test, wired in `main()`).

- [ ] **Step 1 (RED):** add `test_decode_resets_reused_vector` next to `test_through_s16_z_unsigned`
  (same rdram setup: `calloc(0x08000000)`, one through-mode s16 vertex at `0x08800000`,
  `vertex_type = 0x00800100`). Before decoding, fill `out` with two vertices whose `fog = 0.25f` and
  `has_uv = true`. Decode `count = 1`. Expect `out.size() == 1`, `out[0].fog == 1.0f` (`ASSERT_NEAR`)
  and `out[0].has_uv == false`. Free the ram. Call it from `main()`.
- [ ] **Step 2:** build and run `test_ge_vertex`. Expected: FAIL on the fog check (resize kept
  `fog = 0.25`). If it passes, the test is wrong: fix the test, not the code.
- [ ] **Step 3 (GREEN):** replace `out.resize(count);` with `out.assign(count, DecodedVertex{});`.
- [ ] **Step 4:** run `test_ge_vertex`. Expected: PASS, 197 + the new checks. Then
  `ctest --test-dir DECO/build/rtw`: 70/70.
- [ ] **Step 5:** `rt_regress` vs `fog_after`. Expected: PASS, 21/21 at 0.00%.
- [ ] **Step 6:** commit `fix(ge): decode into fresh vertices (no stale fog on a reused vector)`.

### Task 3: Fog in the `[DL]` draw log

Review minor: *"The DRAWLOG `[DL]` lines do not print fog."* During the white-flash work the fog
registers had to be read with `build/tools/gelist.py` instead.

**Files:** `runtime/src/psp_ge_draw.cpp` (`drawlog_prim` ~line 530, per-vertex line ~line 970),
`DEBUGGING.md` (the `[DL]` section, ~line 407, and the `D <lists>` row ~line 225).

- [ ] **Step 1:** in `drawlog_prim`, append `fog=%d f1=%g f2=%g fc=0x%06X` to the PRIM line with
  `s.fog_enable ? 1 : 0, s.fog1, s.fog2, s.fog_color & 0xFFFFFF` (same names `gelist.py` prints).
- [ ] **Step 2:** in the per-vertex `[DL]   v%zu/%zu ...` line, append `fog=%.3f` with `pv.fog`.
- [ ] **Step 3:** check `DECO/build/tools/dlseq.py` (and any other parser of `[DL]` lines in
  `build/tools`) still parses: they must key on field names, not on positions. Fix a positional
  parser if there is one (it is outside the repo; note it in the commit message).
- [ ] **Step 4:** build; `ctest` 70/70.
- [ ] **Step 5:** verify by reading a real log: start a runtime on port 9998 (`p3drive.py` boot to the
  hideout, or the tour's `--to mission_clear`), send `D 1` on the debug socket, and read the `[DL]`
  lines in its log. Expected: every PRIM line ends in `fog=0 f1=... f2=... fc=0x......`; in the
  mission end (white silhouettes) the unit PRIMs show `fog=1 f1=1 f2=10 fc=0xFFFFFF` (as in the
  white-flash plan's evidence) and their vertices show `fog=` values ≤ 0. Close your instance by PID.
- [ ] **Step 6:** update `DEBUGGING.md` (the `[DL]` section: the PRIM line now carries the fog
  registers and each logged vertex its fog factor).
- [ ] **Step 7:** commit `chore(ge): print fog registers and factors in [DL] draw-log lines`.

### Task 4: Pin the `PackedVertex` layout

Review minor: *"There is no `offsetof(PackedVertex, fog)==28` assert."*

**Files:** `runtime/src/psp_ge_draw.cpp` (`struct PackedVertex` ~line 97, `glVertexAttribPointer`
calls ~lines 408-433).

- [ ] **Step 1:** next to `static_assert(sizeof(PackedVertex) == 32, ...)` add
  `static_assert(offsetof(PackedVertex, uv) == 16 && offsetof(PackedVertex, color) == 24 &&
  offsetof(PackedVertex, fog) == 28, "PackedVertex layout must match the shader attributes");`
  (include `<cstddef>` if it is not already there).
- [ ] **Step 2:** replace the literal offsets `0/16/24/28` in the four `glVertexAttribPointer` calls with
  `reinterpret_cast<void*>(offsetof(PackedVertex, pos))` etc., and drop the now-redundant offset
  numbers from their comments.
- [ ] **Step 3 (proof the assert bites):** temporarily swap `color` and `fog` in the struct, build,
  and confirm the static_assert fails with the message; restore the struct. Do not commit the swap.
- [ ] **Step 4:** build; `ctest` 70/70 (or 70 + Task 2's checks); `rt_regress` vs `fog_after`:
  PASS, 21/21 at 0.00%.
- [ ] **Step 5:** commit `refactor(ge): PackedVertex attribute offsets from offsetof, layout asserted`.

## Finish

- Final whole-branch review (fresh reviewer on the most capable model) over the four commits.
- Rebuild the play exe (`cmake --build DECO/build/rtw`) so the user's game has the cleanup.
- Tell the user (in Spanish) what changed (nothing visible), the gates, and that nothing is pushed.

## Pushing (only when the user asks)

`p3/main` does not contain `f639180`, but local `patapon3-windows` does, so a plain push is refused
(non-fast-forward) and must never be forced. Push the same way as on 2026-10-16: a branch from
`p3/main` with only the new commits cherry-picked
(`git worktree add -b push-p3-N .claude/worktrees/wt-push-N p3/main`,
`git cherry-pick <first>^..<last>`), check `git diff <local tip> <push tip>` equals the icon commit
only, build and `ctest` that tree, then `git push p3 push-p3-N:main`. A worktree checks the crates out
with CRLF, which trips the emitter fingerprint check in CMake (false "stale output/"); configure that
verification build with `-DPSPRECOMP_ALLOW_STALE=ON` after confirming `git diff` shows no `crates/`
change.
