# Regenerate Release Variants Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the DxD 1.1.5b (`v115b`) and vanilla EU (`vanilla`) generated outputs up to the current emitter (flat dispatch table, c14a4a8) and rebuild their play exes, with no change in what the player sees.

**Architecture:** `check_fingerprint.py` reports both variants' main output and 3 overlay banks as stale, with `crates/psp-emitter/src/dispatch.rs` as the only changed file. Regenerate each output into a scratch folder with the same commands `p3.sh recompile` / `overlays` run, diff against the current output, and copy only the changed files in (no `rm -rf` of the real output). Then rebuild with `P3_VARIANT=<v> p3.sh build` and A/B the old and new exe of each variant.

**Tech Stack:** Rust `psprecomp` CLI (release build), CMake/ninja, Python tools in `build/tools/`.

**Spec:** none written. Source: the user's request on 2026-10-29 ("haz el plan para Regenerar las variantes, trabajarás con eso") and the pending item from the CPU profile cuts report. Rulings made without a spec are provisional.

## Global Constraints

- Nothing destructive: never `rm -rf` a real output folder; regenerate in the scratchpad and copy changed files only. Back up each variant exe as `psprecomp_runtime.pre_regen.exe` before rebuilding it.
- Do not touch the main release (`build/p3_output`, `build/ovl`, `build/rtw`); its fingerprint is already OK.
- No push. Commits only if a tracked file changes (outputs and build folders are not tracked).
- Test instances on debug port 9998 only; never the user's game on 9999. `python -I build/infra/diskquiet.py 60 900` before each tour.
- A/B tours run at the same hour (the hideout sky follows the real clock; the night reference `batch_final2` does not apply here).

## Review Focus

1. Batches must be byte-identical. Any `batch_*.cpp` or `bank_*.cpp` difference means the variant output was older than its fingerprint said: stop, record what differs, and treat it as a code change (full A/B) instead of a refresh.
2. The variant main output must get the same `--extra-entries` from its own banks' `main_entries.json` (not the main release's), or overlay code reaches LOOKUP_MISS.
3. `PSPRECOMP_CROSS_MID=1` must be set on every recompile, as in `p3.sh` (the fingerprint records `cross_mid=1`).
4. The variant build must configure against its own `PSPRECOMP_BANK_DIRS` (`build/<v>/ovl/...`); a configure that picks the main banks links the wrong overlay code.
5. Vanilla EU has different menus: if `gfx_tour.py` cannot drive it, fall back to a boot smoke (reach the title, no `LOOKUP_MISS` / `[DISPATCH] FATAL` in the log, same as the old exe) and record the ruling.

---

### Task 1: v115b

**Files:** `build/v115b/p3_output/*`, `build/v115b/ovl/ov{Azito,Mission,Title}_output/*` (only files the diff shows changed), `build/v115b/rtw/psprecomp_runtime.exe`.

- [ ] **Step 1: Generate in scratch.** `cargo build --release`; for each bank: `psprecomp recompile data/patapon3/v115b/ov_<lower>/analysis.json --config games/patapon3/game.toml --bank ov<Name> -o <scratch>/ov<Name>_output`; main: `psprecomp recompile data/patapon3/v115b/analysis.json --config games/patapon3/game.toml --extra-entries build/v115b/ovl/ov<Name>_output/main_entries.json (x3) -o <scratch>/p3_output`, all with `PSPRECOMP_CROSS_MID=1`. Expected: 4 outputs, rc 0.
- [ ] **Step 2: Diff.** `git diff --no-index --stat` per output vs the current one. Expected: main differs only in `dispatch.cpp`, `fingerprint.json`, `include/recomp_fingerprint.h`, `recompile_report.json`; banks only in fingerprint files.
- [ ] **Step 3: Copy** the changed files over; `check_fingerprint.py` on all 4. Expected: `fingerprint OK` x4.
- [ ] **Step 4: Build.** Back up the exe, `P3_VARIANT=v115b p3.sh build`. Expected: build OK; `PSPRECOMP_BANK_DIRS` in `build/v115b/rtw/CMakeCache.txt` names `build/v115b/ovl/...`.
- [ ] **Step 5: A/B tour.** Old exe then new exe, `P3DRIVE_DISC0=build/v115b/disc0 P3DRIVE_EXE=../v115b/rtw/<exe> TOUR_RT_PORT=9998 gfx_tour.py --only rt`; `rt_regress.py <old> <new>`. Expected: `RT_REGRESS PASS`; no `LOOKUP_MISS` / `[DISPATCH] FATAL` in either log.

### Task 2: vanilla

Same steps as Task 1 with `vanilla` paths. Step 5 per Review Focus 5 if the tour cannot drive the EU menus.

### Task 3: Report

- [ ] **Step 1:** Short Spanish note at the end of `build/autopilot/RENDIMIENTO_CPU_2026-10-29.md` (variants refreshed, verification), and update memories `p3-v115b.md` / `p3-ge-batching.md` (drop the "needs recompile" item). No changelog line: nothing the player sees changes.
