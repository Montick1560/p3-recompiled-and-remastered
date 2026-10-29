# GE draw batching + automatic render scale — design

Date: 2026-10-28. Owner: the user (Patapon 3 native port). Builder: Claude, working overnight.

## Intent

Players on modest PCs (integrated graphics: Intel HD/UHD, AMD Vega APUs) find the whole game slow; players with a
dedicated GPU already hold 60 fps. The goal is **60 fps on an Intel HD Graphics 4000 (i7-3770, the `tester` PC) at
render scale 2**, without any visible change to the picture.

Measured on this PC (Ryzen 5 4500 + RTX 3050, `build/tour/perf_*`, 21 Oct): hideout at 30 fps uses 25–30 % of one
core, GE translation 1.6–1.75 ms/frame, **≈1 420 GL draw calls per frame** and 27 texture binds per frame. Each
GE `PRIM` becomes its own `glBufferData` + `glDrawArrays` (`runtime/src/psp_ge_draw.cpp:1100`, `:1195`), plus two to
four heap-allocated `std::vector`s. Old Intel GL drivers pay a high CPU cost per draw call, so at 60 fps
(~85 000 draws/s) that is the most likely bottleneck. PPSSPP avoids it by merging consecutive primitives that share
state into one draw (its DrawEngine) — that is the reference design here (`tools/ppsspp-src/`, AGENTS rule 6).

## Goals

1. **Part A — draw batching (psprecomp runtime).** Consecutive non-clear PRIMs whose GL-visible state is identical are
   appended to one pending vertex batch and drawn with a single upload + draw call when that state is about to change
   or the list ends.
2. **Part B — automatic render scale (launcher).** If the player never chose a render scale, the launcher starts the
   game at **4 with a dedicated GPU, 2 without one**. An explicit choice in Opciones always wins.
3. Evidence on real modest hardware: before/after numbers measured on `tester`.

## Non-goals

- No Direct3D 11 / Vulkan backend, no ANGLE (separate decision later, mainly for PCs without GL 3.3).
- No renderer-wide refactor. New and touched code must be clean (non-redundant, readable); untouched code stays.
- No specialised-shader work (uber-shader stays) unless the tester measurement after Part A proves the GPU, not the
  CPU, is the limit — then it is a follow-up design, not part of this one.
- No persistent ring vertex buffer up front. One `glBufferData` per *batch* stays; a ring buffer is added only if the
  post-batching profile shows uploads as a top cost (conditional task in the plan).
- No visual quality reduction beyond the agreed scale-2 default on integrated GPUs.

## Anti-goals (failure even if faster)

- Any picture difference at the same render scale: a missing, reordered, wrongly blended or wrongly textured sprite.
- Breaking render-to-texture, stencil, clears, block transfers, screenshots, texture replacement or button-prompt
  patching (all depend on draw order relative to other GE work).
- Touching the user's server on `tester` (`patapon-server/`, the tunnel, the running `servidor_beta.bat`), shutting
  down or rebooting `tester`, deleting anything there that is not ours.

## Constraints

- PPSSPP is the reference; no invented GE semantics. Project rules: `psprecomp/CLAUDE.md` (GL only on the render
  thread, no game-specific code in `runtime/src`, ARCHITECTURE.md kept true), `AGENTS.md`.
- Workflow rule (user, 28 Oct): superpowers for process, ponytail for code — smallest complete change, reuse what
  exists, no unrequested abstraction, a unit test for every new non-trivial pure logic, `shortcut:` comments for
  known limits.
- Commits: author "Patapon Community", no Claude trailer. Local branches only (`ge-batching` in psprecomp, a new
  branch from `beta-1` in `launcher/`); **no push** without the user's OK.
- Verification per task: build (ninja), ctest, `build/tools/rt_regress.py` vs a baseline tour on port 9998,
  `build/tools/perf_hideout.py` for numbers. The user's game (port 9999) is never touched.

## Part A — how batching works (decided)

