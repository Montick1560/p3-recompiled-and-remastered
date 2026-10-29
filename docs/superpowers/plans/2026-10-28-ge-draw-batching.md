# GE Draw Batching + Automatic Render Scale Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge consecutive same-state GE PRIMs into one GL draw so modest PCs (Intel HD 4000 on `tester`) reach 60 fps at render scale 2, and make the launcher pick scale 4 (dedicated GPU) or 2 (integrated) until the player chooses.

**Architecture:** `ge_draw_prim` keeps decoding/transforming per PRIM on the CPU but appends triangles to a pending batch; a new `ge_draw_flush()` does today's upload + GL state + draw once per batch. The GE command loop flushes before any command that changes draw-time state (simple rule: action commands always, CPU-only commands never, everything else when its 24-bit word changes). The launcher detects the GPU with DXGI and writes `render_scale` unless a `[launcher] render_scale_chosen` marker exists.

**Tech Stack:** C++17 runtime (SDL2 + OpenGL 3.3, ninja, ctest), Python 3 tools in `build/tools/`, Rust/Tauri 2 launcher (`windows` 0.62.2 crate already in Cargo.lock), Vitest.

**Spec:** `psprecomp/docs/superpowers/specs/2026-10-28-ge-draw-batching-design.md`

## Global Constraints

