# Hideout white flash (after a mission) — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written 2026-10-16 (Opus 5.5) for the next session. Nothing implemented yet.

**Goal:** when the army comes back to the hideout after a mission (won or lost), the Patapons, NPCs
and the Hero turn white the way they do on the PSP; today the native port shows them with their
normal colours.

**Architecture:** first prove which GE feature makes them white: dump the display list of a "white"
frame in PPSSPP and the same frame in the runtime, then compare. The leading suspect is **GE fog**,
which the runtime does not implement at all. If Task 1 confirms fog, Tasks 2-4 add it the way PPSSPP
does: a per-vertex fog factor computed on the CPU next to the existing transform, carried to the
shader, and `rgb = mix(fog_color, rgb, clamp(f, 0, 1))` in the fragment shader. If Task 1 finds a
different feature, stop after Task 1 and rewrite Tasks 2-4 for it (see "If it is not fog").

**Tech Stack:** C++17 runtime (SDL2 + OpenGL 3.3, GLSL 330 uber-shader), CTest unit tests, Python
tools in `DECO/build/tools` (gfx_tour, gelist, rt_regress), PPSSPP 1.20.4 (`build/ppsspp_mp`) as the
oracle, PPSSPP source in `DECO/tools/ppsspp-src`.

**Spec:** user report, 2026-10-16 conversation: *"cuando los patapon pierden o ganan, te vas al
escondite, todo se pone en blanco (patapons, npc, heroe, etc) esta version aun no la tiene"*. No
separate spec file. The binding authority is the PSP behaviour as PPSSPP shows it.

`DECO` = `C:/Users/torso/Documents/deco`. Repo = `DECO/psprecomp`, branch `patapon3-windows`.

## Why fog is the leading suspect (evidence found while writing this plan)

- `runtime/src/psp_ge.cpp:656` stores `GE_CMD_FOGENABLE` into `g_ge_state.fog_enable`, `:767` stores
  `FOGCOLOR`, and `:770-772` drops `FOG1`/`FOG2` (`break; // Store if needed later`). Nothing in
  `psp_ge_draw.cpp`, `psp_ge_shader.cpp` or `psp_ge_vertex.cpp` reads fog. Fog is a no-op today.
- With fog the GE paints a model in a flat colour: `FOG1` (fog end) and `FOG2` (slope) chosen so
  the factor is 0 turn every transformed vertex into `FOGCOLOR`. A white `FOGCOLOR` that fades back
  is exactly "everything turns white, then comes back". Fog only affects transformed (non-through)
  draws, which matches "Patapons, NPCs, Hero" (3D-placed sprites) and not the HUD.
- Other GE features the runtime also ignores, so Task 1 must rule them out:
  `COLORTESTENABLE`/`COLORTEST`/`COLORREF`/`COLORTESTMASK` (`psp_ge.cpp:674,808-810`),
  `LOGICOPENABLE`/`LOGICOP` (`:675,852`, a SET/OR logic op also paints white), and the stencil value
  into colour alpha (deferred step 5 of `2026-10-14-ge-stencil.md`, matters if the white comes from
  a `DST_ALPHA` blend).

## PPSSPP reference (`DECO/tools/ppsspp-src`)

- Fog factor per vertex: `GPU/Common/VertexShaderGenerator.cpp:904`
  `v_fogdepth = (viewPos.z + u_fogcoef.x) * u_fogcoef.y;` (`viewPos` = world·view·pos, view space).
- Fog coefficients: `GPU/Common/ShaderUniforms.cpp:255-267` `UpdateFogCoef`: `fog1`/`fog2` are
  float24 (`getFloat24`), and a non-finite value becomes ±65535 (sign kept).
- Fog enable: `GPU/Common/ShaderId.cpp:326` `enableFog = gstate.isFogEnabled() && !isModeThrough`.
- Fragment: `GPU/Common/FragmentShaderGenerator.cpp:865-868`, after the texture function and colour
  doubling, before the alpha/colour tests:
  `fogCoef = clamp(v_fogdepth, 0.0, 1.0); v.rgb = mix(u_fogcolor, v.rgb, fogCoef);`
- Fog colour: `ShaderUniforms.cpp:66-67` `Uint8x3ToFloat3(ub->fogColor, gstate.fogcolor)`: byte 0 = R,
  byte 1 = G, byte 2 = B.
- Software reference (exact hardware rounding, not needed for the GL path):
  `GPU/Software/DrawPixel.cpp:713-719`, `GPU/Software/TransformUnit.cpp:384-395`.
- GE command numbers (`GPU/ge_constants.h`): FOGENABLE 0x1F, FOG1 0xCD, FOG2 0xCE, FOGCOLOR 0xCF,
  COLORTESTENABLE 0x27, LOGICOPENABLE 0x28, LIGHTINGENABLE 0x17, TEXFUNC 0xC9, TEXENVCOLOR 0xCA,
  COLORTEST 0xD8, COLORREF 0xD9, COLORTESTMASK 0xDA, BLENDFIXEDA 0xE0, BLENDFIXEDB 0xE1, LOGICOP 0xE6.

## Global Constraints

- Test instances only on **port 9998** (`TOUR_RT_PORT=9998`). Never touch the user's game (port 9999)
  or `DECO/savedata`; use copies (`build/tour_save`, or a new copy under `build/`).
