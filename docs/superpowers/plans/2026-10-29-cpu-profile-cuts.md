# CPU Profile Cuts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the runtime's CPU per frame by removing the two biggest costs the 2026-10-29 profile found, with no visible change.

**Architecture:** (1) The RTT detector (`psp_ge_draw.cpp`, logs only since the RTT plan stopped at Task 3) hashes every GL-written framebuffer once per display list; make it opt-in. (2) `RECOMP_LOOKUP` (emitted `dispatch.cpp`) is a libc++ `std::unordered_map`; emit a flat open-addressing table instead. Old outputs keep their own `RECOMP_LOOKUP`, so the runtime contract does not change.

**Tech Stack:** C++17 runtime (clang/llvm-mingw, ninja, ctest), Rust emitter (`crates/psp-emitter`, cargo test), Python measurement tools in `build/tools/`.

**Spec:** none written. The source is the measured profile `build/tour/perf_prof_s2_60/profile.txt`
(hideout, scale 2, 60 fps, `perf_hideout.py --profile`, 2026-10-29 03:xx) and the user's request
"mide el perfil de CPU aquí, realiza el plan con super-powers". Rulings made without a spec are provisional.

Profile baseline (CPU ≈ 300 ms per wall second ≈ 27-30 % of one core):

| function | % of CPU | ms/s |
|---|---|---|
| `XXH3_hashLong_64b_default` (RTT `rtt_check_cpu_writes`) | 24.9 | 74.5 |
| `RECOMP_LOOKUP` (unordered_map find) | 9.2 | 27.5 |
| `ff_atrac3p_*` + `imdct_half` (audio decode) | ~5 | ~14 |
| `psp_trace_checkpoint` | 3.1 | 9.2 |

## Global Constraints

- No push. Local commits on `ge-batching` in `psprecomp/`, author from repo config ("Patapon Community"), no Co-Authored-By trailer.
- Test instances only on debug port 9998; never touch the user's game on 9999.
- PPSSPP (`tools/ppsspp-src/`) is the reference for GE semantics; nothing here changes GE semantics.
- Code and commits in English; report to the user in Spanish.
- Visual gate: `rt_regress.py build/tour/batch_final2 <new>` must print `RT_REGRESS PASS`.
- Perf gate: `perf_hideout.py TAG --scale 2 --profile` with `PSPRECOMP_PATCHES=60fps`, sampled twice when a change is < 5 %.

## Review Focus

1. `PSPRECOMP_RTT_LOG` unset: `[RTT]` lines disappear, and nothing else may change. `cpu_newer` only feeds `rtt_targets()` (logs). Task 1 Step 4 greps that `rtt_targets`/`cpu_newer` have no other reader.
2. `psp_dispatch_register` after boot (FUNC_ARGS wrappers, HLE stubs, ~44 game hooks) must overwrite an existing key, not add a duplicate. Task 2's emitted self-check covers last-wins for table entries; registration goes through the same insert.
3. Address `0x00000000` must still return `noop_func`, so the empty-slot marker cannot be 0. Task 2 uses `0xFFFFFFFF`, which is never a 4-aligned code address.
4. Overlay addresses (not in the table) must still reach `psp_on_lookup_miss` → `psp_overlay_resolve`. Task 2 keeps the miss call unchanged; the mission tour points exercise it.
5. Table full (more runtime registrations than headroom): it must abort with a message, never loop forever. Task 2 emits headroom of 4096 slots plus a probe-count guard.

---

### Task 1: RTT detector opt-in

**Files:**
- Modify: `runtime/src/psp_ge_draw.cpp` (`ge_draw_prim` lines ~814 and ~824, `ge_draw_note_transfer` log)
- Modify: `docs/ENV_FLAGS.md` (new row `PSPRECOMP_RTT_LOG`)

**Interfaces:**
- Produces: env flag `PSPRECOMP_RTT_LOG=1` turns the detector on (hash per list, `[RTT] sample`, `[RTT] cpu-write`, `[RTT] xfer` lines). Unset: none of it runs.

