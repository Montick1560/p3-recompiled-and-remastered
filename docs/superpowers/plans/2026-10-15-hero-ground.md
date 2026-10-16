# Hero stays on the ground with smooth animation (Patapon 3, 60 fps) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** With the `60fps` set and smooth animation on, the Hero's feet stay on the ground exactly
as in the original (30 fps, whole-frame poses), while poses still change ~40-60 times per second.

**Architecture:** Today the game module makes GMO models smooth by storing the player's fractional
clock as the motion playhead and setting the model's interpolate flag (+0x28 bit 4) around
`FUN_089DBD2C`, which sends `FUN_089DB1E0` down a key-interpolation path (VFPU lerp / Hermite /
quaternion slerp) that the original game never runs. The user sees the Hero lift slightly off the
ground. First measure and localize (which half of the hook, which bone words); then replace the
interpolate-flag path with a two-sample blend: evaluate the model at the whole frame and at the
next frame through the original path (flag off), and blend the per-bone results by the clock's
fraction (lerp, quaternion nlerp with sign fix). In-between poses then always lie between two
poses the original shows. The baked-skeleton bone blend added in `b743a6d` (barracks Patapons)
is reused through the same pure blend helper.

**Tech Stack:** C++17 runtime + game module (`games/patapon3/runtime/hooks_main.cpp`), header-only
pure helper with ctest unit test, Python drivers outside the repo (`build/tools/*.py`, debug socket
on port 9998), Ghidra headless (`build/tools/ghidra/decomp.sh`).

**Spec:** the user's request on 2026-10-15 ("el héroe se despega un poco del suelo, eso no pasa en
el original ¿puedes arreglarlo?"), plus `docs/superpowers/plans/2026-10-15-smooth-anim.md`
(shipped hook, evidence, barracks follow-up).

## Background facts (verified 2026-10-15, do not re-derive)

- Hook today (`hooks_main.cpp`): `motion_scope` wraps `FUN_08878A18` (tick) / `FUN_08878B18`
  (draw), keeps `g_smooth_time` = player clock (+0x68). `playhead_set_smooth` wraps
  `FUN_089DAF74` (a0 = motion, f12 = whole frame; stores motion +0x1C) and overwrites +0x1C with
  `g_smooth_time` when `f12 == trunc(clock)`. `pose_eval_smooth` wraps `FUN_089DBD2C` (a0 = GMO
  model, a1 = 2, f12 = 0) and sets model +0x28 bit 4 for the call.
- `FUN_089DB1E0(f12, f13, a0 = param_3, a1 = param_4 = motion, a2 = param_5)`: with
  `(*(u32*)(param_3+0x28) & 4) == 0` the playhead is rounded `floor(t + 0.5)` (0x089DB9F0,
  constant at 0x08A65E14) and keys are read at a whole frame (interpolation factor 0). With the
  flag the factor is fractional and the type-1 (VFPU lerp), type-2 (Hermite: `vocp`, `vpfxs`,
  `vmul_p`) and rotation (slerp: `vasin`, `vsqrt`, `vocp` with `vpfxs(8,0,0,0)`, `vsin`, `vrcp`,
  `vdot_p`) code runs — never exercised by the original game.
- Bone outputs: `*(int*)(param_3 + 4) + bone * 0x100` (stride 0x100), bone count
  `*(u16*)(param_3 + 0x18)`. Motion: +0x10 start, +0x14 end, +0x0E loop (s16, non-zero = loop),
  +0x18 rate, +0x1C playhead (from the `param_5 & 1` advance path).
- Runtime VFPU: `runtime/src/psp_vfpu_misc.cpp` (`vfpu_vocp` mirrors PPSSPP `Int_Vocp`),
  `runtime/src/psp_vfpu_trig.cpp` (`vasin` = asin/(pi/2), `vsin` = sin(x*pi/2)); PPSSPP reference
  in `tools/ppsspp-src/Core/MIPS/InterpreterVFPU.cpp`.
- Measuring: per-region frame diffs capture ~50/s through the debug socket (`S` screenshot);
  `PerfSide.land()` (build/tools/perf_hideout.py) lands on the hideout map with the Cuartel
  selected; one short CROSS then enters the barracks (Kan, Chin, Ton, Hero in a row; Hero slot
  x 318-373 at 480x272).
- `PSPRECOMP_NO_AUDIO=1` boots stay black after the opening movie (open bug, unrelated): never set
  it in this plan's runs.

## Global Constraints

- Opt-in only: without `60fps` in `PSPRECOMP_PATCHES` (and without `PSPRECOMP_P3_ANIM_SMOOTH=1`)
  nothing installs; `rt_regress.py build/tour/relevo_base <new>` must PASS (21 gated).
- Test instances only on port 9998; never touch the user's game (9999) or `DECO/savedata`; saves
  are copies (`build/tour_save`).
- Nothing destructive on the PC; no installs; new output in new tagged folders.
- Commits on `patapon3-windows` in `psprecomp/`, author "Patapon Community", **no
  Co-Authored-By**, last lines `Verified: ...` and `Agent: claude-opus-5.5`. **No push** until the
  user plays and confirms.
- Code, comments and commits in English; report to the user in Spanish.
- Game-specific code stays in `games/patapon3/runtime/`; only the generic blend helper goes to
  `runtime/include/`.
- Acceptance: Hero lowest-pixel row (feet) in the barracks, measured at render scale 4, within
  0.25 px (480x272 units) of the 30 fps run's median and p90; Hero pose changes >= 1.6x the
  `PSPRECOMP_P3_ANIM_SMOOTH=0` run; perf_hideout at 60 fps within +10 % CPU of `bone_on60`.

## Review Focus

1. Looping idle reaching its last frame: the "next" sample must be the loop's start frame, and a
   one-shot motion must hold its last frame (no blend toward garbage) — pinned by
   `psp_anim_next_frame` tests in Task 4.
2. Neighbouring rotation keys stored with opposite quaternion signs: blending must take the short
   arc, never pass through a zero/degenerate rotation — pinned by the `psp_blend_pose` sign-flip
   test in Task 4.