- Always `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00` for tours and `rt_regress`.
- Never set `PSPRECOMP_NO_AUDIO=1`. Nothing destructive on the PC, no installs, no system security
  changes. Ask the user before closing any PPSSPP they opened.
- Never edit `build/p3_output/generated/*.cpp`; no game-specific code in `runtime/src` (fog is
  generic GE behaviour, so it belongs in `runtime/src`).
- Commits: author "Patapon Community", **no `Co-Authored-By`**, English, ending with `Verified: ...`
  and `Agent: ...` lines. Talk to the user in Spanish.
- **No push** until the user plays and confirms. **Never push `f639180`** (local exe icon). It sits
  below the fast-exit commits; any push must leave it out (rebase the commits to push onto
  `f639180^` on a push branch), or ask the user.
- Line endings: the repo is CRLF on this checkout; prefer the Edit tool over shell rewrites.
- Build: `cmake --build DECO/build/rtw` builds both the user's play exe
  (`build/rtw/psprecomp_runtime.exe`) and the unit tests; `ctest --test-dir DECO/build/rtw` must stay
  green (69/69 at the start of this plan, plus the new tests). The user may be playing with that exe:
  check that no `psprecomp_runtime.exe` holds port 9999 before rebuilding, and ask if one does.

## Review Focus

1. **Through-mode draws (HUD, menus, text) with fog enable left on**: they must not be fogged
   (PPSSPP `ShaderId.cpp:326`). Pinned by `fog_inactive_in_through_mode` (Task 2) and by the
   `rt_regress` HUD points (Task 4).
2. **Non-finite FOG1/FOG2** (a game can write inf/NaN float24): must become ±65535, never NaN in the
   shader (a NaN factor would black out or drop pixels). Pinned by `fog_coef_nonfinite_clamped`.
3. **Rectangles (sprites) in transform mode**: `ge_expand_rectangle` builds 6 corners from 2; the fog
   factor must be carried to all 6, or sprites (most Patapon parts) stay unfogged. Pinned by
   `rect_expand_keeps_fog` (Task 3).
4. **Every scene that sets fog for real** (mission backgrounds, world map, the hideout sky) changes
   once fog works. That is correct PSP behaviour, but compare each one against PPSSPP in the full
   tour (Task 4 gate), not only the hideout return.
5. **Degenerate-matrix fallback / NDC-direct paths** (`psp_ge_vertex.cpp:690-715`, `w = 1`): the
   view z is not meaningful there; fog must stay off (factor 1) on that path. Pinned by
   `fallback_path_has_no_fog` (Task 3).

---

### Task 1: Diagnose the white frames (no runtime code)

Goal: one sentence in this file that says which GE state makes the units white, with the PRIM lines
that prove it, on both sides.

**Files:**
- Modify: `DECO/build/tools/gelist.py` (outside the repo; extend the PRIM line)
- Modify: this plan (`psprecomp/docs/superpowers/plans/2026-10-16-hideout-white-flash.md`), new
  section "Task 1 finding"
- Output: `DECO/build/tour/white/` (frames, lists)

**Interfaces:**
- Produces: the finding (fog: FOGENABLE/FOG1/FOG2/FOGCOLOR values seen on the white PRIMs; or the
  other feature) that Tasks 2-4 rely on.

- [ ] **Step 1: Add the candidate registers to the gelist PRIM line**

In `gelist.py` `walk()`, after the stencil block (the `+ ' ste=%d stt=...'` expression that ends
with `st.get(0xD2, 0) & 3))`), append a third block so each PRIM also prints fog, colour test, logic
op, lighting, texture function and blend fixed colours:

```python
                         # Whitening candidates: fog enable/end/slope/colour, colour test, logic op,
                         # lighting, texture function/env colour, blend fixed A/B.
                         + ' fog=%d f1=%06X f2=%06X fc=%06X cte=%d ct=%06X lop=%d lo=%X lit=%d tfn=%06X tenv=%06X fa=%06X fb=%06X'
                         % (st.get(0x1F, 0) & 1, st.get(0xCD, 0), st.get(0xCE, 0), st.get(0xCF, 0),
                            st.get(0x27, 0) & 1, st.get(0xD8, 0), st.get(0x28, 0) & 1, st.get(0xE6, 0) & 0xF,
                            st.get(0x17, 0) & 1, st.get(0xC9, 0), st.get(0xCA, 0), st.get(0xE0, 0), st.get(0xE1, 0)))
```

Check: `python -I gelist.py` with no arguments still prints its usage (syntax OK).

- [ ] **Step 2: Find the moment in PPSSPP and the runtime, side by side**

```bash
cd DECO/build/tools
TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 \
  python gfx_tour.py --to after_reward --keep --serve --out ../tour/white
```

This boots both sides with `build/tour_save`, plays the training mission and stops at the
reward screen (`31_after_reward`). In another shell, step forward in small pieces and look at each
stacked image (runtime on top, PPSSPP below) in `build/tour/step/`:

```bash
python tstep.py w01 X 1.0      # leave the reward screen
python tstep.py w02 W1 0.5     # repeat W1 steps (w03, w04, ...) until the hideout appears
```

