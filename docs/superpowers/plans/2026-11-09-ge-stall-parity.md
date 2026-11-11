# GE Stall Parity (beta report #100 freeze) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A display list never runs past its stall address, and a stall update that arrives while the list is being drawn is never lost, so the renderer cannot wander into unwritten memory and leave the game waiting forever (report #100).

**Architecture:** Two pure rules in a new header (`psp_ge_stall.h`), used by the interpreter (`psp_ge.cpp`) and the list queue (`psp_render_queue.cpp`). Both follow PPSSPP `GPU/GPUCommon.cpp`: list pc and stall are compared masked to 28 bits, and a list is stalled exactly when `pc == stall`.

**Tech Stack:** C++17 runtime, ctest (plain `main()` tests with a `CHECK` macro), Python tour tools in `build/tools`.

**Spec:** this section (no separate design doc).

### What the report shows

`build/beta/reportes_2026-11-09/copia/reporte100.log` (Zackbyss, 0.3.0-beta.16, dxd-1.1.5b, GL_API=es, 60fps, fast load, online). Last lines before the watchdog:

```
[DISPLAY] fast-forward: 183 vblanks in 1537 ms
[GE] Hit command limit (4194304) without END at pc=0x05A84BE4
[FREEZE] no new frame for 15 s      (user_main RUNNING, last_func=0x0883BFBC)
```

Same line in old reports #41 and #53 (tower floor change, 25 Oct).

### Confirmed defects (9 Nov, by reading the code and a live probe on port 9998)

1. **The stall address is ignored after the first JUMP.** The game enqueues its frame list in uncached space (`stall=0x49002070` in every log). `ge_relative_address` returns `& 0x0FFFFFFF`, so after any `JUMP` the pc is `0x09xxxxxx`. The stall test is `pc >= stall_addr` on raw values (`psp_ge.cpp:184`): `0x09xxxxxx >= 0x49xxxxxx` is never true. The live frame list has a JUMP every few dozen commands (60 JUMP / 105 CALL in the first 32 KB at `0x09000000`). So whenever the renderer is ahead of the game it keeps reading what the game has not written yet.
2. **A stall update can be lost.** `render_queue_process` copies the entries, unlocks, draws, then writes `stalled = true` back (`psp_render_queue.cpp:228`). A `render_queue_update_stall` that lands in between sets `stalled = false` and is overwritten: the list sleeps with new commands pending until another update comes. If that update was the last of the frame, none comes.
3. After the command limit the list stays queued at a garbage pc, `completed = false`, and nothing ever finishes it.

**Not confirmed:** that #100 is defect 1 and not something else that corrupts the list. Task 1 measures it before any fix.

**PPSSPP reference:** `tools/ppsspp-src/GPU/GPUCommon.cpp` lines 579-581 (`dl.pc = listpc & 0x0FFFFFFF; dl.stall = stall & 0x0FFFFFFF`), 665 (update), 976 and 996 (`list.pc == list.stall`).

## Global Constraints

- Local branch `ge-stall` in `psprecomp/`, created from `4f9ab65` (the code of beta u21). Never `achi-rework`: it carries the font whose licence is open.
- No push, no package, no beta build. Commit author "Patapon Community" (`GIT_AUTHOR_*` and `GIT_COMMITTER_*` env vars), no `Co-Authored-By` line, no change to `git config` or `.git/`.
- Frozen: `psprecomp/crates/`, `analysis/`, `Cargo.*`, `game.toml`, `build/p3_output/`, `build/ovl/`, `disc0/`, `savedata*/`.
- Tests use port 9998. Never touch the user's game on 9999; kill only by PID.
- Evidence per commit, pasted in the commit body as `Verified: ...`:
  `cd C:/Users/torso/Documents/deco/build/rtw && ninja 2>&1 | tail -20`, `ctest --output-on-failure 2>&1 | tail -15`, then the tour with `PSPRECOMP_RTC_FIXED` set and `python -I build/tools/rt_regress.py build/tour/relevo_base build/tour/ge_stall_<n>` = PASS.
- Runtime core only (`runtime/src`, `runtime/include`): the rule is PSP behaviour, not Patapon behaviour.
- Code and commits in English.

## Review Focus

1. A list enqueued with `stall == list address` (empty so far): must stall at once and draw nothing. Test in Task 2.
2. `stall == 0`: no stall, runs to END. Test in Task 2.
3. Stall update while the list is being drawn: the list must be drawn again without another update. Test in Task 3.
4. `sceGeDrawSync(0)` / `sceGeListSync` while a list is truly stalled: must not add 500 ms waits. Gate in Task 4 (`DrawSync timeout` count = 0 in the tour log).
5. A stall reached inside a CALLed sub-list would lose the call stack (it is local to one `ge_process_display_list` call). With `pc == stall` this needs a stall address inside a sub-list, which the game does not do. Reviewer: confirm no `RET with empty stack` in the tour log; no code for it.

---

### Task 1: Measure the defect before fixing it (no commit)

**Files:**
- Temporary edit, reverted at the end of the task: `runtime/src/psp_ge.cpp`

- [ ] **Step 1:** Create branch `ge-stall` from `4f9ab65`.
- [ ] **Step 2:** Temporary counter in `ge_process_display_list`: once per call, when `stall_addr != 0` and `(pc & 0x0FFFFFFF) == (stall_addr & 0x0FFFFFFF)` is seen while the raw test did not stop the list, print `[GE-STALL] passed stall 0x%08X at pc 0x%08X` (first 20 only) and count the rest; print the total at exit.
- [ ] **Step 3:** Build, then run the tour on port 9998 with `PSPRECOMP_PATCHES=60fps`, `PSPRECOMP_P3_FAST_LOAD=1`, plus three title → Continue → hideout → mission → back-to-title rounds (the path of the last minutes of the report).