3. Second evaluation per frame firing frame-keyed side effects twice (sounds, effects): Task 3
   Step 4 lists every call target of the evaluator chain; the full tour attack/combo checkpoints
   and the user's play test confirm.
4. Models with several motion slots (transition blends): only playheads set inside the scope
   move to the next frame; a slot set elsewhere stays identical in both samples — Task 5 records
   every slot set in scope.
5. Barracks Patapons and army (baked skeletons, `bone_smooth`) must keep ~40+ changes/s after the
   refactor onto the shared helper — re-measured in Task 6.

---

### Task 1: Measure the lift (acceptance tool + baseline)

**Files:**
- Create: `C:/Users/torso/Documents/deco/build/tools/to_cuartel.py` (outside the repo)
- Create: `C:/Users/torso/Documents/deco/build/tools/foot_gap.py` (outside the repo)
- Notes: Evidence section at the end of this plan

**Interfaces:**
- Produces: `python build/tools/to_cuartel.py TAG [SCALE]` → instance on 9998 inside the
  barracks, prints `cuartel pid <n>`; `python build/tools/foot_gap.py PORT SECONDS [x0 y0 x1 y1]`
  → prints `frames=.. lowest_y median=.. p10=.. p90=.. max=..` (480x272 units).

- [ ] **Step 1: Write `to_cuartel.py`**

```python
"""to_cuartel.py TAG [SCALE]: boot a port-9998 instance (extra PSPRECOMP_* from os.environ) on a copy
of build/tour_save, land on the hideout map with the Cuartel selected (gfx_tour's landing), enter
the barracks and leave it running. Never touches port 9999. Do not set PSPRECOMP_NO_AUDIO."""
import os, sys, time
sys.path.insert(0, 'C:/Users/torso/Documents/deco/build/tools')
os.environ.setdefault('TOUR_RT_PORT', '9998')
import perf_hideout
from p3drive import B
os.environ.setdefault('P3DRIVE_SAVE', B + '/tour_save')
os.environ.pop('PSPRECOMP_TEXTURES', None)
scale = sys.argv[2] if len(sys.argv) > 2 else '1'
side = perf_hideout.PerfSide(sys.argv[1], {'PSPRECOMP_PERF': '1', 'PSPRECOMP_RENDER_SCALE': scale})
side.boot()
if not side.land(lambda who, msg: print(f'[{who}] {msg}', flush=True)):
    sys.exit('hideout landing FAILED')
time.sleep(2)
side.press('X', short=True)
time.sleep(9)
print('cuartel pid', side.inst.proc.pid, flush=True)
```

- [ ] **Step 2: Write `foot_gap.py`**

```python
"""foot_gap.py PORT SECONDS [x0 y0 x1 y1]: lowest dark row (the feet) of a unit per captured frame.
Region in 480x272 units (default: the Hero slot in the barracks, above the name bar). Works at any
render scale (the region is scaled to the screenshot). Output in 480x272 units."""
import sys, time
import numpy as np
sys.path.insert(0, 'C:/Users/torso/Documents/deco/build/tools')
import gfx_tour
from p3drive import Instance

port, secs = int(sys.argv[1]), float(sys.argv[2])
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (318, 150, 373, 231)
side = gfx_tour.RtSide.__new__(gfx_tour.RtSide)
side.inst, side.tga = Instance('foot_gap', port), f'C:/Users/torso/Documents/deco/build/shots/_fg{port}.tga'
low, t0 = [], time.perf_counter()
while time.perf_counter() - t0 < secs:
    im = np.asarray(side.frame().convert('RGB'), dtype=np.int16)
    k = im.shape[1] / 480.0
    reg = im[int(y0 * k):int(y1 * k), int(x0 * k):int(x1 * k)]
    rows = np.nonzero(((reg.max(axis=2) < 60).sum(axis=1)) >= max(2, int(2 * k)))[0]
    if len(rows):
        low.append((y0 * k + rows[-1]) / k)
a = np.array(low)
print(f'frames={len(a)} lowest_y median={np.median(a):.2f} p10={np.percentile(a, 10):.2f} '
      f'p90={np.percentile(a, 90):.2f} max={a.max():.2f}')
```

- [ ] **Step 3: Calibrate the region** — boot `PSPRECOMP_PATCHES= python build/tools/to_cuartel.py
  fg30 4` (30 fps, scale 4), save one frame (`side.frame().save(...)` as in foot_gap) and confirm
  the default region ends **above** the black name bar (no row of the bar inside it) and contains
  the Hero's feet; if not, adjust `y1` in the default and note it in Evidence. Kill the instance
  (`taskkill //PID <pid> //F`).

- [ ] **Step 4: Baseline, three configurations** (each: boot with `to_cuartel.py TAG 4`, wait 3 s,
  `python build/tools/foot_gap.py 9998 10`, kill):
  - `fg30`: `PSPRECOMP_PATCHES` unset (original timing, whole frames).
  - `fg60off`: `PSPRECOMP_PATCHES=60fps PSPRECOMP_P3_ANIM_SMOOTH=0`.
  - `fg60on`: `PSPRECOMP_PATCHES=60fps` (shipped mode).
  Expected: `fg60on` median/p90 smaller (higher on screen) than `fg30` by >= 0.25 px = the
  reported lift. If `fg60off` already shows the lift, the cause is the 60fps set itself, not the
  animation hook: stop, write that in Evidence and report to the user before Task 2.
  Also run `python build/tools/anim_rate.py 9998 8 318 150 373 231` per config for the pose rate.

- [ ] **Step 5: Record** the numbers in Evidence (table: config, median, p10, p90, max, pose
  changes/s). No commit (tools live outside the repo).