Expected: at some step the PPSSPP half shows the units white in the hideout and the runtime half
does not. Write down the step names. If the white phase is shorter than one step, use `W0.3`.
If the stacked image never shows the white phase, read "If the white phase is not reached" below.

- [ ] **Step 3: Dump the display list of a white frame on both sides**

As soon as a step shows the white units, queue a list dump (the next list each side enqueues):

```bash
echo '{"gelist": 1}' > ../tour/cmd/wdl.txt
```

Output: `build/tour/step/wdl_pp.txt` and `wdl_rt.txt`. Copy both and the step PNGs to
`build/tour/white/`.

- [ ] **Step 4: Read the lists**

In `wdl_pp.txt`, take the PRIMs with `thr=0` (transformed) whose texture is a Patapon/Hero/NPC part
(the same `ta=` as in a normal hideout frame, `build/tour/relevo_base_pp`). Expected for the fog
hypothesis: `fog=1`, `fc=FFFFFF` (or near white), and `f1`/`f2` that make
`(view_z + float24(f1)) * float24(f2) <= 0` for those models. Compare `wdl_rt.txt`: the same
registers should be there (the runtime walks the same guest list), which proves the game sets them
and the runtime ignores them. If `fog=0` on those PRIMs, check `cte`/`ct`, `lop`/`lo`, `tfn`/`tenv`,
`lit`, `fa`/`fb` and `bm` the same way.

Float24 → float in Python: `struct.unpack('<f', struct.pack('<I', (x & 0xFFFFFF) << 8))[0]`.

- [ ] **Step 5: Tie the PRIMs to the white pixels**

Run the same dump on a normal hideout frame (after the white phase ends): the suspect registers
must differ between the two dumps for the same textures (for fog: `fog=1` or a white `fc` only in
the white frame, or `f1`/`f2` moving over the fade). Keep a crop of one Patapon from both sides of
the white step image in `build/tour/white/`. The proof that skipping is not needed: Task 4 Step 5
shows the runtime turning white with the fix.

- [ ] **Step 6: Write the finding and stop the tour**

Add a section "Task 1 finding (date)" to this plan: which feature, the PRIM lines (2-3 lines from
each side), the decoded FOG1/FOG2/FOGCOLOR values, and whether the effect is the same after a lost
mission (if you could not reach a lost mission, say so; the user can check it when playing).
Stop serve mode: `touch ../tour/cmd/STOP`.

- [ ] **Step 7: Commit the finding**

```bash
cd DECO/psprecomp
git add docs/superpowers/plans/2026-10-16-hideout-white-flash.md
git commit --author="Patapon Community <rexgamer1560@gmail.com>" -m "docs(plan): hideout white flash, Task 1 finding

<one line: the feature and the evidence>

Verified: gelist dumps build/tour/white/wdl_pp.txt, wdl_rt.txt
Agent: <model>"
```

**Decision gate:** fog confirmed → Task 2. Anything else → "If it is not fog".

#### Task 1 finding (2026-10-16)