**Where vertices come from.** Today `ge_draw_prim` decodes, transforms on the CPU (`ge_transform_vertices`: matrices,
skinning, morph, lighting, UV scale/offset and fog all end up baked into `PackedVertex`), expands rectangles and
through-mode lines, then uploads and draws. Because the transform is already on the CPU at the time the PRIM is
processed, a batch can freely mix vertices produced under different matrices. Only state read **at draw time** must
match.

**Batch key = state read at draw time.** Everything `ge_draw_prim` sets on GL after packing: shader uniforms
(`ge_shader_set_uniforms`: texture enable/func/alpha/double/env colour, alpha test, fog enable/colour, through flag),
the bound texture (`ge_texture_bind` inputs), colour write mask, blend, depth test/func/write, stencil, cull, scissor,
viewport + depth range, and the GL primitive class (triangles / lines / points).

**Flush rule — port PPSSPP's command flags.** PPSSPP's command table (`GPU/GPUCommonHW.cpp`, `FLAG_FLUSHBEFORE`,
`FLAG_FLUSHBEFOREONCHANGE`, `FLAG_EXECUTE`) says which GE commands must flush pending draws before they apply, and
whether only when the 24-bit value changes. Port it as a pure table (one header, unit-tested), consulted in the GE
command loop (`psp_ge.cpp`) **before** the command updates `g_ge_state`; keep the last raw word per command to detect
"on change". Then clear the flush flag from the commands our renderer consumes only on the CPU at PRIM time (world /
view / projection / bone / texgen matrix numbers and data, morph weights, light and material parameters, UV
scale/offset, vertex/index addresses, base/offset/origin), each one justified by a grep showing the field is read only
from the decode/transform path and never at draw time. When unsure, keep the flush.

**Other flush points (always flush first):** list end / FINISH / SIGNAL, render target switch
(`ge_draw_select_target`), block transfer (`ge_draw_note_transfer`), clear-mode PRIMs, BEZIER/SPLINE entry (they
reuse `ge_draw_prim`, which is fine, but their state must not be split mid-patch), present and screenshot capture,
RTT CPU-write checks, and any debug path that observes a single draw (`PSPRECOMP_DRAW_LOG`, skip ranges, `TEX_WATCH`,
`VTX_WATCH`: these force batch-of-one so the tools keep meaning what they say).

**Primitive merging.** Triangle lists append as-is. Strips and fans are converted to triangle lists on append
(strip: alternate the winding of odd triangles so culling stays correct; PPSSPP `IndexGenerator` is the reference).
Rectangles and through-mode lines are already expanded to triangles. Non-through lines and points batch only with
their own class. Merged draws keep submission order, so blending and depth results are identical.

**Per-draw extras.** `draw_max_v` (texture-replacement hash height) becomes the max over the batch. RTT sampling notes,
target `gl_written`/`cpu_newer` bookkeeping and the existing debug counters stay per PRIM at append time. The perf
counter `draw_calls` counts real GL draws; `prims` keeps counting PRIMs, so `perf_hideout.py` shows the drop
directly.

**Memory.** The batch owns one `std::vector<PackedVertex>` reused across frames (capacity kept), which also removes
the per-PRIM allocations of `packed`.

**Kill switch.** `PSPRECOMP_GE_BATCH=0` restores one draw per PRIM (same binary A/B for regression and perf checks,
and a rollback for players if a bug slips through). Documented in `docs/ENV_FLAGS.md`.

