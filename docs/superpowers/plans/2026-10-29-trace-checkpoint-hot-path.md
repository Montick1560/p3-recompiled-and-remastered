# Trace Checkpoint Hot Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the per-function-entry cost of `psp_trace_checkpoint` (2.9 % of process CPU, 13.8 ms/s in the 2026-10-29 profile) with no change to any diagnostic it feeds.

**Architecture:** Every generated function calls `psp_trace_checkpoint(addr)`. Its disassembly (build/rtw, 2026-10-29) shows two costs on the always-taken path: a `lock xadd` on the cross-thread ring position, and a prologue that saves 8 registers + 0x88 bytes of stack because the rarely-on diagnostics (first-call env init, FUNC_WATCH, FUNC_CENSUS, PC_TRACE, socket `K` backtrace) are inlined into it. Move those into `noinline` cold functions behind one flag, and claim the shared-ring slot with a relaxed load + store instead of `fetch_add`.

**Tech Stack:** C++17 runtime (clang 22 / llvm-mingw, ninja, ctest), Python tools in `build/tools/`.

**Spec:** none written. The source is the profile `build/tour/perf_cpu_t2_b/profile.txt` and the user's go-ahead on 2026-10-29 ("Va, ahora sí ¿Qué sigue en el plan?") for the candidate proposed earlier. Rulings made without a spec are provisional.

## Global Constraints

- No push. Local commit on `ge-batching` in `psprecomp/`, author from repo config, no Co-Authored-By trailer; `Verified:` / `Agent:` lines.
- Do not touch the play exe `build/rtw/psprecomp_runtime.exe` until every gate passes (the user is about to run perf tests with it). Build in `build/rtw_trace` (same cache values as `build/rtw`) and run tools with `P3DRIVE_EXE=../rtw_trace/psprecomp_runtime.exe`.
- Test instances on debug port 9998 only; never touch the user's game on 9999.
- Names and types that other files `extern` stay as they are: `g_last_func_addr`, `g_prev_func_addr`, `g_func_ring[32]`, `g_func_ring_pos` (thread_local), `PspThread::last_func`, `psp_dispatch_get_recent_funcs`.
- Run `python -I build/infra/diskquiet.py 60 900` before any timing run.

## Review Focus

1. `PSPRECOMP_FUNC_WATCH=<hex>` still prints `[FUNC-WATCH] 0x... from 0x...; this thread's path: ...` with the caller and path from *before* this entry is recorded (the cold call must run before the hot updates). Task 1 Step 5 checks it live.
2. Debug socket `I`: `recent_funcs` is non-empty and every thread row has a non-zero `last_func`. Task 1 Step 5.
3. Debug socket `K <tid>`: the named thread still prints its host backtrace at its next function entry. Task 1 Step 5.
4. `PSPRECOMP_PC_TRACE=1` and `PSPRECOMP_FUNC_CENSUS=1` still turn on their paths: the cold flag must be computed after the env init, never left `false` before init has run.
5. Two host threads entering functions at once may now write the same shared-ring slot (one entry lost). Acceptable: the ring is documented as approximate, diagnostics only; no reader indexes by count.

---

### Task 1: Lean `psp_trace_checkpoint`

**Files:**
- Modify: `runtime/src/psp_dispatch.cpp` (`psp_trace_checkpoint` and the statics above it, lines ~324-445; ring comment at ~37-43)

**Interfaces:**
- Produces: same exported symbol `void psp_trace_checkpoint(uint32_t addr)`, same observable diagnostics.
- New file-local: `static bool g_trace_slow = true;` (true until the first-call init, then `g_pc_trace || g_func_watch_count > 0 || g_census`), `[[gnu::cold, gnu::noinline]] static void trace_slow_path(uint32_t addr)` (init, FUNC_WATCH, census, PC_TRACE count/print), `[[gnu::cold, gnu::noinline]] static void trace_backtrace(PspThread* self, uint32_t addr)` (the `K` request).

- [ ] **Step 1: Baseline.** `build/rtw_trace` configured with the `build/rtw` cache values; `ninja -C build/rtw_trace psprecomp_runtime`. Expected: builds (baseline exe for the A/B is `build/rtw/psprecomp_runtime.exe`, same commit).
- [ ] **Step 2: Implement** the split. Hot path order: `if (g_trace_slow) trace_slow_path(addr);` then prev/last update, `self->last_func`, `K` check calling `trace_backtrace`, per-thread ring, shared ring via `pos.load(relaxed)` / `pos.store(pos+1, relaxed)` / slot store.
- [ ] **Step 3: Build + ctest.** `ninja -C build/rtw_trace && ctest --test-dir build/rtw_trace`. Expected: 116/116.
- [ ] **Step 4: Disassembly check.** `objdump -d -C` of `psp_trace_checkpoint` in the new exe. Expected: no `lock` prefix, no `getenv`/`fprintf` call inline, prologue pushes ≤ 2 registers.
- [ ] **Step 5: Live diagnostics smoke** (port 9998, new exe, `PSPRECOMP_FUNC_WATCH` set to one hot address from the profile run): boot, send `I`, send `K <a thread id from I>`. Expected: `[FUNC-WATCH]` lines in the log, non-empty `recent_funcs`, non-zero `last_func`, a host backtrace in the log after `K`.
- [ ] **Step 6: Visual gate.** `TOUR_RT_PORT=9998 P3DRIVE_EXE=../rtw_trace/psprecomp_runtime.exe python -I build/tools/gfx_tour.py --only rt --out build/tour/trace_t1` then `rt_regress.py build/tour/batch_final2 build/tour/trace_t1`. Expected: `RT_REGRESS PASS`.
- [ ] **Step 7: Profile A/B** (diskquiet first, `PSPRECOMP_PATCHES=60fps`, `perf_hideout.py TAG --scale 2 --profile`, twice per exe). Expected: `psp_trace_checkpoint` ms/s at least halved (13.8 → ≤ 7) and process Gcycles/s not higher than the baseline runs.
- [ ] **Step 8: Commit** `perf(runtime): keep psp_trace_checkpoint's hot path lean`. Then copy the exe into `build/rtw` only after telling the user (play exe swap).

### Task 2: Report

- [ ] **Step 1:** Append a section to `build/autopilot/RENDIMIENTO_CPU_2026-10-29.md` (Spanish): before/after, what the user should check.
- [ ] **Step 2:** Update memory `p3-ge-batching.md`.