**GE fog.** The white phase is not in the hideout but at the very end of the mission, right after
the victory chant (the user's "when they finish singing and the victory song plays"): the field
darkens and the Patapons, Hero, flag and signpost turn into white silhouettes for ~2 s, then
MISIÓN COMPLETADA and the reward chest. PPSSPP paints them white; the runtime painted them black
(invisible on the dark field). The game enables fog with a white colour on those models only:

```
pp 337 p=4 n=15 vt=014783 thr=0 tex=1 ta=041A5C40 ... fog=1 f1=3F8000 f2=411FFF fc=FFFFFF cte=0 lop=0 lit=0 tfn=000100
pp 338 p=4 n=7  vt=014783 thr=0 tex=1 ta=041A5C40 ... fog=1 f1=3F8000 f2=411FFF fc=FFFFFF cte=0 lop=0 lit=0 tfn=000100
rt 337 p=4 n=15 vt=014783 thr=0 tex=1 ta=041A5C40 ... fog=1 f1=3F8000 f2=411FFF fc=FFFFFF cte=0 lop=0 lit=0 tfn=000100
rt 338 p=4 n=7  vt=014783 thr=0 tex=1 ta=041A5C40 ... fog=1 f1=3F8000 f2=411FFF fc=FFFFFF cte=0 lop=0 lit=0 tfn=000100
```

FOG1 = float24 0x3F8000 = 1.0, FOG2 = 0x411FFF ≈ 9.99, FOGCOLOR = 0xFFFFFF: factor
`(view_z + 1) * 10` is <= 0 for every model at view z <= -1, i.e. the whole unit is the fog colour.
White frame: 54 fogged PRIMs (pp) / 47 (rt, the dump caught a slightly different frame); normal
field frame before the end: 0 fogged PRIMs, FOG1/FOG2/FOGCOLOR still 0 on both sides. Colour test,
logic op, lighting and the texture function are the same in both dumps (cte=0, lop=0, tfn=000100).
Lost mission: not reached in this session (the user says the effect happens on every return).
Evidence: `build/tour/white/dump/{normal,white0,white1,white2}_{pp,rt}.{txt,png}`,
`pp_sheet.png`, `rt_sheet.png`; PPSSPP frame sequence `build/tour/white2/seq`.

How it was caught: per-side tours (`gfx_tour.py --only rt|pp --to attack_combo --keep --serve`)
with a serve hook (`build/tour/cmd/wh_<side>.py.txt`) that captures every ~0.2 s and dumps the
list on the first dark frame after the bright field; the two sides reach the end ~45 s apart, so
a two-sided serve step misses it on one side.

#### If the white phase is not reached

The return to the hideout after `after_reward` may need more presses (dialogue, the AUTOSAVE
prompt). Follow the PPSSPP half: press `X` (`python tstep.py wNN X 1.0`) whenever it waits for
input. Never hold buttons for long (`B 4000 30` style short presses; long holds stall on the reward
chest). For a lost mission: in a mission, `START` opens the pause menu; pick the retreat option
there, if the game offers one, and follow it back to the hideout the same way.

#### If it is not fog

Write the finding (Step 6), commit it, and stop. Rewrite Tasks 2-4 for the feature you found with
the same shape (pure helper + unit tests from the PPSSPP source lines, then the GL path, then the
gates). Pointers:
- Colour test: `GPU/Common/FragmentShaderGenerator.cpp` (`enableColorTest`), `GPU/GPUState.h`
  (`getColorTestFunction`, `getColorTestRef`, `getColorTestMask`).
- Logic op: `FragmentShaderGenerator.cpp:172-190` (`replaceLogicOp`, `SimulateLogicOpType`),
  `GPU/Common/GPUStateUtils.cpp` (`ConvertLogicOpToBlendState` / `SimulateLogicOpShaderTypeIfNeeded`).
- Stencil → alpha: step 5 of `2026-10-14-ge-stencil.md`.

---

### Task 2: Pure fog helper (TDD)

**Files:**
- Create: `psprecomp/runtime/include/psp_ge_fog.h`
- Create: `psprecomp/runtime/tests/test_ge_fog.cpp`
- Modify: `psprecomp/runtime/CMakeLists.txt` (register the test next to `test_ge_stencil`, ~line 682)

**Interfaces:**
- Produces (all `inline`, header-only, no GL):
  - `float ge_float24(uint32_t data)` — 24-bit GE value → float (PPSSPP `getFloat24`).
  - `float ge_fog_coef(uint32_t data)` — `ge_float24`, non-finite → ±65535 (PPSSPP `UpdateFogCoef`).
  - `bool ge_fog_active(bool fog_enable, bool through)` — `fog_enable && !through`.
  - `float ge_fog_factor(float view_z, float fog1, float fog2)` — `(view_z + fog1) * fog2`
    (unclamped; 1 = no fog, <= 0 = full fog colour).
  - `void ge_fog_color_rgb(uint32_t fogcolor, float out[3])` — 0xBBGGRR → r,g,b in 0..1.
  - `void ge_fog_mix(const float fog_rgb[3], float factor, float rgb[3])` — CPU twin of the shader
    line, used only by tests: `rgb = mix(fog, rgb, clamp(factor, 0, 1))`.

- [ ] **Step 1: Write the failing tests**

`runtime/tests/test_ge_fog.cpp`, in the style of `test_ge_stencil.cpp` (plain `main`, a `CHECK`
macro, returns non-zero on failure):

```cpp
// test_ge_fog.cpp -- GE fog helpers against PPSSPP (ShaderUniforms.cpp:255 UpdateFogCoef,
// VertexShaderGenerator.cpp:904 fog depth, FragmentShaderGenerator.cpp:866 mix, ShaderId.cpp:326).
#include "psp_ge_fog.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void float24_decodes_top_bits() {
    CHECK(near(ge_float24(0x3F8000), 1.0f));     // 0x3F800000
    CHECK(near(ge_float24(0xC00000), -2.0f));    // 0xC0000000
    CHECK(near(ge_float24(0x000000), 0.0f));
    CHECK(near(ge_float24(0xFF3F8000), 1.0f));   // command byte ignored
}
static void fog_coef_nonfinite_clamped() {
    CHECK(near(ge_fog_coef(0x7F8000), 65535.0f));    // +inf
    CHECK(near(ge_fog_coef(0xFF8000), -65535.0f));   // -inf
    CHECK(std::isfinite(ge_fog_coef(0x7FC000)));     // NaN -> finite
    CHECK(near(ge_fog_coef(0x3F8000), 1.0f));        // finite passes through
}
static void fog_inactive_in_through_mode() {
    CHECK(ge_fog_active(true, false));
    CHECK(!ge_fog_active(true, true));
    CHECK(!ge_fog_active(false, false));
}
static void fog_factor_matches_ppsspp() {
    CHECK(near(ge_fog_factor(-10.0f, 20.0f, 0.1f), 1.0f));   // (-10+20)*0.1
    CHECK(near(ge_fog_factor(-10.0f, 10.0f, 0.5f), 0.0f));   // at the fog end: full fog
    CHECK(ge_fog_factor(-50.0f, 10.0f, 0.5f) < 0.0f);         // past it: clamps to full fog
}
static void fog_mix_white() {
    float white[3]; ge_fog_color_rgb(0xFFFFFF, white);
    float rgb[3] = {0.2f, 0.4f, 0.6f};
    ge_fog_mix(white, -3.0f, rgb);                  // full fog
    CHECK(near(rgb[0], 1.0f) && near(rgb[1], 1.0f) && near(rgb[2], 1.0f));
    float keep[3] = {0.2f, 0.4f, 0.6f};
    ge_fog_mix(white, 2.0f, keep);                  // no fog
    CHECK(near(keep[0], 0.2f) && near(keep[1], 0.4f) && near(keep[2], 0.6f));
    float half[3] = {0.0f, 0.0f, 0.0f};
    ge_fog_mix(white, 0.25f, half);                 // 75 % fog colour
    CHECK(near(half[0], 0.75f));
}
static void fog_color_byte_order() {
    float c[3]; ge_fog_color_rgb(0x0000FF, c);      // R in byte 0
    CHECK(near(c[0], 1.0f) && near(c[1], 0.0f) && near(c[2], 0.0f));
    ge_fog_color_rgb(0xFF0000, c);                  // B in byte 2
    CHECK(near(c[0], 0.0f) && near(c[2], 1.0f));
}

int main() {
    float24_decodes_top_bits();
    fog_coef_nonfinite_clamped();
    fog_inactive_in_through_mode();
    fog_factor_matches_ppsspp();
    fog_mix_white();
    fog_color_byte_order();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_ge_fog: all passed\n");
    return 0;
}
```

CMake, next to `test_ge_stencil`:

```cmake
add_executable(test_ge_fog tests/test_ge_fog.cpp)
target_include_directories(test_ge_fog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_fog PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_fog_tests COMMAND test_ge_fog)
```

- [ ] **Step 2: Run it, watch it fail**

Run: `cmake --build DECO/build/rtw --target test_ge_fog`
Expected: compile error, `psp_ge_fog.h: No such file or directory`.

- [ ] **Step 3: Write the header**

```cpp
// psp_ge_fog.h -- GE fog, the PPSSPP hardware-renderer way. A transformed vertex gets
// f = (view z + FOG1) * FOG2 (VertexShaderGenerator.cpp:904); the fragment mixes
// rgb = mix(FOGCOLOR, rgb, clamp(f, 0, 1)) after the texture function (FragmentShaderGenerator.cpp:866).
// Through-mode draws are never fogged (ShaderId.cpp:326).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// GE float24: the register's 24 bits are the top 24 bits of an IEEE float (PPSSPP getFloat24).
inline float ge_float24(uint32_t data) {
    const uint32_t bits = (data & 0xFFFFFFu) << 8;
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// FOG1 / FOG2 as the shader uses them: the PSP ignores inf/NaN, PPSSPP maps them to +-65535
// keeping the sign (ShaderUniforms.cpp:255 UpdateFogCoef).
inline float ge_fog_coef(uint32_t data) {
    const float f = ge_float24(data);
    if (std::isfinite(f)) return f;
    return std::signbit(f) ? -65535.0f : 65535.0f;
}

inline bool ge_fog_active(bool fog_enable, bool through) { return fog_enable && !through; }

// Unclamped; 1 = no fog, <= 0 = only the fog colour. The shader clamps.
inline float ge_fog_factor(float view_z, float fog1, float fog2) { return (view_z + fog1) * fog2; }

// FOGCOLOR is 0xBBGGRR.
inline void ge_fog_color_rgb(uint32_t fogcolor, float out[3]) {
    out[0] = static_cast<float>(fogcolor & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((fogcolor >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((fogcolor >> 16) & 0xFFu) / 255.0f;
}

// CPU twin of the fragment-shader line, for tests.
inline void ge_fog_mix(const float fog_rgb[3], float factor, float rgb[3]) {
    const float t = factor < 0.0f ? 0.0f : (factor > 1.0f ? 1.0f : factor);
    for (int i = 0; i < 3; ++i) rgb[i] = fog_rgb[i] + (rgb[i] - fog_rgb[i]) * t;
}
```

- [ ] **Step 4: Run it, watch it pass**

Run: `cmake --build DECO/build/rtw --target test_ge_fog && ctest --test-dir DECO/build/rtw -R ge_fog`
Expected: `test_ge_fog: all passed`, 1/1.

- [ ] **Step 5: Commit**

```bash
git add runtime/include/psp_ge_fog.h runtime/tests/test_ge_fog.cpp runtime/CMakeLists.txt
git commit --author="Patapon Community <rexgamer1560@gmail.com>" -m "feat(ge): fog helpers (float24 coefs, factor, colour), PPSSPP semantics

Verified: ctest -R ge_fog 1/1
Agent: <model>"
```

---

### Task 3: Fog factor per vertex (CPU transform + vertex layout)

**Files:**
- Modify: `runtime/include/psp_ge.h:136-138` (`GeState`: add `float fog1, fog2;`)
- Modify: `runtime/src/psp_ge.cpp:770-772` (store FOG1/FOG2 through `ge_fog_coef`)
- Modify: `runtime/include/psp_ge_vertex.h:9-18` (`DecodedVertex`: add `float fog = 1.0f;`)
- Modify: `runtime/src/psp_ge_vertex.cpp` `ge_transform_vertices` (~line 611-740)
- Modify: `runtime/src/psp_ge_vertex.cpp` `ge_expand_rectangle` (~line 775) only if it does not
  copy whole `DecodedVertex` values
- Test: `runtime/tests/test_ge_vertex.cpp` (existing)

**Interfaces:**
- Consumes: `ge_fog_coef`, `ge_fog_active`, `ge_fog_factor` (Task 2).
- Produces: `DecodedVertex::fog` (float, 1.0 = no fog) filled for every transformed vertex when
  `ge_fog_active(state.fog_enable, through)`; `GeState::fog1`, `GeState::fog2` (decoded floats).

- [ ] **Step 1: Write the failing tests** (add to `test_ge_vertex.cpp`; it already has
  `ASSERT_NEAR(actual, expected, msg)`, `make_perspective_state()` (identity world/view, clip.w = -z),
  `make_through_state(bool)` and `make_pos_vertex(x, y, z)`)

```cpp
// Fog: (view z + FOG1) * FOG2 per transformed vertex (PPSSPP SoftwareTransformCommon.cpp:355);
// fog off, through mode and the NDC fallback stay at 1.
static void test_transform_fills_fog_factor() {
    GeState s = make_perspective_state();
    s.fog_enable = true;
    s.fog1 = 10.0f;
    s.fog2 = 0.5f;
    std::vector<DecodedVertex> verts = {make_pos_vertex(0.0f, 0.0f, -10.0f)};   // view z = -10
    ge_transform_vertices(verts, s);
    ASSERT_NEAR(verts[0].fog, 0.0f, "fog: (-10 + 10) * 0.5");
    std::vector<DecodedVertex> near_v = {make_pos_vertex(0.0f, 0.0f, -2.0f)};
    ge_transform_vertices(near_v, s);
    ASSERT_NEAR(near_v[0].fog, 4.0f, "fog: (-2 + 10) * 0.5, unclamped");
}
static void test_fog_off_leaves_factor_one() {
    GeState s = make_perspective_state();
    s.fog_enable = false; s.fog1 = 10.0f; s.fog2 = 0.5f;
    std::vector<DecodedVertex> verts = {make_pos_vertex(0.0f, 0.0f, -10.0f)};
    ge_transform_vertices(verts, s);
    ASSERT_NEAR(verts[0].fog, 1.0f, "fog off: factor 1");
}
static void test_fog_inactive_through() {
    GeState s = make_through_state(false);
    s.fog_enable = true; s.fog1 = 10.0f; s.fog2 = 0.5f;
    std::vector<DecodedVertex> verts = {make_pos_vertex(10.0f, 10.0f, 0.0f)};
    ge_transform_vertices(verts, s);
    ASSERT_NEAR(verts[0].fog, 1.0f, "through mode: never fogged");
}
// PPSSPP ExpandRectangles (SoftwareTransformCommon.cpp:1037-1050) starts every corner from the
// bottom-right vertex, so the whole rectangle takes v1's fog.
static void test_rect_expand_keeps_fog() {
    DecodedVertex v0 = make_pos_vertex(-0.5f, 0.5f, -1.0f);
    DecodedVertex v1 = make_pos_vertex(0.5f, -0.5f, -1.0f);
    v0.w = 1.0f; v1.w = 1.0f;
    v0.fog = 0.25f; v1.fog = 0.75f;
    DecodedVertex out[6];
    tests_run++;
    if (!ge_expand_rectangle(v0, v1, out)) {
        std::fprintf(stderr, "FAIL: rect fog: expansion dropped the rectangle\n");
        failures++;
        return;
    }
    for (int k = 0; k < 6; k++) ASSERT_NEAR(out[k].fog, 0.75f, "rect: every corner takes v1 fog");
}
static void test_fallback_path_has_no_fog() {
    GeState s = make_perspective_state();
    for (float& m : s.view_matrix) m = 0.0f;   // all-zero view -> fallback (psp_ge_vertex.cpp:375)
    s.fog_enable = true; s.fog1 = 10.0f; s.fog2 = 0.5f;
    std::vector<DecodedVertex> verts = {make_pos_vertex(0.0f, 0.0f, -10.0f)};
    ge_transform_vertices(verts, s);
    ASSERT_NEAR(verts[0].fog, 1.0f, "NDC fallback: no fog");
}
```

Call the five from the file's `main` next to the other `test_*` calls.

- [ ] **Step 2: Run it, watch it fail**

Run: `cmake --build DECO/build/rtw --target test_ge_vertex`
Expected: compile error, `'struct DecodedVertex' has no member named 'fog'` (and `fog1`/`fog2`
missing from `GeState`).

- [ ] **Step 3: Implement**

`psp_ge.h`, in the fog block:

```cpp
    // -- Fog --
    bool fog_enable;
    uint32_t fog_color;
    float fog1;   // GE_CMD_FOG1 fog end (float24, non-finite -> +-65535; ge_fog_coef)
    float fog2;   // GE_CMD_FOG2 fog slope
```

`psp_ge.cpp:770`, replace the two-case `break` with:

```cpp
        case GE_CMD_FOG1:
            g_ge_state.fog1 = ge_fog_coef(data);
            break;
        case GE_CMD_FOG2:
            g_ge_state.fog2 = ge_fog_coef(data);
            break;
```

and `#include "psp_ge_fog.h"` at the top of `psp_ge.cpp`.

`psp_ge_vertex.h` `DecodedVertex`, after `w`:

```cpp
    float fog = 1.0f;   // GE fog factor (view z + FOG1) * FOG2, unclamped; 1 = no fog
```

`psp_ge_vertex.cpp` `ge_transform_vertices`: compute once at the top
`const bool fog_on = ge_fog_active(state.fog_enable, through);`. In the transformed branch, at the
point where `vpos` (view-space position) is known and the projection branch writes `v.w = clip[3]`
(~line 729), add `if (fog_on) v.fog = ge_fog_factor(vpos[2], state.fog1, state.fog2);`. Leave
`v.fog` at its default 1.0 on the through branch and on the degenerate/NDC-direct fallback branch
(`v.w = 1.0f; // positions are already NDC`). Skinning/morph paths that end in the same projection
code get it automatically; check that no other branch writes clip-space positions.

The shader turns fog on per draw, not per vertex, so every vertex of a fogged draw that this code
does not fill must hold exactly 1.0. Check that `ge_decode_vertices` and the bezier/spline path
(`psp_ge_patch.cpp`) create `DecodedVertex` values without `memset` (a zeroed `fog` would paint the
whole draw in the fog colour), and that patch vertices go through `ge_transform_vertices`.

`ge_expand_rectangle` (`psp_ge_vertex.cpp:775`) copies `a` (from `v0`) and `b` (from `v1`) into the
corners, so today the `a`/`c3` corners would carry `v0`'s fog. PPSSPP builds every corner from the
bottom-right vertex. Right after `b.w = 1.0f;` add:

```cpp
    // Fog is flat across a rectangle: PPSSPP builds every corner from the bottom-right
    // vertex (SoftwareTransformCommon.cpp:1037-1050).
    a.fog = v1.fog;
```

(`c1` and `c3` are copies of `b` and `a`, so all six outputs get `v1.fog`.)

- [ ] **Step 4: Run it, watch it pass**

Run: `cmake --build DECO/build/rtw --target test_ge_vertex && ctest --test-dir DECO/build/rtw -R "ge_vertex|ge_fog"`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add runtime/include/psp_ge.h runtime/src/psp_ge.cpp runtime/include/psp_ge_vertex.h runtime/src/psp_ge_vertex.cpp runtime/tests/test_ge_vertex.cpp
git commit --author="Patapon Community <rexgamer1560@gmail.com>" -m "feat(ge): per-vertex fog factor from the view-space z

Verified: ctest -R ge_vertex|ge_fog
Agent: <model>"
```

---

### Task 4: Fog in the shader, then the gates

**Files:**
- Modify: `runtime/src/psp_ge_draw.cpp:94-103` (`PackedVertex`: add `float fog;`, 28 → 32 bytes),
  `:405-426` (attribute location 3), and every place that builds a `PackedVertex`
  (`grep -n "PackedVertex\|packed.push_back\|packed\[i\] =" src/psp_ge_draw.cpp`; at least the
  rectangle loop ~line 925 and the plain loop ~line 937)
- Modify: `runtime/src/psp_ge_shader.cpp` (vertex shader `a_fog`/`v_fog`; fragment `u_fog_enable`,
  `u_fog_color`; uniform locations; `ge_shader_set_uniforms`)
- Modify: `runtime/src/psp_ge_draw.cpp` (drawlog/debug prints that print `PackedVertex` fields, if
  any print by position, keep compiling)
- Docs: `psprecomp/ARCHITECTURE.md` (GE section: fog now applied), this plan (evidence)

**Interfaces:**
- Consumes: `DecodedVertex::fog`, `GeState::fog_enable`, `GeState::fog_color` (Task 3),
  `ge_fog_active`, `ge_vtype_through` (existing).

- [ ] **Step 1: Make the size change fail first**

Change the `static_assert` to the new size before touching the struct:

```cpp
static_assert(sizeof(PackedVertex) == 32,
              "PackedVertex must be 32 bytes");
```

Run: `cmake --build DECO/build/rtw`
Expected: compile error on the static_assert (28 != 32).

- [ ] **Step 2: Vertex layout**

```cpp
// ---- Packed vertex for VBO upload ----
// Layout: 4 floats (clip pos) + 2 floats (uv) + 4 bytes (color) + 1 float (fog factor)
// Total: 16 + 8 + 4 + 4 = 32 bytes
struct PackedVertex {
    float pos[4];   // Clip-space (x,y,z,w); GL does the perspective divide
    float uv[2];
    uint8_t color[4];
    float fog;      // GE fog factor, unclamped (1 = none); the fragment shader clamps
};
```

Attribute, after location 2:

```cpp
    // location 3: fog factor (1 float, offset 28)
    glVertexAttribPointer(
        3, 1, GL_FLOAT, GL_FALSE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(28));
    glEnableVertexAttribArray(3);
```

Each `PackedVertex` initializer gains `, v.fog` after the colour braces, for example:

```cpp
                packed.push_back({
                    {v.pos[0], v.pos[1], v.pos[2], v.w},
                    {v.uv[0], v.uv[1]},
                    {v.color[0], v.color[1],
                     v.color[2], v.color[3]},
                    v.fog
                });
```

Any `PackedVertex` built without a `DecodedVertex` (clear quads, blits, line expansion) gets `1.0f`.

- [ ] **Step 3: Shader**

Vertex shader: add `layout(location = 3) in float a_fog;`, `out float v_fog;`, and `v_fog = a_fog;`
in `main`. Fragment shader: add `in float v_fog;`, `uniform bool u_fog_enable;`,
`uniform vec3 u_fog_color;`, and right after the `if (u_texture_enable) { ... }` block, before
`// Alpha test`:

```glsl
    // Fog (PPSSPP FragmentShaderGenerator.cpp:866): after the texture function and doubling,
    // before the alpha test. 1 = no fog, 0 = only the fog colour.
    if (u_fog_enable) {
        color.rgb = mix(u_fog_color, color.rgb, clamp(v_fog, 0.0, 1.0));
    }
```

Uniform locations next to the others (`g_uniforms.u_fog_enable = glGetUniformLocation(g_program,
"u_fog_enable");`, same for `u_fog_color`, add both fields to `ShaderUniforms`), and in
`ge_shader_set_uniforms`:

```cpp
    const bool fog_on = ge_fog_active(state.fog_enable, ge_vtype_through(state.vertex_type));
    gl_state_uniform1i(g_uniforms.u_fog_enable, fog_on ? 1 : 0);
    if (fog_on) {
        float fc[3];
        ge_fog_color_rgb(state.fog_color, fc);
        gl_state_uniform3f(g_uniforms.u_fog_color, fc[0], fc[1], fc[2]);
    }
```

(`#include "psp_ge_fog.h"`; include whatever header declares `ge_vtype_through` if the shader file
does not have it yet.) Clear-mode draws: PPSSPP never fogs them (`isModeClear` builds a separate
shader); if the clear path uses this program, make sure it sets `u_fog_enable = 0` (check how the
alpha test is turned off for clears and do the same).

- [ ] **Step 4: Build, unit tests**

Run: `cmake --build DECO/build/rtw`; `ctest --test-dir DECO/build/rtw`.
Expected: builds; ctest all green (69 + `ge_fog_tests` + the new vertex cases).

- [ ] **Step 5: The white frames**

Repeat Task 1 Step 2 with the new exe (serve mode, same `tstep.py` steps). Expected: in the stacked
images the runtime half now shows the units white at the same steps as PPSSPP, and coming back to
their colours on the same timing. Save the step images to `build/tour/white/after/`.

- [ ] **Step 6: Gates**

- `rt_regress.py` against `relevo_base` with `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`: static points
  0.00 %. If a static point moves, compare that point with PPSSPP: a change toward PPSSPP (a scene
  that really uses fog) is the fix, a change away from it is a bug. Write which points moved and why.
- Full PPSSPP tour (`gfx_tour.py --out ../tour/fog_after`) + `gfx_score.py`: no checkpoint worse than
  `build/tour/relevo_base_pp`; list the checkpoints that got closer (fogged scenes).
- `perf_hideout.py`: interleaved A/B against the current exe (Defender skews sequential runs). The
  extra 4 bytes per vertex and one `mix` should not show; any change above noise is a finding.
- A scale 4 + HD pack run of the return to the hideout (the user's settings) to check the fog
  colour also applies to replaced textures (it should: fog runs after the texture sample).

- [ ] **Step 7: Docs and commit**

ARCHITECTURE.md GE section: one paragraph (fog factor on the CPU in `ge_transform_vertices`, carried
in `PackedVertex::fog`, mixed in the fragment shader; through mode never fogged; PPSSPP lines).
Evidence section at the end of this plan (finding, before/after images, gates). Then:

```bash
git add runtime/src/psp_ge_draw.cpp runtime/src/psp_ge_shader.cpp ARCHITECTURE.md docs/superpowers/plans/2026-10-16-hideout-white-flash.md
git commit --author="Patapon Community <rexgamer1560@gmail.com>" -m "feat(ge): apply GE fog (hideout white flash after a mission)

Verified: ctest <N>/<N>; rt_regress <result>; gfx_score <result>; white frames match PPSSPP (build/tour/white/after)
Agent: <model>"
```

- [ ] **Step 8: Hand over to the user**

Rebuild `build/rtw/psprecomp_runtime.exe` (it is the user's play exe), tell the user in Spanish
what changed and ask them to finish a mission (one won and, if they can, one lost) and watch the
return to the hideout. No push until they confirm, and the push leaves out `f639180`.

## Risks

- **Fog everywhere.** Any scene that enables fog changes at once (backgrounds getting a distance
  tint, the world map). That is PSP-correct; the tour comparison in Task 4 Step 6 is how we tell
  correct from wrong.
- **Wrong z sign or space.** PPSSPP uses the view-space z (after world·view, before projection).
  Using the clip z or NDC z gives a wrong factor everywhere; the unit test pins the formula, the
  white frames pin the space.
- **Shader cost.** One extra varying and a `mix`; the runtime is CPU-bound in the hideout
  (`perf_hideout`), so no visible cost is expected. Measure, do not assume.
- **Rectangles.** Sprites are rectangles in transform mode and go through `ge_expand_rectangle`;
  missing the fog there would leave exactly the Patapon parts unfogged (Review Focus 3).