### Task 2: Localize: which half of the hook lifts the Hero

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp` (diagnostic env, no behaviour change unset)
- Modify: `docs/ENV_FLAGS.md` (Patapon 3 module — diagnostics section)

**Interfaces:**
- Produces: env `PSPRECOMP_P3_ANIM_SMOOTH_DIAG` = `time` (fractional playhead only, flag never
  set) or `flag` (flag set, playhead left whole). Unset = shipped behaviour.

- [ ] **Step 1: Add the switch** (above `playhead_set_smooth`):

```cpp
// PSPRECOMP_P3_ANIM_SMOOTH_DIAG (diagnostic): "time" keeps only the fractional playhead (the
// evaluator still rounds it, so poses stay whole frames, shifted by the rounding), "flag" keeps
// only the interpolate flag (the playhead stays whole, so the factor is 0). Unset = both.
bool g_diag_no_flag = false;
bool g_diag_no_time = false;
```

In `playhead_set_smooth`, after `g_playhead_set(rdram, ctx);` add `if (g_diag_no_time) return;`.
In `pose_eval_smooth`, change the first test to
`if (!g_smooth_scope || model == 0 || g_diag_no_flag) {`.
In `patapon3_register_hooks`, inside `if (smooth_on) {` before the prints:

```cpp
        const char* diag = std::getenv("PSPRECOMP_P3_ANIM_SMOOTH_DIAG");
        g_diag_no_flag = diag && std::strcmp(diag, "time") == 0;
        g_diag_no_time = diag && std::strcmp(diag, "flag") == 0;
        if (g_diag_no_flag || g_diag_no_time) std::fprintf(stderr, "[P3] smooth animation diag: %s only\n", diag);
```

- [ ] **Step 2: Build** — `cd C:/Users/torso/Documents/deco/build/rtw && ninja` (kill any 9998
  instance first: the exe is locked while it runs). Expected: links, no new warnings.
- [ ] **Step 3: Measure** `fg60time` (`PSPRECOMP_P3_ANIM_SMOOTH_DIAG=time`) and `fg60flag`
  (`=flag`) exactly as Task 1 Step 4. Expected outcomes and meaning:
  - lift only in `fg60on` (both) → the interpolation path causes it → Task 3.
  - lift in `fg60time` → the rounded fractional playhead picks a different pose (e.g. a secondary
    motion slot gets the main clock): inspect the motions recorded in Task 3 Step 2 before Task 5.
  - lift in `fg60flag` → the flag changes more than interpolation: decompile the other uses of
    `param_3 + 0x28` in `FUN_089DBD2C` / callees, report, then still do Task 5 (route B never sets it).
- [ ] **Step 4: Docs + commit** — add to `docs/ENV_FLAGS.md` under "Patapon 3 module —
  diagnostics":

```markdown
### `PSPRECOMP_P3_ANIM_SMOOTH_DIAG`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`time` or `flag`; unset = normal).
- **Effect:** With smooth animation on, keep only one half of it: `time` stores the fractional
  playhead but never sets the interpolate flag; `flag` sets the flag but leaves the playhead whole.
- **Set it when:** localizing a pose difference between the smooth and the stepped animation.
```

```bash
git add games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md
git commit -m "chore(p3): PSPRECOMP_P3_ANIM_SMOOTH_DIAG to split the smooth-animation hook" \
  -m "Verified: ninja OK, fg60time/fg60flag foot_gap numbers in the hero-ground plan" -m "Agent: claude-opus-5.5"
```

### Task 3: Bone-level probe of the evaluator (what moves, where it leaves the original range)

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp` (diagnostic `PSPRECOMP_P3_ANIM_PROBE`)
- Create: `C:/Users/torso/Documents/deco/build/tools/anim_probe.py` (outside the repo)

**Interfaces:**
- Consumes: Task 2 diag build.
- Produces: `<dir>/probe_<n>.txt` (model, bones, alpha, motions) and `probe_<n>_{a,b,mid}.bin`
  (bone blocks, `count * 0x100` bytes each); `python build/tools/anim_probe.py <dir> <n>` prints
  per-word findings and a C++ initializer `constexpr uint8_t kBoneKinds[64] = {...};`.

- [ ] **Step 1: Confirm the bone layout** — in `build/p3_output/generated/batch_0197.cpp`, read
  `FUN_089dbd2c` around its three `RECOMP_LOOKUP(0x089DB1E0)` calls and confirm which register
  holds FUN_089DB1E0's `param_3` (the model with +4 bones, +0x18 count). If it is not FUN_089DBD2C's
  own a0, write the actual source (e.g. `*(a0 + X)`) into this plan and use it in Steps 2 and 5.

- [ ] **Step 2: Record the motions set in scope** (needed by the probe and by Task 5). In
  `hooks_main.cpp`, next to `g_smooth_scope`:

```cpp
// Motions whose playhead the player set inside the current tick/draw scope, with the whole frame
// it asked for (FUN_089DAF74's f12). Saved/restored by motion_scope like the clock.
struct ScopeMotions {
    uint32_t addr[8];
    float whole[8];
    int n;
};
thread_local ScopeMotions g_scope_motions = {};
```

In `motion_scope`, save `const ScopeMotions outer_motions = g_scope_motions;`, set
`g_scope_motions.n = 0;` before `(*Orig)(rdram, ctx);` and restore `g_scope_motions = outer_motions;`
after. In `playhead_set_smooth`, after the original call:

```cpp
    if (g_smooth_scope && motion != 0 && g_scope_motions.n < 8) {
        g_scope_motions.addr[g_scope_motions.n] = motion;
        g_scope_motions.whole[g_scope_motions.n] = whole;
        g_scope_motions.n++;
    }
```

- [ ] **Step 3: Add the probe** (diagnostic; inside `pose_eval_smooth`, at the top, when
  `g_probe_dir` is set and `<dir>/probe.trigger` exists — check the file at most every 256 calls):

```cpp
// PSPRECOMP_P3_ANIM_PROBE=<dir> (diagnostic): when <dir>/probe.trigger exists, the next model
// evaluated in scope with a fractional clock is evaluated three times — playheads whole (a),
// whole + 1 (b), and the shipped fractional clock + interpolate flag (mid) — and each bone block
// is written to <dir>/probe_<n>_{a,b,mid}.bin. The model keeps the shipped (mid) result.
std::string g_probe_dir;
int g_probe_seq = 0;

bool probe_due() {
    static int calls = 0;
    if (g_probe_dir.empty() || (++calls & 255) != 0) return false;
    const std::string trig = g_probe_dir + "/probe.trigger";
    if (std::FILE* f = std::fopen(trig.c_str(), "rb")) {
        std::fclose(f);
        std::remove(trig.c_str());
        return true;
    }
    return false;
}

void probe_dump(uint8_t* rdram, uint32_t bones, uint32_t count, const char* tag) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/probe_%d_%s.bin", g_probe_dir.c_str(), g_probe_seq, tag);
    if (std::FILE* f = std::fopen(path, "wb")) {
        std::fwrite(rdram + (bones & 0x07FFFFFFu), 1, count * 0x100u, f);
        std::fclose(f);
    }
}

// Runs the evaluation with every scope motion's playhead at `t_of(i)`, flag as given.
template <typename F>
void eval_at(uint8_t* rdram, recomp_context* ctx, const recomp_context& args, F t_of, bool flag) {
    const uint32_t model = static_cast<uint32_t>(args.r[4]);
    for (int i = 0; i < g_scope_motions.n; i++) {
        const float t = t_of(i);
        std::memcpy(rdram + ((g_scope_motions.addr[i] + 0x1C) & 0x07FFFFFFu), &t, 4);
    }
    uint8_t* flags = rdram + ((model + 0x28) & 0x07FFFFFFu);
    uint32_t f0;
    std::memcpy(&f0, flags, 4);
    const uint32_t f1 = flag ? (f0 | kModelInterpolate) : (f0 & ~kModelInterpolate);
    std::memcpy(flags, &f1, 4);
    *ctx = args;
    g_pose_eval(rdram, ctx);
    std::memcpy(flags, &f0, 4);
}
```

Probe body at the top of `pose_eval_smooth` (after the `model == 0` check):

```cpp
    if (g_smooth_scope && g_scope_motions.n > 0 && probe_due()) {
        const float alpha = g_smooth_time - static_cast<float>(psp_f2i(g_smooth_time));
        const uint32_t bones = rd32(rdram, model + 4);
        uint16_t count;
        std::memcpy(&count, rdram + ((model + 0x18) & 0x07FFFFFFu), 2);
        const recomp_context args = *ctx;
        eval_at(rdram, ctx, args, [](int i) { return g_scope_motions.whole[i]; }, false);
        probe_dump(rdram, bones, count, "a");
        eval_at(rdram, ctx, args, [](int i) { return g_scope_motions.whole[i] + 1.0f; }, false);
        probe_dump(rdram, bones, count, "b");
        eval_at(rdram, ctx, args, [](int) { return g_smooth_time; }, true);
        probe_dump(rdram, bones, count, "mid");
        char path[512];
        std::snprintf(path, sizeof path, "%s/probe_%d.txt", g_probe_dir.c_str(), g_probe_seq++);
        if (std::FILE* f = std::fopen(path, "w")) {
            std::fprintf(f, "model=0x%08X bones=0x%08X count=%u alpha=%.4f clock=%.4f motions=%d\n",
                         model, bones, count, alpha, g_smooth_time, g_scope_motions.n);
            for (int i = 0; i < g_scope_motions.n; i++)
                std::fprintf(f, "motion=0x%08X whole=%.1f\n", g_scope_motions.addr[i], g_scope_motions.whole[i]);
            std::fclose(f);
        }
        return;
    }
```

Register in `patapon3_register_hooks` (inside `if (smooth_on)`):
`if (const char* p = std::getenv("PSPRECOMP_P3_ANIM_PROBE")) g_probe_dir = p;` and add
`#include <string>` at the top.

- [ ] **Step 4: List the evaluator chain's call targets** (Review Focus 3): for each of
  `FUN_089dbd2c`, `FUN_089db1e0`, `FUN_089deb94`, `FUN_089de108`, `FUN_089ddd88`, `FUN_089dc264` in
  `build/p3_output/generated/*.cpp`, list `RECOMP_LOOKUP(0x...)` targets
  (`grep -o "RECOMP_LOOKUP(0x[0-9A-F]*)"` inside each function body). Expected: only math/GMO
  helpers in 0x089D0000-0x089E2000 and libc (0x08A5xxxx memcpy). Any other target (sound, effect,
  callback through a register) → decompile it, write it in Evidence, and do not run a second
  evaluation for motions that use it (stop and ask the user if unclear).

- [ ] **Step 5: Write `anim_probe.py`**

```python
"""anim_probe.py DIR N: compare probe_N_{a,b,mid}.bin (bone blocks, 0x100 bytes per bone).
Per word: varies (a != b), mid outside [min(a,b), max(a,b)] by more than tol, and a guess of
quaternions (4 varying words with |q| ~ 1 in both a and b). Prints the worst offenders and a C++
initializer of per-word blend kinds for one bone (0 keep, 1 lerp, 2 quaternion start)."""
import sys
import numpy as np
d, n = sys.argv[1], sys.argv[2]
rd = lambda t: np.fromfile(f'{d}/probe_{n}_{t}.bin', dtype='<f4').reshape(-1, 64)
a, b, m = rd('a'), rd('b'), rd('mid')
hdr = open(f'{d}/probe_{n}.txt').read().split()
alpha = float([h for h in hdr if h.startswith('alpha=')][0][6:])
finite = np.isfinite(a) & np.isfinite(b) & np.isfinite(m)
varies = finite & (np.abs(a - b) > 1e-6)
lo, hi = np.minimum(a, b), np.maximum(a, b)
tol = 0.02 * np.abs(a - b) + 1e-4
out = varies & ((m < lo - tol) | (m > hi + tol))
print(f'bones={a.shape[0]} alpha={alpha} varying words={int(varies.sum())} out-of-range={int(out.sum())}')
for bone, word in zip(*np.nonzero(out)):
    print(f'  bone {bone:3d} +0x{word * 4:02X}: a={a[bone, word]:.5f} b={b[bone, word]:.5f} '
          f'mid={m[bone, word]:.5f} lerp={a[bone, word] + alpha * (b[bone, word] - a[bone, word]):.5f}')
var_off = varies.any(axis=0)
kinds = [1 if v else 0 for v in var_off]
for w in range(0, 61):
    if all(var_off[w:w + 4]):
        qa, qb = np.linalg.norm(a[:, w:w + 4], axis=1), np.linalg.norm(b[:, w:w + 4], axis=1)
        if np.all(np.abs(qa - 1) < 1e-3) and np.all(np.abs(qb - 1) < 1e-3) and kinds[w] == 1:
            kinds[w:w + 4] = [2, 0, 0, 0]
print('varying offsets:', ' '.join(f'+0x{w * 4:02X}' for w in np.nonzero(var_off)[0]))
print('constexpr uint8_t kBoneKinds[64] = {' + ', '.join(map(str, kinds)) + '};')
```

- [ ] **Step 6: Run the probe** — build; `mkdir -p C:/Users/torso/Documents/deco/build/tmp/anim_probe`;
  boot `PSPRECOMP_PATCHES=60fps PSPRECOMP_P3_ANIM_PROBE=C:/Users/torso/Documents/deco/build/tmp/anim_probe
  python build/tools/to_cuartel.py probe 1`; then 5 times: `touch .../anim_probe/probe.trigger`,
  wait 1 s. Identify the Hero's probe (largest `count`, or the model whose motions match the Hero
  — compare `model=` against a `[DL]`-free check: the probe whose `out-of-range` words include the
  root/leg bones). Run `python build/tools/anim_probe.py <dir> <n>` for each. Expected (if Task 2
  said "interpolation path"): out-of-range words on the Hero's bones; record bones, offsets,
  values and the `kBoneKinds` line in Evidence.
- [ ] **Step 7: Classify** in Evidence: (a) out-of-range words only where the channel is Hermite
  (type 2: tangents can overshoot legitimately) → game data, not an emulation bug; (b) out-of-range
  on linear or slerp channels → suspected VFPU emulation bug: note the ops (`vocp` with explicit
  `vpfxs`, `vasin`, `vsin`, `vrcp`, `vdot_p`) as a follow-up for `test_vfpu.cpp` against
  `tools/ppsspp-src/Core/MIPS/InterpreterVFPU.cpp`, but do NOT fix it in this plan unless Task 5
  is rejected. Either way Task 5 is the fix (it never enters that path).
- [ ] **Step 8: Commit the diagnostics**

```bash
git add games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md
git commit -m "chore(p3): PSPRECOMP_P3_ANIM_PROBE dumps whole/next/interpolated bone blocks" \
  -m "Verified: ninja OK, probe dumps for the barracks Hero (see hero-ground plan Evidence)" -m "Agent: claude-opus-5.5"
```

(add a `PSPRECOMP_P3_ANIM_PROBE` entry to ENV_FLAGS.md in the same style as Task 2's.)

### Task 4: Pure blend helper (TDD)

**Files:**
- Create: `runtime/include/psp_anim_blend.h`
- Create: `runtime/tests/test_anim_blend.cpp`
- Modify: `runtime/CMakeLists.txt` (register the test next to `test_patch_sets`)

**Interfaces:**
- Produces:
  - `enum : uint8_t { kBlendKeep = 0, kBlendLerp = 1, kBlendQuat = 2 };`
  - `void psp_blend_pose(float* a, const float* b, const uint8_t* kinds, size_t n, float t);`
    — in place into `a`; `kBlendQuat` marks the first of 4 words (nlerp, short arc, normalized);
    the 3 following words are consumed by it.
  - `int32_t psp_anim_next_frame(int32_t whole, float start, float end, bool loop);` — returns
    the whole frame to blend toward, or -1 for "no blend" (one-shot at its end).

- [ ] **Step 1: Write the failing test** `runtime/tests/test_anim_blend.cpp`:

```cpp
// psp_anim_blend.h: blending two evaluated poses (smooth animation at 60 fps).
#include "psp_anim_blend.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)
static bool near(float x, float y) { return std::fabs(x - y) < 1e-5f; }

int main() {
    {   // lerp and keep
        float a[3] = {0.0f, 10.0f, 5.0f};
        const float b[3] = {2.0f, 20.0f, 7.0f};
        const uint8_t k[3] = {kBlendLerp, kBlendLerp, kBlendKeep};
        psp_blend_pose(a, b, k, 3, 0.25f);
        CHECK(near(a[0], 0.5f) && near(a[1], 12.5f), "lerp by t");
        CHECK(near(a[2], 5.0f), "keep leaves a");
    }
    {   // t = 0 is exactly a, t = 1 is exactly b
        float a[2] = {1.0f, -3.0f};
        const float b[2] = {4.0f, 9.0f};
        const uint8_t k[2] = {kBlendLerp, kBlendLerp};
        psp_blend_pose(a, b, k, 2, 0.0f);
        CHECK(a[0] == 1.0f && a[1] == -3.0f, "t=0 keeps a bit-exact");
        psp_blend_pose(a, b, k, 2, 1.0f);
        CHECK(near(a[0], 4.0f) && near(a[1], 9.0f), "t=1 reaches b");
    }
    {   // quaternion: identity to 90 deg about z, halfway = 45 deg, unit length
        const float s = std::sqrt(0.5f);
        float a[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float b[4] = {0.0f, 0.0f, s, s};
        const uint8_t k[4] = {kBlendQuat, kBlendKeep, kBlendKeep, kBlendKeep};
        psp_blend_pose(a, b, k, 4, 0.5f);
        const float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
        CHECK(near(len, 1.0f), "nlerp result is unit length");
        CHECK(near(a[2], std::sin(0.3926991f)) && near(a[3], std::cos(0.3926991f)), "halfway is 45 deg");
    }
    {   // quaternion sign flip: b = -a (same rotation) must stay a, not collapse to zero
        float a[4] = {0.1f, 0.2f, 0.3f, 0.9273618f};
        const float b[4] = {-0.1f, -0.2f, -0.3f, -0.9273618f};
        const uint8_t k[4] = {kBlendQuat, kBlendKeep, kBlendKeep, kBlendKeep};
        psp_blend_pose(a, b, k, 4, 0.5f);
        CHECK(near(a[0], 0.1f) && near(a[3], 0.9273618f), "short arc across a sign flip");
    }
    // next frame selection
    CHECK(psp_anim_next_frame(3, 0.0f, 10.0f, false) == 4, "inside: whole + 1");
    CHECK(psp_anim_next_frame(10, 0.0f, 10.0f, false) == -1, "one-shot at its end: no blend");
    CHECK(psp_anim_next_frame(9, 0.0f, 9.5f, false) == -1, "one-shot: next beyond end");
    CHECK(psp_anim_next_frame(10, 2.0f, 10.0f, true) == 2, "loop wraps to its start");
    CHECK(psp_anim_next_frame(9, 2.0f, 10.0f, true) == 10, "loop: last frame still inside");
    if (failures == 0) std::printf("anim_blend: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register and run (fails to build: header missing)** — in `runtime/CMakeLists.txt`
  after the `test_patch_sets` block:

```cmake
# Pose blending for smooth animation (two whole-frame samples). Header-only.
add_executable(test_anim_blend tests/test_anim_blend.cpp)
target_include_directories(test_anim_blend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_anim_blend PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME anim_blend_tests COMMAND test_anim_blend)
```

Run: `cd C:/Users/torso/Documents/deco/build/rtw && ninja test_anim_blend`
Expected: FAIL, `psp_anim_blend.h: No such file or directory`.

- [ ] **Step 3: Implement** `runtime/include/psp_anim_blend.h`:

```cpp
// psp_anim_blend.h — blend two evaluated poses word by word (smooth animation from two
// whole-frame samples). Pure, header-only; used by game modules.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

enum : uint8_t { kBlendKeep = 0, kBlendLerp = 1, kBlendQuat = 2 };

// In place into a: kBlendLerp words become a + (b - a) * t; a kBlendQuat word starts a 4-float
// quaternion blended by normalized lerp along the short arc (b's sign flipped when a.b < 0);
// kBlendKeep words (and the 3 words after a quaternion start) are left as a.
inline void psp_blend_pose(float* a, const float* b, const uint8_t* kinds, size_t n, float t) {
    for (size_t i = 0; i < n; i++) {
        if (kinds[i] == kBlendLerp) {
            a[i] += (b[i] - a[i]) * t;
        } else if (kinds[i] == kBlendQuat && i + 3 < n) {
            const float dot = a[i] * b[i] + a[i + 1] * b[i + 1] + a[i + 2] * b[i + 2] + a[i + 3] * b[i + 3];
            const float sign = dot < 0.0f ? -1.0f : 1.0f;
            float q[4];
            float len2 = 0.0f;
            for (int k = 0; k < 4; k++) {
                q[k] = a[i + k] + (sign * b[i + k] - a[i + k]) * t;
                len2 += q[k] * q[k];
            }
            if (len2 > 0.0f) {
                const float inv = 1.0f / std::sqrt(len2);
                for (int k = 0; k < 4; k++) a[i + k] = q[k] * inv;
            }
            i += 3;
        }
    }
}

// Whole frame to blend toward from `whole`: whole + 1 while it is within [.., end]; past the end
// a looping motion wraps to its start frame, a one-shot returns -1 (hold, no blend).
inline int32_t psp_anim_next_frame(int32_t whole, float start, float end, bool loop) {
    const int32_t next = whole + 1;
    if (static_cast<float>(next) <= end) return next;
    return loop ? static_cast<int32_t>(start) : -1;
}
```

- [ ] **Step 4: Run** — `ninja test_anim_blend && ctest -R anim_blend --output-on-failure`
  Expected: `anim_blend: all tests passed`, ctest 1/1.
- [ ] **Step 5: Reuse it in `bone_smooth`** (hooks_main.cpp): replace the open-coded
  `for (int i = 0; i < 16; i++) m0[i] += ...` with

```cpp
    static const uint8_t kMatrix[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    psp_blend_pose(m0, m1, kMatrix, 16, alpha);
```

and the `next` computation with
`int32_t next = psp_anim_next_frame(frame, start, end, rdram[(player + 0x7B) & 0x07FFFFFFu] != 0);`
(add `#include "psp_anim_blend.h"`). Build; `python build/tools/track.py`-style check is in Task 6.
- [ ] **Step 6: Commit**

```bash
git add runtime/include/psp_anim_blend.h runtime/tests/test_anim_blend.cpp runtime/CMakeLists.txt games/patapon3/runtime/hooks_main.cpp
git commit -m "feat(runtime): psp_anim_blend.h pose blend helper; bone_smooth uses it" \
  -m "Verified: ninja OK, ctest (anim_blend_tests + all) green" -m "Agent: claude-opus-5.5"
```

### Task 5: Two-sample blend replaces the interpolate-flag path

**Files:**
- Modify: `games/patapon3/runtime/hooks_main.cpp`

**Interfaces:**
- Consumes: Task 3 `g_scope_motions`, `eval_at`, the `kBoneKinds` line from `anim_probe.py`;
  Task 4 `psp_blend_pose`, `psp_anim_next_frame`.
- Produces: shipped smooth mode = two-sample blend. `PSPRECOMP_P3_ANIM_SMOOTH_DIAG=interp` keeps
  the old interpolate-flag path for A/B (replaces `time`/`flag`, which are removed).

- [ ] **Step 1: Stop overriding the playhead** — `playhead_set_smooth` keeps only the original call
  and the Task 3 recording (delete the `memcpy` of `g_smooth_time` into +0x1C unless
  `g_diag_interp`).
- [ ] **Step 2: Paste the kinds table** from Task 3 Step 6 above `pose_eval_smooth`:

```cpp
// Per-word blend kinds of one 0x100-byte GMO bone block (FUN_089DB1E0 outputs), from
// anim_probe.py on the barracks Hero (hero-ground plan, Evidence): words that change between two
// whole frames are lerped, quaternions nlerped, the rest kept.
constexpr uint8_t kBoneKinds[64] = { /* the exact line printed by anim_probe.py */ };
```

(The initializer is the literal output of `anim_probe.py`; if probes of different models print
different tables, OR them: a word varying in any model is lerped, quaternion starts must agree —
if they disagree, stop and report.)

- [ ] **Step 3: Replace `pose_eval_smooth`'s flag path** with the blend:

```cpp
void pose_eval_smooth(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t model = static_cast<uint32_t>(ctx->r[4]);
    // (Task 3 probe block stays here, unchanged.)
    if (!g_smooth_scope || model == 0 || g_scope_motions.n == 0) {
        g_pose_eval(rdram, ctx);
        return;
    }
    if (g_diag_interp) {  // old path, for A/B only
        pose_eval_interp(rdram, ctx);
        return;
    }
    const int32_t whole = psp_f2i(g_smooth_time);
    const float alpha = g_smooth_time - static_cast<float>(whole);
    const recomp_context args = *ctx;
    g_pose_eval(rdram, ctx);  // sample a: the original whole-frame pose
    if (!(alpha > 0.0f && alpha < 1.0f)) return;
    int32_t next[8];
    bool any = false;
    for (int i = 0; i < g_scope_motions.n; i++) {
        const uint32_t m = g_scope_motions.addr[i];
        float start, end;
        std::memcpy(&start, rdram + ((m + 0x10) & 0x07FFFFFFu), 4);
        std::memcpy(&end, rdram + ((m + 0x14) & 0x07FFFFFFu), 4);
        int16_t loop;
        std::memcpy(&loop, rdram + ((m + 0x0E) & 0x07FFFFFFu), 2);
        const int32_t w = static_cast<int32_t>(g_scope_motions.whole[i]);
        next[i] = psp_anim_next_frame(w, start, end, loop != 0);
        if (next[i] < 0) next[i] = w;  // one-shot at its end: same pose in both samples
        any |= next[i] != w;
    }
    if (!any) return;
    const uint32_t bones = rd32(rdram, model + 4);
    uint16_t count;
    std::memcpy(&count, rdram + ((model + 0x18) & 0x07FFFFFFu), 2);
    thread_local std::vector<float> sample_a;
    sample_a.resize(static_cast<size_t>(count) * 64);
    std::memcpy(sample_a.data(), rdram + (bones & 0x07FFFFFFu), sample_a.size() * 4);
    eval_at(rdram, ctx, args, [&](int i) { return static_cast<float>(next[i]); }, false);  // sample b
    for (int i = 0; i < g_scope_motions.n; i++) {  // playheads back to what the game set
        std::memcpy(rdram + ((g_scope_motions.addr[i] + 0x1C) & 0x07FFFFFFu), &g_scope_motions.whole[i], 4);
    }
    float* b = reinterpret_cast<float*>(rdram + (bones & 0x07FFFFFFu));
    for (uint32_t bone = 0; bone < count; bone++) {
        float* a = sample_a.data() + bone * 64;
        psp_blend_pose(a, b + bone * 64, kBoneKinds, 64, alpha);
    }
    std::memcpy(b, sample_a.data(), sample_a.size() * 4);
}
```

Rename the current body to `void pose_eval_interp(uint8_t* rdram, recomp_context* ctx)` (flag
set around the call, as today); `g_diag_interp` = `PSPRECOMP_P3_ANIM_SMOOTH_DIAG=interp`; remove
`g_diag_no_flag` / `g_diag_no_time` and update the ENV_FLAGS entry (values: `interp`). Add
`#include <vector>`. If Task 3 Step 1 found that `param_3` is not FUN_089DBD2C's a0, use that
source for `bones`/`count` here.

- [ ] **Step 4: Build and measure** — `ninja`; boot `fg60blend` (`PSPRECOMP_PATCHES=60fps`) with
  `to_cuartel.py fg60blend 4`; `foot_gap.py 9998 10`; `anim_rate.py 9998 8 318 150 373 231`.
  Expected: median/p90 within 0.25 px of `fg30`; Hero pose changes >= 1.6x `fg60off`.
  If the lift remains: compare with `PSPRECOMP_P3_ANIM_SMOOTH_DIAG=interp` and the probe again
  (the blend may be missing a varying word: re-run anim_probe with the blend build, look for
  words that differ between `a`, `b` and the blended block and are `kBlendKeep`).
- [ ] **Step 5: Commit**

```bash
git add games/patapon3/runtime/hooks_main.cpp docs/ENV_FLAGS.md
git commit -m "fix(p3): smooth GMO poses from two whole-frame samples (Hero stays grounded)" \
  -m "<body: what the interpolate-flag path did, what the probe showed, the blend>" \
  -m "Verified: ninja OK, ctest green, barracks Hero feet fg30 <m30> vs fg60blend <mB> px, pose rate <r>/s" \
  -m "Agent: claude-opus-5.5"
```

(The body and numbers are written from this run's Evidence; no co-author line.)

### Task 6: Regressions, coverage, docs, hand-off

- [ ] **Step 1: Unit + regression gates** — `ctest` (all green); flag-off tour
  `TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/ground_off`
  then `python -I build/tools/rt_regress.py build/tour/relevo_base build/tour/ground_off` → PASS.
- [ ] **Step 2: 60 fps tour** — same with `PSPRECOMP_PATCHES=60fps` into `build/tour/ground60`:
  31/31 checkpoints; `rt_regress.py build/tour/bone_smooth60 build/tour/ground60` gated PASS;
  look at `25_march`, `26_attack_wall`, `27_attack_combo` next to `bone_smooth60` (army intact,
  Hero on the ground).
- [ ] **Step 3: Coverage re-measure** — barracks `track.py`-style per-unit rates (Kan/Chin/Ton
  >= 40/s, Hero >= 1.6x `fg60off`); hideout map Hero `foot_gap.py 9998 10 200 100 260 200` vs 30 fps
  (calibrate as Task 1 Step 3); training mission idle army `anim_rate.py` >= 1.6x smooth-off.
- [ ] **Step 4: Performance** — `PSPRECOMP_PATCHES=60fps python -I build/tools/perf_hideout.py ground_on60`
  then `--diff bone_on60 ground_on60`: fps ~60, CPU <= +10 %.
- [ ] **Step 5: Docs** — `docs/ENV_FLAGS.md` `PSPRECOMP_P3_ANIM_SMOOTH` effect text (two
  whole-frame samples blended; no interpolate flag), `ARCHITECTURE.md` 60fps paragraph (same
  sentence), `README.md` only if its sentence became false, this plan's Evidence; memory
  `p3-60fps.md` + MEMORY.md line.
- [ ] **Step 6: Commit** `docs(p3): hero-ground evidence and smooth-animation docs` (Verified/Agent
  lines). No push.
- [ ] **Step 7: Spanish summary for the user** — what was wrong, numbers (feet 30 vs 60, rates),
  what to look at while playing (Hero standing/walking in the barracks, hideout map, march; loop
  restarts; attack sounds once), how to compare (`set PSPRECOMP_P3_ANIM_SMOOTH_DIAG=interp` in a copy
  of `jugar_60fps.bat` shows the old way). Push only after the user confirms.

## Evidence

Executed 2026-10-15 (inline, executing-plans). Commits 75c7d2e (diagnostics), 9343e86 (helper),
182a785 (fix), plus the docs commit. The user played with `jugar_60fps.bat` and confirmed the
Hero stays on the ground (2026-10-16).

**Where it shows.** The user sees the lift "everywhere the Hero appears (mission and barracks)".
In the barracks the Hero's feet are behind the name bar (bar from y=232.0), so `foot_gap.py`
measures from y=180 and also prints the top row and the dark-pixel centroid. `gfx_tour` cannot
land at render scale 4 (it matches 480x272 references): `tour_scaled.py` / `hero_march.py`
downscale only for matching.

**Task 1 (pixels, idle).** No lift was measurable at idle with the frozen tour save:

| scene (scale 4) | config | metric | median | p10 | p90 |
|---|---|---|---|---|---|
| barracks, Hero slot | fg30 | centroid_y | 211.06 | 211.05 | 211.18 |
| barracks, Hero slot | fg60on | centroid_y | 211.08 | 210.98 | 211.19 |
| mission start, legs (150,214)-(166,232) | m30 / m60on | lowest_y | 231.75 | 231.75 | 231.75 |

Per-column gap above the ground at the Hero's legs (x 152-157, 250 frames): 0.00 px median and
max in m30, m60off and m60on. After the march command the Hero is hidden by the wall.

**Task 3 (bone probes) — the cause.** Bone block layout (0x100 per bone): +0x50 translation
vec4, +0x60 rotation quaternion (|q| = 1 in every bone), +0x70 scale vec4, +0x4C not a float.
With the interpolate-flag path, FUN_089DB1E0 treats channels differently:
- barracks Hero (27 bones, clock 8.333): rotations +0x68/+0x6C at 0.333 of a->b, translations
  +0x50/+0x54/+0x5C at 0.000 (held at the whole frame);
- a 20-bone model at clock x.75: every varying word at 1.000 (snapped to the next frame);
- other models: some words linear at alpha, scale words 0.26..0.52 at alpha 0.5.
So each pose mixed two frames per channel; no single word left [a, b] (out-of-range 0 in all
probes), but the combination is a pose the game never shows. The evaluator's call closure
(Task 3 Step 4): GMO lib 0x089D5664..0x089DEB94, memset 0x088129EC, memcpy 0x0881260C; indirect
calls only in the GMO allocator (lazy, one-time). DBD2C with f12 = 0 freezes slot crossfades,
and key search walks both ways from its cached index, so a repeated evaluation is idempotent.

kBoneKinds (OR of six barracks models, +0x58 added as the translation's z):
lerp +0x50..+0x5C, quaternion at +0x60, lerp +0x70..+0x7C, the rest kept.

**Task 5 (blend).** Probe with `mid` = shipped mode, 20-bone model at alpha 0.75: blend puts
every varying word at 0.750 (interp: 1.000). Barracks crops, scale 1, 300 captures, Hero slot
changes per 60 captures at thresholds >24x3 / >12x1 / >6x1 and mean |diff| per step:

| config | >24x3 | >12x1 | >6x1 | step |diff| |
|---|---|---|---|---|
| 30 fps | 10.4 | 10.4 | 10.4 | 1152 |
| 60 fps, smooth off | 9.8 | 9.8 | 9.8 | 1073 |
| 60 fps, blend | 11.8 | 35.7 | 49.8 | 1251 |
| 60 fps, interp (old) | 16.9 | 40.9 | 54.2 | 1998 |

The Hero idles with sub-pixel motion, so only the fine threshold sees the in-between poses
(blend 5x smooth-off). The old mode moves ~1.6x more per step than 30 fps (the per-channel
jitter); the blend moves like 30 fps. Kan/Chin/Ton (bone_smooth on the shared helper):
43.7/47.2/46.4 vs 9.6 off.

**Task 6 (gates).** ctest 62/62. Tours at night first failed rt_regress on 11-20 (hideout sky
follows the RTC, identical with and without the hook); rerun with
`PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`: flag-off `ground_off_rtc` vs `relevo_base` PASS 21/21;
60 fps `ground60_rtc` vs `bone_smooth60` PASS 21/21, 31/31 checkpoints, march/attack army intact.
perf_hideout `ground_on60` vs `bone_on60`: fps 59.45 (-0.4 %), CPU one core 63.5 % (-3.6 %),
ge_ms -3.6 %. Barracks Kan/Chin/Ton stay ~45 changes per 60 captures. Final review: self-review
against Review Focus 1-5 (FUN_089DAF74 is a plain store to motion +0x1C, so writing playheads
directly is faithful).

