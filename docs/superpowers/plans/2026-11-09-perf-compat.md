# Performance and Compatibility Program Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Know where the recompiled game loses time on players' PCs, fix what the numbers justify, and widen the set of PCs that can start it.

**Architecture:** Measure first (Tasks 1-4, no product code), then make the field visible (Task 5), then only the fixes whose gate passed (Tasks 6-9). Everything the measurements do not justify stays in the backlog at the end and gets its own plan later.

**Tech Stack:** C++17 runtime (clang, llvm-mingw), SDL2 + OpenGL 3.3 / ES 3.0 (ANGLE), Python lab tools in `build/tools`, SQLite beta server (`tools/infra_server.py`).

**Spec:** none. The evidence below is the argument; rulings made without a spec are provisional.

## Global Constraints

- Local branch `perf-compat` from `ge-stall` (eede072), in its own worktree and build dir (as `build/wt_gestall` + `build/rtw_gestall`). Never overwrite `build/rtw`.
- No push, no package, no CHANGELOG line without the user's OK. Commit author `Patapon Community <rexgamer1560@gmail.com>`, no Co-Authored-By.
- Test instances on debug port 9998 only; kill by PID; nothing typed into open windows.
- No measurement while anything else builds or runs; `build/infra/diskquiet.py` before each sample; two samples per side before claiming a change under 5 %.
- A fix ships only with: ctest all green, `rt_regress.py` PASS (`PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`), `perf_hideout.py` before/after.
- Baseline x86-64 only: no `-march` above the default (the field has Ivy Bridge and older).
- No hardware names leave a player's PC in session stats; classes and counts only (Task 5).

## Evidence (9 Nov 2026)

Lab, this PC (RTX, 164 Hz), exe `build/rtw` (achi-rework), hideout, 60 fps, scale 2 (`build/tour/perf_plan_now`):
- 60.0 fps, GE 0.8 ms/frame, 45 draws, 33.4 % of one core, 136 MB RAM. The game is light here.
- CPU split: video driver + present 22 %, guest code + runtime 43 %, `ntdll` 17 %.
- Inside `ntdll`: heap alloc 3.4 %, ANSI/Unicode string conversion and `LdrLoadDll` about 5 %. Nothing in a steady frame should load a DLL or convert strings. Caller unknown.
- Known hot spots, unchanged since 1 Nov: `RECOMP_LOOKUP` 3.7 %, `psp_trace_checkpoint` 2.1 %.

Code:
- Runtime and generated code build at `-O2`, no LTO, no PGO.
- `SDL_GL_SetSwapInterval` is never called: vsync is whatever the driver defaults to, on top of the 59.94 Hz timer pacing. Players' screens are 60, 75, 100, 165, 180 Hz.
- No process priority, power-throttling or background handling at all.

Field (beta.db, 6-9 Nov, 51 online players, 227 sessions):
- Frames of 100 ms or more: median player 21 per hour, worst tenth 260 per hour, one player 11 % of all frames.
- 15 sessions had a frame of 1 s or more, 6 of 5 s or more.
- The server cannot say who runs below target: it only counts frames >= 100 ms and knows nothing about the PC.
- Hardware seen in 16 reports: half are integrated GPUs (HD 4000, HD 530, UHD, Iris Xe, Vega, 610M), several with 4-8 GB RAM and 1366x768.

Compatibility:
- #95: crash inside the Intel GL driver (`igxelpicd64.dll`, Iris Xe, driver 30.0.101.1404).
- #101: Intel HD (D3D 10_0): neither GL 3.3 nor ANGLE ES 3.0. The PC lists Microsoft Basic Render Driver (D3D 11_1, software).
- HD 4000 numbers were never taken (`perf_tester.py` needs `TESTER_PASS` in the shell).

Never measured: a mission with many units, loads, 30 fps mode, a weak PC.

## Review Focus

- A player on a 60 Hz screen after Task 7: no new judder, no tearing.
- The game minimized or behind another window during online co-op after Task 8: keeps its pace, does not burn a core.
- An old server receiving the Task 5 fields, and a new server receiving a u21 client: both still accept the report.
- Laptop on battery after Task 8: no change in power use that the player did not ask for while the game is paused in the launcher.
- A PC where the ES fallback (Task 9) also crashes: no restart loop.