- [ ] **Step 1: Add `static bool rtt_log_on()`** in `psp_ge_draw.cpp` next to the RTT detector (reads `PSPRECOMP_RTT_LOG` once, `'1'` = on, same idiom as `perf_enabled`). Gate `rtt_check_cpu_writes(rdram)` and `rtt_note_sample(state)` in `ge_draw_prim`, and the `[RTT] xfer` print in `ge_draw_note_transfer`, on it. The `cpu_newer` / `gl_written` bookkeeping stays as is (cheap, and later RTT tasks read it).
- [ ] **Step 2: Build + ctest.** Run: `ninja -C ../build/rtw && ctest --test-dir ../build/rtw` (build/rtw builds the play exe). Expected: build OK, ctest 116/116.
- [ ] **Step 3: Visual gate.** `TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out build/tour/cpu_t1` then `rt_regress.py build/tour/batch_final2 build/tour/cpu_t1`. Expected: `RT_REGRESS PASS`.
- [ ] **Step 4: Readers check.** `grep -n "cpu_newer\|rtt_targets\|guest_hash" runtime/src` lists only the detector and its bookkeeping. Expected: no draw/texture path reads them.
- [ ] **Step 5: Profile.** `PSPRECOMP_PATCHES=60fps python -I build/tools/perf_hideout.py cpu_t1_s2_60 --scale 2 --profile`. Expected: `XXH3_hashLong` < 3 % and `cpu_pct_one_core` ≤ 23 (from 27-29).
- [ ] **Step 6: Commit** `perf(ge): RTT detector only with PSPRECOMP_RTT_LOG=1`.

### Task 2: Flat dispatch table for RECOMP_LOOKUP

Run only if `RECOMP_LOOKUP` is still ≥ 5 % of CPU in Task 1's profile. Otherwise rule it out in the ledger.

**Files:**
- Modify: `crates/psp-emitter/src/dispatch.rs` (`emit_dispatch_table` + its tests)
- Regenerate: `build/p3_output/dispatch.cpp` via `games/patapon3/scripts/p3.sh recompile` (only `dispatch.cpp` text changes; `batch_*.cpp` must be byte-identical: check with `git diff --no-index --stat` against a copy taken first)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: the same three emitted symbols with the same signatures: `void psp_init_dispatch_table()`, `FuncPtr RECOMP_LOOKUP(uint32_t)`, `void psp_dispatch_register(uint32_t, FuncPtr)`. `psp_on_lookup_miss` is called exactly as before.

Emitted table (decided here, not left open):
- `static constexpr uint32_t kDispatchBits` = smallest power of two ≥ 2 × (entries + 4096); `kEmpty = 0xFFFFFFFFu`.
- `static uint32_t s_keys[1u << kDispatchBits]` (filled with `kEmpty` at init), `static FuncPtr s_fns[...]`.
- Slot: `(vaddr >> 2) * 0x9E3779B1u >> (32 - kDispatchBits)`, linear probing.
- Insert overwrites an equal key (last wins, like the map). More than the capacity probes → `fprintf` + `abort()`.
- `psp_init_dispatch_table()` ends with a self-check: every `k_dispatch_entries[i]` whose address does not appear again later in the table must look up to its own `fn`; on failure, print and abort.

- [ ] **Step 1: Write failing Rust tests** in `dispatch.rs`:
  - `dispatch_uses_flat_table`: emitted text contains `kEmpty = 0xFFFFFFFFu` and not `unordered_map`.
  - `dispatch_capacity_has_headroom`: with 10 functions, the emitted `kDispatchBits` is 14 (2 × (11 + 4096) = 8214 → 2^14).
  - Keep `dispatch_contains_noop` updated to the new insert form (`0x00000000U` still maps to `noop_func`).
- [ ] **Step 2: Run** `cargo test -p psp-emitter dispatch`. Expected: the new tests FAIL.
- [ ] **Step 3: Implement** in `emit_dispatch_table`: compute bits in Rust, emit the table, `RECOMP_LOOKUP` probing loop, `psp_dispatch_register` = insert, init loop + self-check.
- [ ] **Step 4: Run** `cargo test -p psp-emitter`. Expected: all PASS.
- [ ] **Step 5: Regenerate + build.** `cargo build --release`, copy `build/p3_output/generated` aside, `p3.sh recompile`, diff the batches (expected: identical), then the runtime build + ctest. Expected: build OK, ctest all pass.
- [ ] **Step 6: Visual gate + profile** as in Task 1 (tags `cpu_t2`, `cpu_t2_s2_60`). Expected: `RT_REGRESS PASS`; `RECOMP_LOOKUP` share at least halved.
- [ ] **Step 7: Commit** `perf(emitter): open-addressing dispatch table for RECOMP_LOOKUP`.

### Task 3: Report

- [ ] **Step 1:** Write `build/autopilot/RENDIMIENTO_CPU_2026-10-29.md` (Spanish): before/after table (hideout s2 60 fps + one mission point), profile top 10 after, what was skipped and why, user checks.
- [ ] **Step 2:** Update memory `p3-ge-batching.md` with the new commits.

Out of scope (record in the report as next candidates if they top the new profile): `psp_trace_checkpoint` per-function-entry cost, ATRAC3+ decode, `win32u` GDI calls inside the GL driver's present.