- Picture must be identical at the same render scale; when unsure whether a command needs a flush, flush.
- PPSSPP (`tools/ppsspp-src/`) is the reference; no invented GE semantics.
- GL calls only on the render thread; no game-specific code in `runtime/src`; keep ARCHITECTURE.md and `docs/ENV_FLAGS.md` true in the same commit.
- Ponytail: smallest complete change, reuse existing helpers, no unrequested abstraction, unit test for new non-trivial pure logic, `shortcut: <limit>, <when to upgrade>` comments.
- Kill switch `PSPRECOMP_GE_BATCH=0` = one draw per PRIM.
- Commits: author "Patapon Community" (repo config already set), no Claude trailer, English messages. psprecomp branch `ge-batching` (exists); launcher branch `auto-scale` from `beta-1`. **No push.**
- `tester` (192.168.194.59): only `C:\Users\tester\Documents\p3-perf\`, **≤ 2.9 GB total including logs**, debug port 9998, kill only our PID, never stop/reboot it or touch `patapon-server\`; over budget → measure on this PC instead. The tester password comes from the `TESTER_PASS` environment variable of the shell, never from a file.
- Local runs: debug port 9998 only; the user's game (9999) is never touched.
- Gates per runtime task: `cd C:/Users/torso/Documents/deco/build/rtw && ninja && ctest --output-on-failure` green; `rt_regress.py` PASS; perf numbers recorded.

## Review Focus

1. Mid-list texture content change: `LOADCLUT` / `TRXKICK` / `TEXFLUSH` repeated with an identical word but new memory — must still flush (always-flush set), or palettes/fonts go stale.
2. Strip winding with culling on: odd strip triangles must keep the facing GL would give them — a wrong order makes sprites vanish when `CULLFACEENABLE` is on.
3. A list that stalls mid-batch and resumes later (`psp_render_queue.cpp`): pending vertices must be drawn before the return, never carried into another frame/target.
4. Render-target switch / block transfer / clear between two same-state PRIMs: the first PRIM must reach the old target before the switch.
5. Debug tools (`PSPRECOMP_DRAW_LOG`, skip ranges, `TEX_WATCH`, `VTX_WATCH`): with batching they must still describe single draws — batching is off while any is active.

---

### Task 1: Local baseline (before any code)

**Files:** none (results in `build/tour/`).

- [ ] **Step 1: Hideout perf baseline, scale 1, 2 and 2 with the 60 fps set**

Run (Git Bash, `C:/Users/torso/Documents/deco`):
```
python -I build/tools/perf_hideout.py batch_antes_s1
python -I build/tools/perf_hideout.py batch_antes_s2 --scale 2
PSPRECOMP_PATCHES=60fps python -I build/tools/perf_hideout.py batch_antes_s2_60 --scale 2
```
Expected: three `build/tour/perf_batch_antes_*/perf.json`, each with `draws/frame` ≈ 1 400.

- [ ] **Step 2: Visual baseline tour**

Run: `TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/batch_base`
Then: `python -I build/tools/rt_regress.py build/tour/relevo_base build/tour/batch_base`
Expected: `RT_REGRESS PASS` (proves the baseline itself is sane; later tasks compare against `batch_base`).

### Task 2: Remote perf harness for `tester` + tester baseline

**Files:**
- Create: `build/tools/perf_remote.py` (runs ON tester; stdlib only — no PIL, nothing installed)
- Create: `build/tools/perf_tester.py` (runs here; deploys, schedules, collects; PIL allowed)
- Results: `build/tour/tester_<tag>/`

**Interfaces:**
- Produces: `python -I build/tools/perf_tester.py deploy`, `... run TAG [--scale S] [--fps60] [--batch 0|1] [--seconds N]`, `... --diff TAG_A TAG_B`, `... clean`. Result file `build/tour/tester_<tag>/perf.json` with keys `fps, ge_ms/frame, prims/frame, draws/frame, binds/frame, cpu_pct_one_core, hardware, landed, seconds, scale, fps60, batch, when` (same key names as `perf_hideout.py` where they overlap) plus `hideout.png`.

- [ ] **Step 1: Find what the exe needs at run time**

Copy `build/rtw/psprecomp_runtime.exe` + `*.dll` to a scratch folder, run it there with `PSPRECOMP_DISC0=C:/Users/torso/Documents/deco/disc0`, `PSPRECOMP_SAVEDATA=<copy of build/tour_save>`, `PSPRECOMP_DEBUG_PORT=9998`, `PSPRECOMP_PERF=1`; add files from `build/rtw` until it reaches the title screen. Record the final file list as `DEPLOY_FILES` in `perf_tester.py`. Expected deploy size ≈ 0.6–0.8 GB (disc0 is 506 MB).

- [ ] **Step 2: Write `perf_remote.py`**

Reads `run.json` next to it (`scale, fps60, batch, seconds`), launches the exe from its own folder with the env above plus `PSPRECOMP_RENDER_SCALE`, `PSPRECOMP_PATCHES=60fps` when `fps60`, `PSPRECOMP_GE_BATCH`, `PSPRECOMP_OSK_TEXT=Hero`, stdout+stderr to `game.log`. Boot keys = `gfx_tour.RtSide.boot` (wait for `DATAMS.BND`, 8 s, `D`×5, `X`, `X`, `START`, `D`, `X` with the same waits) over the debug socket (`B <mask> 120`, masks from `p3drive.MASKS`). Then `land()` as in `gfx_tour.py` but classifying with stdlib: decode the `S <path>` TGA (`gfx_tour.RtSide.frame` layout), downsample to 60×34 RGB, nearest of `refs.json` by mean absolute difference (%, same thresholds 20/25). Then sample `seconds` of `[PERF]` lines, take `hideout.tga`, read process CPU with `ctypes` `GetProcessTimes`, kill **only** its own child PID, write `result.json` and `done.flag`. Safety: kill the game and write `result.json` with `"error"` if `game.log` exceeds 300 MB or the run passes 15 min.

- [ ] **Step 3: Write `perf_tester.py`**

`deploy`: build `refs.json` from `build/tools/gfx_ref/*.png` with PIL (same 60×34 downsample, names as `gfx_tour.REFS`), then `robocopy` `DEPLOY_FILES`, `disc0`, `tour_save` (as `savedata_src`), `perf_remote.py`, `refs.json` to `\\192.168.194.59\c$\Users\tester\Documents\p3-perf\`. Refuse (exit 2) if the remote folder would exceed 2.9 GB — measure with `os.walk` over the share before and after.
`run`: refuse if folder > 2.9 GB; write `run.json`; fresh copy of `savedata_src` → `savedata` remotely; `schtasks /create /s 192.168.194.59 /u tester /p %TESTER_PASS% /tn p3perf /sc once /st 23:59 /it /ru tester /rp %TESTER_PASS% /f /tr "<python> -I C:\Users\tester\Documents\p3-perf\perf_remote.py"` (python = `C:\Users\tester\AppData\Local\Microsoft\WindowsApps\python.exe`), `schtasks /run`, poll `done.flag` over SMB every 10 s (timeout 20 min), copy `result.json` + `hideout.tga` back, convert to `perf.json` + `hideout.png`, delete remote `game.log`, `hideout.tga`, `done.flag`, `schtasks /delete /f`. `--diff`: same table as `perf_hideout.py --diff`. `clean`: delete the whole remote `p3-perf\` folder.

- [ ] **Step 4: Dry run the remote script locally**

Run `perf_remote.py` on this PC against a local copy of the deploy folder (port 9998). Expected: `result.json` with `landed` = `cuartel` and ~20 `[PERF]` samples.

- [ ] **Step 5: Deploy and run the tester baseline (current exe, batching does not exist yet)**

```
python -I build/tools/perf_tester.py deploy
python -I build/tools/perf_tester.py run base_s2 --scale 2
python -I build/tools/perf_tester.py run base_s2_60 --scale 2 --fps60
python -I build/tools/perf_tester.py run base_s4_60 --scale 4 --fps60
```
Expected: three `perf.json` with `hardware` naming the GPU (confirm HD 4000) and `landed` = `cuartel`; `hideout.png` shows the hideout. If the scheduled task cannot start in the interactive session, stop using tester (note it in the report) and continue locally.

### Task 3: Pure batching helpers

**Files:**
- Create: `psprecomp/runtime/include/psp_ge_batch.h` (header-only, no GL)
- Create: `psprecomp/runtime/tests/test_ge_batch.cpp`
- Modify: `psprecomp/runtime/CMakeLists.txt` (register like `test_gl_shadow`: executable, include dir, `-O2 -Wall -Wextra -Wno-unused-parameter`, `add_test(NAME ge_batch_tests COMMAND test_ge_batch)`)

**Interfaces:**
- Produces:
  - `bool ge_cmd_flushes(uint8_t cmd, bool word_changed)` — true for the always-flush set regardless of `word_changed`; false for the CPU-only set; otherwise `word_changed`.
  - `void ge_triangle_list_indices(int prim_type, size_t count, std::vector<uint32_t>& out)` — appends triangle-list indices into a source of `count` vertices for `GE_PRIM_TRIANGLES`, `GE_PRIM_TRIANGLE_STRIP`, `GE_PRIM_TRIANGLE_FAN` (constants from `psp_ge_constants.h`); incomplete trailing triangles are dropped.

- [ ] **Step 1: Write the failing tests** (CHECK macro style of `test_gl_shadow.cpp`)

```cpp
// flush rule
CHECK(ge_cmd_flushes(GE_CMD_LOADCLUT, false), "LOADCLUT flushes even with the same word");
CHECK(ge_cmd_flushes(GE_CMD_TRXKICK, false), "block transfer always flushes");
CHECK(ge_cmd_flushes(GE_CMD_FINISH, false) && ge_cmd_flushes(GE_CMD_END, false) && ge_cmd_flushes(GE_CMD_SIGNAL, false), "list control flushes");
CHECK(!ge_cmd_flushes(GE_CMD_PRIM, true), "PRIM is what gets batched");
CHECK(!ge_cmd_flushes(GE_CMD_VADDR, true) && !ge_cmd_flushes(GE_CMD_WORLDMATRIXDATA, true), "CPU-only state never flushes");
CHECK(ge_cmd_flushes(GE_CMD_TEXADDR0, true) && !ge_cmd_flushes(GE_CMD_TEXADDR0, false), "draw-time state flushes on change only");
CHECK(ge_cmd_flushes(GE_CMD_ALPHABLENDENABLE, true) && ge_cmd_flushes(GE_CMD_SCISSOR1, true), "blend and scissor are draw-time state");
// list conversion
std::vector<uint32_t> i;
ge_triangle_list_indices(GE_PRIM_TRIANGLE_STRIP, 5, i);
CHECK(i == std::vector<uint32_t>({0,1,2, 2,1,3, 2,3,4}), "strip: odd triangles swap their first two vertices");
i.clear(); ge_triangle_list_indices(GE_PRIM_TRIANGLE_FAN, 5, i);
CHECK(i == std::vector<uint32_t>({0,1,2, 0,2,3, 0,3,4}), "fan pivots on vertex 0");
i.clear(); ge_triangle_list_indices(GE_PRIM_TRIANGLES, 7, i);
CHECK(i == std::vector<uint32_t>({0,1,2, 3,4,5}), "list drops the incomplete tail");
i.clear(); ge_triangle_list_indices(GE_PRIM_TRIANGLE_STRIP, 2, i);
CHECK(i.empty(), "a strip shorter than 3 adds nothing");
```
Strip odd order: check it against PPSSPP `GPU/Common/IndexGenerator.cpp` (AddStrip) before relying on the expected vector above; if PPSSPP's odd order differs, use PPSSPP's and fix the test (both keep the facing).

- [ ] **Step 2: Run to verify it fails**

Run: `cd C:/Users/torso/Documents/deco/build/rtw && ninja test_ge_batch` — Expected: compile error (header missing).

- [ ] **Step 3: Implement `psp_ge_batch.h`**

Always-flush set: `LOADCLUT, TRXKICK, TEXFLUSH, TEXSYNC, BEZIER, SPLINE, FINISH, END, SIGNAL, CLEARMODE`. CPU-only set (each must be justified by a grep showing its `GeState` field is read only from `ge_decode_vertices` / `ge_transform_vertices` / lighting, never in `psp_ge_draw.cpp` draw-time code, `psp_ge_shader.cpp` or `psp_ge_texture.cpp`): `NOP, 0xFF, PRIM, BASE, OFFSETADDR, ORIGIN, VADDR, IADDR, VERTEXTYPE` (its through bit is handled by the batch key in Task 4), `JUMP, CALL, RET, BJUMP`, world/view/proj/bone/texgen matrix NUMBER and DATA, `MORPHWEIGHT0..7`, `TEXSCALEU/V, TEXOFFSETU/V`, and light/material commands only if lighting is applied on the CPU. Anything not provably CPU-only stays out of the set. One line comment naming PPSSPP `GPUCommonHW.cpp` as the source of the draw-time list.

- [ ] **Step 4: Run tests** — `ninja && ctest -R ge_batch --output-on-failure` → PASS.

- [ ] **Step 5: Commit**

```
cd C:/Users/torso/Documents/deco/psprecomp
git add runtime/include/psp_ge_batch.h runtime/tests/test_ge_batch.cpp runtime/CMakeLists.txt
git commit -m "feat(ge): pure flush rule and triangle-list indices for draw batching"
```
(+ `Verified: ninja OK, ctest N/N` line.)

### Task 4: Batch PRIMs in the renderer

**Files:**
- Modify: `psprecomp/runtime/src/psp_ge_draw.cpp` (`ge_draw_prim` :689-1199, `ge_draw_select_target` :355, `ge_draw_note_transfer` :673, `ge_draw_end_list` :534)
- Modify: `psprecomp/runtime/include/psp_ge_draw.h` (declare `ge_draw_flush`)
- Modify: `psprecomp/runtime/src/psp_ge.cpp` (command loop :200, every `return` of the list processor)
- Modify: `psprecomp/docs/ENV_FLAGS.md`, `psprecomp/ARCHITECTURE.md` (renderer row)

**Interfaces:**
- Consumes: `ge_cmd_flushes`, `ge_triangle_list_indices` (Task 3).
- Produces: `void ge_draw_flush();` — draws the pending batch (no-op when empty); safe to call anywhere on the render thread.

- [ ] **Step 1: Split `ge_draw_prim` into append + `ge_draw_flush`**

Per PRIM (append, unchanged code): counters, drawlog, RTT checks/notes, target bookkeeping, skip ranges, clear handling, decode, `draw_max_v`, snapping, transform, line/rectangle expansion, packing. New: triangle-class PRIMs (TRIANGLES/STRIP/FAN, rectangles, through-mode lines) append to a file-static `std::vector<PackedVertex> g_batch` via `ge_triangle_list_indices` (rectangles/lines are already lists), and record `g_batch_through` (bool) and `g_batch_max_v` (max). Before appending: if the batch is non-empty and `through` differs → `ge_draw_flush()`. Moved into `ge_draw_flush()`: scissor, viewport/depth range, VBO upload of `g_batch`, shader + uniforms, texture bind (with `g_batch_max_v`), colour mask, blend, depth, stencil, cull, `glDrawArrays(GL_TRIANGLES, ...)`, `g_perf.draw_calls`, then `g_batch.clear()` (capacity kept). Clear-mode PRIMs, non-through lines/line strips and points: `ge_draw_flush()` first, then draw immediately as today (batch-of-one through the same code). After appending, if batching is off → `ge_draw_flush()` at once.

- [ ] **Step 2: Batching on/off**

`static bool batching_on()`: false when `PSPRECOMP_GE_BATCH=0` (read once), or while `g_drawlog_lists > 0`, a skip range is set (`g_skip_lo <= g_skip_hi`), `PSPRECOMP_TEX_WATCH` or `PSPRECOMP_VTX_WATCH` is set.

- [ ] **Step 3: Flush points**

`psp_ge.cpp` command loop: keep `static uint32_t g_last_word[256]`; before the `switch`, `if (ge_cmd_flushes(cmd, word != g_last_word[cmd])) ge_draw_flush(); g_last_word[cmd] = word;`. Call `ge_draw_flush()` before every `return` of the list processor (end, stall, finish, signal, command limit). Also at the top of `ge_draw_select_target`, `ge_draw_note_transfer`, `ge_draw_end_list`, `ge_present_frame`, `ge_draw_present_if_stale` and screenshot capture.

- [ ] **Step 4: Build + unit tests**

Run: `cd C:/Users/torso/Documents/deco/build/rtw && ninja && ctest --output-on-failure` → all green.

- [ ] **Step 5: Visual gate, batching on and off**

```
TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/batch_on
python -I build/tools/rt_regress.py build/tour/batch_base build/tour/batch_on
PSPRECOMP_GE_BATCH=0 TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/batch_off
python -I build/tools/rt_regress.py build/tour/batch_base build/tour/batch_off
```
Expected: both `RT_REGRESS PASS`. Also compare the hideout frame pixel by pixel at scale 2: `perf_hideout.py batch_on_s2 --scale 2` vs `PSPRECOMP_GE_BATCH=0 perf_hideout.py batch_off_s2 --scale 2` `hideout.png` — Expected: max channel difference 0 (if not 0, find the first differing draw with `PSPRECOMP_DRAW_LOG` before going on).

- [ ] **Step 6: Perf here**

`perf_hideout.py batch_on_s1`, `batch_on_s2 --scale 2`, `PSPRECOMP_PATCHES=60fps ... batch_on_s2_60 --scale 2`, then `--diff` each against Task 1. Expected: `draws/frame` well below 1 400 (target: a few hundred or less); `ge_ms/frame` not higher than before. Mission (best effort): `gfx_tour.py` runs with `PSPRECOMP_PERF=1`, so compare the `[PERF]` lines logged during its mission checkpoints in `batch_base` vs `batch_on` logs. Record the numbers in the commit message.

- [ ] **Step 7: Docs + commit**

`ENV_FLAGS.md`: `PSPRECOMP_GE_BATCH` (default on; `0` = one draw per PRIM; automatically off while draw-log / skip / watch tools run). ARCHITECTURE.md renderer row: one sentence on batching and its flush rule.
```
git add runtime/src/psp_ge_draw.cpp runtime/include/psp_ge_draw.h runtime/src/psp_ge.cpp docs/ENV_FLAGS.md ARCHITECTURE.md
git commit -m "perf(ge): batch consecutive same-state PRIMs into one draw"
```
(+ `Verified: ninja OK, ctest N/N, rt_regress PASS on/off, hideout pixel-identical, draws/frame A->B`.)

### Task 5: Measure on `tester` after batching (+ conditional ring buffer)

**Files:** results only; conditionally `psprecomp/runtime/src/psp_ge_draw.cpp`.

- [ ] **Step 1: Redeploy the new exe and run the matrix**

`perf_tester.py deploy` (exe + dlls only change), then runs `after_s2`, `after_s2_60 --fps60`, `after_s4_60 --scale 4 --fps60`, and `after_s2_60_off --fps60 --batch 0`; `--diff` against Task 2.
Expected: `after_s2_60` fps ≥ 58 is success. Write the table to `build/autopilot/RENDIMIENTO_2026-10-29.md` (Spanish, for the user).

- [ ] **Step 2: Conditional — only if `after_s2_60` fps < 58 AND its `ge_ms/frame` > 8**

Replace the per-flush `glBufferData` with a 4 MiB `GL_ARRAY_BUFFER` ring: `glMapBufferRange(offset, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT)`, orphan with `glBufferData(..., nullptr, GL_STREAM_DRAW)` when the next batch does not fit, draw with the batch's first vertex offset. Gate again with Task 4 Steps 4–6 and re-run Step 1. If the condition is false, note in the report that the ring buffer was not needed and why; if fps is still < 58 after it, name the measured next bottleneck in the report (CPU % vs GPU) and stop — shaders/ANGLE are a later design.

### Task 6: Run the game on the dedicated GPU of hybrid laptops

**Files:** Modify `psprecomp/runtime/src/main.cpp` (file scope, `#ifdef _WIN32`).

- [ ] **Step 1: Add the two exports**

`extern "C" __declspec(dllexport) unsigned long NvOptimusEnablement = 1;` and `extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;` with a one-line why comment.

- [ ] **Step 2: Verify**

Run: `ninja && objdump -p C:/Users/torso/Documents/deco/build/rtw/psprecomp_runtime.exe | grep -E "NvOptimusEnablement|AmdPowerXpress"` → both names listed. `ctest` green; the game still reaches the hideout (`perf_hideout.py hybrid_check`).

- [ ] **Step 3: Commit** — `git commit -m "feat(rt): ask hybrid laptops for the dedicated GPU"`.

### Task 7: Launcher GPU detection

**Files:**
- Create: `launcher/src-tauri/src/gpu.rs`
- Modify: `launcher/src-tauri/src/lib.rs` (`mod gpu;`), `launcher/src-tauri/Cargo.toml` (`[target.'cfg(windows)'.dependencies] windows = { version = "0.62.2", features = ["Win32_Graphics_Dxgi"] }` — already in Cargo.lock, no new crate)

**Interfaces:**
- Produces: `pub fn auto_render_scale() -> u32` (4 or 2) and the pure `pub fn scale_for(adapters: &[(u32, u64, bool)]) -> u32` (vendor id, dedicated video memory bytes, is_software).

- [ ] **Step 0: Branch** — `cd C:/Users/torso/Documents/deco/launcher && git switch -c auto-scale beta-1`.

- [ ] **Step 1: Failing tests in `gpu.rs`**

```rust
#[test] fn integrated_only_gets_2() { assert_eq!(scale_for(&[(0x8086, 128 << 20, false)]), 2); }
#[test] fn dedicated_gets_4() { assert_eq!(scale_for(&[(0x8086, 128 << 20, false), (0x10DE, 4 << 30, false)]), 4); }
#[test] fn two_gib_counts_as_dedicated() { assert_eq!(scale_for(&[(0x1002, 2 << 30, false)]), 4); }
#[test] fn software_adapter_ignored() { assert_eq!(scale_for(&[(0x1414, 8 << 30, true)]), 2); }
#[test] fn no_adapters_gets_2() { assert_eq!(scale_for(&[]), 2); }
```

- [ ] **Step 2: Run** — `cd launcher/src-tauri && cargo test gpu` → fails to compile.

- [ ] **Step 3: Implement**

`scale_for`: 4 if any non-software adapter has ≥ 2 GiB, else 2, with the comment `// shortcut: an APU with >= 2 GiB reserved counts as dedicated, upgrade to a D3D12 UMA check if players report it`. `auto_render_scale` (Windows): `CreateDXGIFactory1::<IDXGIFactory1>()`, `EnumAdapters1(i)` until error, `GetDesc1()` → `(VendorId, DedicatedVideoMemory as u64, Flags & DXGI_ADAPTER_FLAG_SOFTWARE != 0)`; any error → 2. Non-Windows: 2.

- [ ] **Step 4: Run** — `cargo test` → all PASS. Print `auto_render_scale()` once from a test marked `#[ignore]` and run it here: expected 4 (RTX 3050).

- [ ] **Step 5: Commit** — `git add src/gpu.rs src/lib.rs Cargo.toml Cargo.lock && git commit -m "feat: detect a dedicated GPU with DXGI"`.

### Task 8: Launcher writes the detected scale until the player chooses

**Files:**
- Modify: `launcher/src-tauri/src/settings.rs`, `launcher/src-tauri/src/lib.rs` (`save_settings` :38, `.setup` :510)
- Modify: `psprecomp/docs/contracts/settings-v1.md`

**Interfaces:**
- Consumes: `gpu::auto_render_scale()` (Task 7), `settings::save`, `settings::launcher_get/launcher_set`.
- Produces: `pub const SCALE_CHOSEN_KEY: &str = "render_scale_chosen";` and `pub fn apply_auto_render_scale(root: &Path, scale: u32) -> Result<(), String>` in `settings.rs`.

- [ ] **Step 1: Failing tests in `settings.rs`**

```rust
#[test] fn auto_scale_written_when_not_chosen() {
    let d = root(); load(d.path()).unwrap();
    apply_auto_render_scale(d.path(), 2).unwrap();
    assert_eq!(load(d.path()).unwrap()["display.render_scale"], "2");
}
#[test] fn auto_scale_respects_a_choice() {
    let d = root(); load(d.path()).unwrap();
    launcher_set(d.path(), SCALE_CHOSEN_KEY, "true").unwrap();
    apply_auto_render_scale(d.path(), 2).unwrap();
    assert_eq!(load(d.path()).unwrap()["display.render_scale"], "4");
}
```

- [ ] **Step 2: Run** — `cargo test auto_scale` → fails to compile.

- [ ] **Step 3: Implement**

`apply_auto_render_scale`: return `Ok(())` if `launcher_get(root, SCALE_CHOSEN_KEY)` is `Some`; else `save(root, {"display.render_scale": scale.to_string()})`. In `lib.rs`: `save_settings` → after `settings::save` succeeds, if `changes` contains `"display.render_scale"` → `settings::launcher_set(&root, settings::SCALE_CHOSEN_KEY, "true")`. In `.setup` → `settings::apply_auto_render_scale(&settings::root_dir(), gpu::auto_render_scale())`, logging and ignoring an error (never block start-up).

- [ ] **Step 4: Run** — `cargo test` and `npm test` (from `launcher/`) → all PASS.

- [ ] **Step 5: Contract** — `settings-v1.md`: document `[launcher] render_scale_chosen` (written when the player saves a changed resolution in Opciones; while absent the launcher writes the detected `render_scale` at start: 4 with a dedicated GPU, 2 otherwise). Fixture unchanged.

- [ ] **Step 6: Commit** (two repos)

```
cd C:/Users/torso/Documents/deco/launcher && git add src-tauri/src/settings.rs src-tauri/src/lib.rs && git commit -m "feat: start at the detected render scale until the player picks one"
cd C:/Users/torso/Documents/deco/psprecomp && git add docs/contracts/settings-v1.md && git commit -m "docs(contract): launcher render_scale_chosen marker"
```

### Task 9: Final check, review, hand-off

- [ ] **Step 1:** Full gates once more on the final psprecomp HEAD: `ninja && ctest`, `rt_regress.py batch_base <final tour>` PASS.
- [ ] **Step 2:** One whole-branch review (psprecomp `ge-batching` vs `build-2026-10-25`, launcher `auto-scale` vs `beta-1`) with `ponytail:ponytail-review`; fix confirmed findings with their own commits and re-run the gates.
- [ ] **Step 3:** `perf_tester.py clean` unless the user wants the folder kept; confirm the remote folder is gone and `patapon-server\` untouched (file list and mtimes unchanged vs the start of the night).
- [ ] **Step 4:** Finish `build/autopilot/RENDIMIENTO_2026-10-29.md` (Spanish): before/after tables (here + tester), what was skipped and why, what the user must check (launcher on a fresh folder; a game session at 60 fps), and the exact commits. Update memory. **No push.**