---

### Task 1: Frame log of a whole tour (loads, mission, reward)

**Files:** none in the repo. Output `build/tour/perf_tour_60/`, `build/tour/perf_tour_30/`.

- [ ] **Step 1:** Run the tour twice (60 fps, then 30 fps) with the frame log on:
  `PSPRECOMP_FRAMELOG=<abs path>/frames.csv P3DRIVE_EXE=psprecomp_runtime_gestall.exe TOUR_RT_PORT=9998 PSPRECOMP_P3_FAST_LOAD=1 [PSPRECOMP_PATCHES=60fps] python -I build/tools/gfx_tour.py --only rt --out <dir>`
- [ ] **Step 2:** `python -I build/tools/framelog.py <dir>/frames.csv`. Record p50/p99/p99.9, hitches > 25 ms and the 15 worst frames with their columns (`ge_ms`, `tex_miss_ms`, `pack_decode_ms`, `swap_ms`, `cpu_wait_ms`).
- [ ] **Step 3:** Same at 60 fps with the HD pack (`PSPRECOMP_TEXTURES` as `jugar.bat` sets it).
- [ ] **Step 4:** Write the table to `build/autopilot/RENDIMIENTO_2026-11-09.md` with one line per hitch class: what column explains it.

Expected: every hitch over 50 ms is assigned to a column or marked "unexplained".

### Task 2: Mission CPU profile and the `ntdll` caller

**Files:** Modify `build/tools/cpu_profile.py` (lab tool, outside the repo; keep a `.pre_stacks` copy).

