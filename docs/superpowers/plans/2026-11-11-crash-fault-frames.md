# Crash Fault Frames Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The next field crash like beta #94/#99 (jump to address 0 on a host thread, `last_guest_func=0`) logs who called address 0, so the cause can be found.

**Architecture:** The crash filter's `[BT] crash:` line walks the filter's own stack (useless: `+b77404` = psp_crash_filter then system frames). Add `psp_fault_frames(const CONTEXT*, ...)` that unwinds the *faulting* context (RtlLookupFunctionEntry/RtlVirtualUnwind; a frame without unwind data, such as address 0, pops the return address at Rsp) and prints each frame as `module+offset`. The filter logs it as `[BT] fault: ...`.

**Tech Stack:** C++17, Win32 (MinGW clang), ctest.

**Spec:** beta reports #94 (Soy_pinguino, 1.1.5b) and #99 (caterpilar, vanilla-eu): `[CRASH] exception 0xC0000005 at 0 ... module=? access=8 addr=0x0 last_guest_func=0x00000000`, `[BT] crash: +b77404 +70ab0c507 ...` (build/beta/reportes_2026-11-09/copia). Memory note: "print the return address at the faulting Rsp when the fault address is 0".

## Global Constraints

- Diagnostics only: no behavior change outside the crash filter.
- No heap allocation in the filter path (the heap may be what broke); fixed buffers only.
- Never read outside the faulting thread's stack (GetCurrentThreadStackLimits); at most 16 frames.
- Commits by "Patapon Community <rexgamer1560@gmail.com>", no co-author trailer.

## Review Focus

1. Null call (Rip = 0): first frame `?`, second = the caller in exe (covered: `fault_frames_find_the_caller_of_a_null_call`).
2. Garbage Rsp outside the stack: stop, never fault inside the filter (covered: `fault_frames_stop_at_a_bad_stack`).
3. Tiny output buffer: cut and terminated (covered: `fault_frames_short_buffer`).
4. Frames in system DLLs / driver DLLs: named by module, not as exe offsets (psp_module_of, already tested).
5. Recursion: a fault inside psp_fault_frames itself — prevented by 2; the `[CRASH]` line is printed before it so it is never lost.

---

### Task 1: `psp_fault_frames` + `[BT] fault:` line

**Files:**
- Modify: `runtime/src/psp_backtrace.cpp` (new function after `psp_module_of`), `runtime/src/main.cpp:80-106` (declare + call in `psp_crash_filter` after the `[CRASH]` line), `runtime/tests/test_backtrace.cpp`, `DEBUGGING.md` (§ Guest backtraces: one paragraph on `[BT] fault:`)

**Interfaces:**
- Produces: `void psp_fault_frames(const void* context, char* out, size_t size);` — `context` is a `const CONTEXT*` on Windows (no-op writing "" elsewhere); `out` = space-separated `psp_module_of` names of up to 16 frames starting at the faulting Rip.

- [ ] **Step 1: Write the failing tests** (Windows-only block in `test_backtrace.cpp`)
  - `fault_frames_find_the_caller_of_a_null_call`: a vectored handler, on ACCESS_VIOLATION with Rip == 0, calls `psp_fault_frames(ctx, buf, sizeof buf)`, then simulates the return (`Rip = *(u64*)Rsp; Rsp += 8`) and continues. A noinline function calls a `volatile` null function pointer. Assert: `buf` starts with `"? exe+0x"`; the second frame's offset is within [offset of that function, +0x100); at least 3 frames.
  - `fault_frames_stop_at_a_bad_stack`: a CONTEXT with Rip = 0 and Rsp = 0x10 → output is exactly `"?"`.
  - `fault_frames_short_buffer`: the first test's context into `char tiny[4]` → strlen 3.
- [ ] **Step 2:** `ninja -C ../rtw_gestall test_backtrace && ../rtw_gestall/test_backtrace` → link error / FAIL.
- [ ] **Step 3:** Implement: copy the context; loop ≤ 16: append name of Rip; `RtlLookupFunctionEntry(Rip)` → `RtlVirtualUnwind(UNW_FLAG_NHANDLER, ...)`, else pop `[Rsp]` (only if `Rsp` and `Rsp+8` lie within the current thread's stack limits); stop when Rip is 0 after a step or Rsp leaves the stack. Call it in `psp_crash_filter` into a 1 KB stack buffer and print `[BT] fault: <frames>`.
- [ ] **Step 4:** test_backtrace passes; `ninja -C ../rtw_gestall && ctest` (in rtw_gestall) → 137/137.
- [ ] **Step 5:** Commit `feat(runtime): crash filter logs the faulting thread's frames ([BT] fault)`.