Run: `grep -c "GE-STALL" <log>; grep -c "Hit command limit\|not found in queue" <log>`
Expected: either count of `[GE-STALL]` > 0 (defect 1 happens in normal play: go on) or 0 in all rounds (defect 1 is real but did not fire here: go on with Tasks 2-3 as hardening, and write in the ledger that #100 is not explained).
- [ ] **Step 4:** `git checkout -- runtime/src/psp_ge.cpp`. Write the counts in the ledger and in `build/beta/reportes_2026-11-09/TRIAGE.md` (row #100).

### Task 2: Stall test as PPSSPP does it

**Files:**
- Create: `runtime/include/psp_ge_stall.h`
- Create: `runtime/tests/test_ge_stall.cpp`
- Modify: `runtime/src/psp_ge.cpp:183-192` (stall check), `runtime/CMakeLists.txt` (after the `ge_list_blend_tests` block)

**Interfaces:**
- Produces: `inline bool ge_at_stall(uint32_t pc, uint32_t stall)`; `inline bool ge_stall_still_current(uint32_t stall_drawn_with, uint32_t stall_now)` (used by Task 3).

- [ ] **Step 1: Write the failing test** `runtime/tests/test_ge_stall.cpp` (same `CHECK` style as `test_ge_list_blend.cpp`):

```cpp
CHECK(ge_at_stall(0x49002070, 0x49002070), "same address stalls");
CHECK(ge_at_stall(0x09002070, 0x49002070), "pc after a JUMP (28 bits) still meets an uncached stall");
CHECK(ge_at_stall(0x49002070, 0x09002070), "and the other way round");
CHECK(!ge_at_stall(0x0900206C, 0x49002070), "one command before the stall keeps running");
CHECK(!ge_at_stall(0x09A6E604, 0x49002070), "a sub-list above the stall address is not a stall");
CHECK(!ge_at_stall(0x09002070, 0), "stall 0 means no stall");
CHECK(ge_stall_still_current(0x49002070, 0x49002070), "no update while drawing: stays stalled");
CHECK(!ge_stall_still_current(0x49002070, 0x49002400), "update while drawing: must run again");
```

- [ ] **Step 2: Register and run it**: `add_executable(test_ge_stall tests/test_ge_stall.cpp)` + include dir + `add_test(NAME ge_stall_tests COMMAND test_ge_stall)`.

Run: `ninja test_ge_stall 2>&1 | tail -5`
Expected: FAIL to compile, `psp_ge_stall.h: No such file`.

- [ ] **Step 3: Implement `psp_ge_stall.h`**: both functions compare `& 0x0FFFFFFFu`; `ge_at_stall` is false for `stall == 0`. Header comment cites `GPUCommon.cpp` 579-581 and 996.
- [ ] **Step 4: Use it** in `psp_ge.cpp`: `if (ge_at_stall(pc, stall_addr))` replaces `stall_addr != 0 && pc >= stall_addr`. Nothing else changes in the loop.
- [ ] **Step 5: Run**

Run: `ninja 2>&1 | tail -5 && ctest --output-on-failure 2>&1 | tail -5`
Expected: all tests pass, `ge_stall_tests` among them.

- [ ] **Step 6: Commit** `fix(ge): stop a display list at its stall address after a JUMP`.

### Task 3: Do not lose a stall update that arrives while drawing

**Files:**
- Modify: `runtime/src/psp_render_queue.cpp:222-232` (write-back loop)

**Interfaces:**
- Consumes: `ge_stall_still_current(uint32_t, uint32_t)` from Task 2.

- [ ] **Step 1:** The rule is already under test (Task 2, last two checks). In the write-back loop, `queued.stalled = processed.stalled && ge_stall_still_current(processed.stall_addr, queued.stall_addr);` and leave `queued.stall_addr` as the game set it. One comment line: why (the update came while the lock was released).
- [ ] **Step 2: Run**

Run: `ninja 2>&1 | tail -5 && ctest --output-on-failure 2>&1 | tail -5`
Expected: all pass.

- [ ] **Step 3: Commit** `fix(ge): keep a stall update that arrives while its list is drawn`.

### Task 4: Proof in the game

**Files:**
- Modify: `build/beta/CHANGELOG.md` (one player line under the update in preparation), `build/beta/reportes_2026-11-09/TRIAGE.md`

- [ ] **Step 1:** Tour + `rt_regress.py` (commands in Global Constraints).
Expected: PASS.
- [ ] **Step 2:** Same three rounds as Task 1 Step 3, on port 9998, 60 fps + fast load, once with `PSPRECOMP_GL_API=es`.

Run: `grep -c "Hit command limit\|DrawSync timeout\|not found in queue\|RET with empty stack\|\[FREEZE\]" <log>`
Expected: 0.
- [ ] **Step 3:** `python -I build/tools/perf_hideout.py` before (4f9ab65 build) and after.
Expected: frame time within noise (the list now really waits for the game; a slowdown here is a finding, stop and report).
- [ ] **Step 4:** Amend nothing; add the `Verified:` evidence in a final commit `docs(beta): note the display-list stall fix` with the CHANGELOG line ("Arreglado un congelamiento raro al cambiar de pantalla.") and the TRIAGE row.

## Out of scope (named, not done)

- Command-limit recovery (finishing or dropping a runaway list): PPSSPP has no such limit; if Task 4 is clean it is not needed.
- Keeping the GE call stack across a stall.
- Reports #94 / #99: no starting point. Their `[BT]` is the crash handler's own stack (`+b77404` = `psp_crash_filter` in the shipped 1.1.5b exe), not the crash. A separate small change would give one next time: when the fault address is 0, print the return address at the faulting `Rsp`.