- [ ] **Step 1:** Add `--stacks MODULE`: for samples whose RIP is in MODULE, walk the stack with `dbghelp!StackWalk64` (x64 unwind data; no frame pointers needed) and print the 10 most common first frames outside MODULE. Self-check in the tool: profiling its own Python process for 2 s returns at least one resolved caller.
- [ ] **Step 2:** Hideout, 60 fps, scale 2: `perf_hideout.py stacks --scale 2 --profile` with `--stacks ntdll.dll`. Expected: a named caller for the string-conversion and `LdrLoadDll` samples.
- [ ] **Step 3:** Mission profile: drive to the mission (`to_mission.sh` or the tour's mission checkpoint), then `cpu_profile.py <pid> 20`. Record threads, modules, top 30 functions.
- [ ] **Step 4:** Append both to the report. State which share is guest code (`FUN_*`), runtime, driver.

Gate for Task 6: Step 2 names a caller in `psprecomp_runtime.exe` or in how we call SDL.

### Task 3: Swap interval and pacing on a non-60 Hz screen

**Files:**
- Modify: `runtime/src/psp_event_loop.cpp` (after `SDL_GL_CreateContext`, line ~516)
- Modify: `runtime/src/psp_settings.cpp` + its table (`[display] vsync = auto|on|off` -> `PSPRECOMP_VSYNC`)
- Test: `runtime/tests/test_settings.cpp`

- [ ] **Step 1: failing test** in `test_settings.cpp`: `[display] vsync = off` yields env `PSPRECOMP_VSYNC=off`; a value outside the three is dropped.
- [ ] **Step 2:** Run `ctest -R settings`. Expected: FAIL (unknown key).
- [ ] **Step 3:** Add the key. In the event loop: log `[RT] swap interval: <SDL_GL_GetSwapInterval()>` always; with `on`/`off` call `SDL_GL_SetSwapInterval(1/0)`; `auto` changes nothing yet.
- [ ] **Step 4:** ctest green. Commit `feat(display): vsync setting and swap interval log`.
- [ ] **Step 5:** Measure on this PC (164 Hz) at 60 fps, 60 s of hideout each, `auto`/`on`/`off`: framelog p50/p99 of `dt_ms`, `swap_ms`, hitches > 25 ms; plus CPU % (`perf_hideout.py`).
- [ ] **Step 6:** If a 60 Hz mode is available on this screen, repeat there.

Gate for Task 7: one mode is better on p99 or CPU by more than the spread between two samples of the same mode.

### Task 4: Weak-PC numbers (HD 4000)

**Files:** none.

Needs the user: `TESTER_PASS` set in the shell (never written to a file).

- [ ] **Step 1:** `perf_tester.py deploy`, then `run` for scale 1 and 2, 30 and 60 fps, desktop GL and ES (`PSPRECOMP_GL_API=es`): 8 samples.
- [ ] **Step 2:** Table in the report: fps, GE ms, CPU %. `perf_tester.py clean`.

Expected: we know whether an HD 4000 holds 60 fps at scale 2, and whether it is CPU- or GPU-bound (GE ms vs frame time).
If the password is not given: mark the task blocked in the ledger and go on.

### Task 5: Frame pace in the session stats

**Files:**
- Modify: `runtime/include/net/link_stats.h`, `runtime/src/net/link_stats.cpp`
- Modify: `runtime/src/net/infra_client.cpp:495` (the `link_stats` message)
- Modify: `tools/infra_server.py` (`FRAME_COLUMNS`, `link_stats_selftest`), `tools/beta_summary.py`
- Test: `runtime/tests/test_link_stats.cpp`

**Interfaces — Produces:** `LinkSummary` gains `int frames_over_20 = 0, frames_over_40 = 0;` (frames of 20 ms or more: missed 60 fps; 40 ms or more: missed 30 fps) and `int fps_target = 0, render_scale = 0, gpu_class = 0;` (`gpu_class`: 0 unknown, 1 integrated, 2 dedicated, 3 software; from the adapter list `psp_hw_info` already reads). JSON keys of the same names.

- [ ] **Step 1: failing tests** in `test_link_stats.cpp`: frames at 16, 19, 20, 39, 40, 100 ms give `frames_over_20 == 4`, `frames_over_40 == 2`, `slow_frames == 1`; `reset()` zeroes both; the JSON carries the five keys.
- [ ] **Step 2:** `ctest -R link_stats`. Expected: FAIL (no such member).
- [ ] **Step 3:** Implement. Two more atomics beside `g_slow_frames`, same cap.
- [ ] **Step 4:** Server: the five names join `FRAME_COLUMNS` (columns added in place, as the existing ones are); a report without them is still accepted, one with them out of range is refused. Extend `link_stats_selftest` with both cases first, watch it fail, then implement. `python -I tools/infra_server.py --selftest` PASS.
- [ ] **Step 5:** `beta_summary.py`: one table "pace by GPU class and target": players, share of frames over target, per class.
- [ ] **Step 6:** ctest all green; old-server check: run the client against the unmodified `infra_server.py` of u21 in the lab and confirm the session still closes cleanly. Commit `feat(infra): frame pace and PC class in session stats`.

Deploy is the user's step (server file to the tester PC first, then a beta).

### Task 6: Remove the per-frame Windows string and heap calls

Gate: Task 2 named the caller. Without it, skip and ledger why.

**Files:** the file Task 2 names. Test beside it.

- [ ] **Step 1:** Write the test the cause allows (for a lookup done every frame: the value is read once and a second call does not reach the OS). Watch it fail.
- [ ] **Step 2:** Hoist the call out of the frame (read once at start, as `897cbd8` did for `getenv`). Grep every caller of the touched function first.
- [ ] **Step 3:** ctest, `rt_regress.py` PASS, `perf_hideout.py` x2 before/after. Expected: the named `ntdll` functions leave the top 30; CPU % down by more than the sample spread. If not, revert and ledger.
- [ ] **Step 4:** Commit `perf(runtime): <what was hoisted>`.

### Task 7: Pick the swap policy for `auto`

Gate: Task 3 found a better mode.

**Files:** `runtime/src/psp_event_loop.cpp`; `runtime/tests/test_window_mode.cpp` if the choice depends on the display rate (pure function `int swap_interval_for(int display_hz, PspVsync mode)`, tested for 60, 75, 164 Hz and each mode before it is written).

- [ ] **Step 1:** Test first, watch it fail, implement the rule Task 3's table supports, ctest green.
- [ ] **Step 2:** Re-measure as Task 3 Step 5. Tour once for regress. Commit `feat(display): swap interval chosen from the display rate`.
- [ ] **Step 3:** Launcher option only if the user asks: the setting already works from `settings.ini`.

### Task 8: Keep the pace in the background

**Files:**
- Create: `runtime/src/psp_power_win.cpp`, declared in `runtime/include/psp_runtime.h` as `void psp_power_setup();`
- Modify: `runtime/src/main.cpp` (one call at start), `runtime/CMakeLists.txt`

- [ ] **Step 1: measure first.** 60 fps hideout, 60 s each: focused, behind another window, minimized. Framelog p50/p99 and `[PERF] fps`. Gate: background or minimized is worse than focused. If equal, skip the task and ledger it.
- [ ] **Step 2:** `psp_power_setup()`: `SetProcessInformation(ProcessPowerThrottling)` with `PROCESS_POWER_THROTTLING_EXECUTION_SPEED` and `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` both switched off (Windows 11; a failed call on older Windows is ignored). Non-Windows: empty.
- [ ] **Step 3:** Repeat Step 1. Expected: background p99 within the spread of focused. CPU % focused unchanged.
- [ ] **Step 4:** ctest, commit `fix(win): no power throttling while the game is in the background`.

### Task 9: Start on more PCs

**Part A, #95 (Intel driver crash), no code first:** the user asks the player to set Opciones > Gráficos > Motor gráfico to the ES engine and report. Only if that cures it:

**Files:** `runtime/src/psp_crash_filter` source (the faulting-module line already exists), `runtime/src/psp_event_loop.cpp` (renderer `auto`), test beside `test_window_mode.cpp`.

- [ ] **Step 1:** Pure function `bool gl_driver_crashed_last_run(const std::string& marker_text)` + test (marker with a GL driver module name -> true; empty or other module -> false; a marker written by an ES run -> false, so there is no loop). Watch it fail.
- [ ] **Step 2:** Crash filter writes the marker when the faulting module is the GL driver; `auto` starts in ES once when the marker says so and logs `[RT] GL api: es (driver crashed last run)`. A clean exit deletes the marker.
- [ ] **Step 3:** Lab check with a forced fault (existing crash test hook). ctest green. Commit.

**Part B, #101 (no GL, no ES 3.0), probe only:**

- [ ] **Step 1:** In `tools/gles_probe.cpp` add a `--warp` mode: create the display with `eglGetPlatformDisplayEXT` and `EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE`, print the ES version.
- [ ] **Step 2:** If it reports ES 3.0: start the game that way (temporary env), scale 1, 30 fps, and take `perf_hideout.py`. Record fps and CPU.
- [ ] **Step 3:** Write the result in the report. A playable software mode becomes its own plan; an unplayable one closes #101 as "PC not supported".

### Task 10: Report and review

- [ ] **Step 1:** `build/autopilot/RENDIMIENTO_2026-11-09.md` in Spanish, plain words: what was measured, what changed with before/after, what was skipped and why, what the user must test (.bat per variant in `build/plata/`).
- [ ] **Step 2:** Whole-branch review; update `TRIAGE.md` rows #95 and #101 and the memory note.

## Backlog (not planned here; each needs its own numbers and plan)

Ordered by what the evidence suggests today:

1. `psp_trace_checkpoint` out of release builds (2.1 %) and a per-site cache for `RECOMP_LOOKUP` (3.7 %): emitter changes, all three variants regenerate.
2. `-O3`, LTO or PGO for the generated code: only after Task 2 shows guest code matters in missions.
3. HD-pack PNG decode on a worker thread: only if Task 1 shows `pack_decode_ms` hitches.
4. VBO ring buffer for big battles: only if Task 1 shows `ge_ms` hitches in the mission.
5. Rendering above 60 fps on fast screens (the smooth-animation system blending to the display rate): large, a design of its own.
6. RAM on 4 GB PCs with the HD pack: measure working set with the pack before proposing anything.

Ruled out: `-march=native`/AVX2 builds (breaks old CPUs for a few percent), more guest threads (the game logic is single-threaded by design of the PSP code).