**Correctness checks.**
- Pure unit tests: the flush table (spot checks against PPSSPP's flags, the CPU-only exemptions), strip/fan →
  list conversion (vertex order and winding), batch-key equality.
- Runtime: `rt_regress.py` on the standard tour, batching on vs off with the same binary; gated checkpoints must
  PASS. Plus a direct pixel comparison of the hideout frame at scale 1 and 2 (on vs off).
- Perf: `perf_hideout.py` on vs off here (draws/frame must drop sharply; ge_ms/frame must not rise).

## Part B — automatic render scale (decided)

- **Who decides:** the launcher. Its first load writes every option with its default (`settings.rs` `load` →
  `default_ini`), so the presence of `render_scale` says nothing. Instead (user, 28 Oct):
  - When the player saves a changed resolution in Opciones (Opciones saves only the changed rows), the launcher also
    writes a marker `[launcher] render_scale_chosen = true` (the game ignores `[launcher]`).
  - At each launch without that marker, the launcher detects the GPU and **writes** `[display] render_scale` (4 or
    2) into `settings.ini` before starting the game. No environment variable: Opciones, `jugar.bat` and `mi_build`
    all read the same value.
  - Existing installs have no marker, so they are re-detected on their next launch: integrated GPUs drop from 4 to 2
    (accepted by the user). A player who sets 4 again in Opciones keeps it from then on.
- **Dedicated vs integrated:** enumerate DXGI hardware adapters (skip Microsoft Basic Render); "dedicated" =
  any adapter with `DedicatedVideoMemory ≥ 2 GiB`. `shortcut:` an APU whose BIOS reserves ≥ 2 GiB counts as dedicated
  and gets 4; upgrade to a D3D12 UMA check if players report it.
- **Hybrid laptops:** the runtime exe exports `NvOptimusEnablement = 1` and `AmdPowerXpressRequestHighPerformance = 1`
  so the game actually runs on the dedicated GPU the launcher detected (otherwise it may pick 4 and run on the Intel
  chip). Two exported globals in `main.cpp`, Windows only.
- Update `docs/contracts/settings-v1.md` (the `[launcher] render_scale_chosen` key and the detection rule; the
  defaults fixture keeps `render_scale = 4`) and the launcher tests.
- Part A also flushes on every return from the GE list processor (end, stall, finish, signal), since a stalled list
  resumes later through `psp_render_queue.cpp`.

## Measuring on `tester` (decided)

The user authorised remote runs on `tester` (192.168.194.59, i7-3770, Intel HD 4000, Windows; it hosts the live beta
server). Facts checked 28 Oct: SMB admin share works with the session the user authorised; `schtasks /s` works; the
debug socket binds loopback only, so the driver must run on `tester` itself; Python there is the Microsoft Store
build with unknown packages (no installing); 7.9 GB free.

- Our folder only: `C:\Users\tester\Documents\p3-perf\` (a game build + our scripts + results). Nothing else is
  written on `tester`.
- **Disk budget (user, 28 Oct): at most 2.9 GB on `tester`**, counted as the whole `p3-perf\` folder including game
  logs (runtime logs can grow to GB: run with quiet logs, cap each run's log, delete each run's log once its `[PERF]`
  lines are copied back). Check the folder size before every run; if a run would exceed the budget, stop using
  `tester` and measure on this PC instead.
- Runs start as a one-shot scheduled task in the logged-in user's interactive session (the game needs a desktop),
  debug port 9998, and stop by killing **only the PID we started**. The task is deleted after each run.
- The driver on `tester` is stdlib-only Python (no PIL): fixed button sequence over the debug socket to reach the
  hideout from the frozen tour save, then `PSPRECOMP_PERF=1` `[PERF]` lines for N seconds, plus a few screenshots
  (TGA) copied back so we can confirm here (with PIL) that it really was the hideout.
- Matrix: scale 2 and 4 × batching off/on, 30 and 60 fps patch sets, hideout gated; one mission best-effort.
- The live server keeps running; its CPU use during the run is noted next to the numbers.

## Left to the builder

Exact names and file split (keep it inside the existing `psp_ge_draw.cpp` / `psp_ge.cpp` structure plus one small
pure header for the flush table and the list conversion), the exact exemption list (by evidence), the driver's button
timings, and whether the conditional ring-buffer task is needed.

## Success

- `tester`, hideout, scale 2, 60 fps set: steady 60 fps (or a measured, explained gap with the next bottleneck named).
- Draws/frame drop from ≈1 420 to a few hundred or less; `rt_regress.py` PASS on vs off; ctest green.
- Launcher: a fresh install starts at 2 on `tester` and at 4 on this PC; an explicit Opciones value is kept.
