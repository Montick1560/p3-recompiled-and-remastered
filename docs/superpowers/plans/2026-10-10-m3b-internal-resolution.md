# M3b — Internal render resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Task 1 is a gate: finish it (and record its result at the end of this file) before Task 2.

**Goal:** Render the game at N× the PSP resolution (`PSPRECOMP_RENDER_SCALE=1..8`, e.g. 4 = 1920x1088), so it looks sharp in a large window or in fullscreen. Scale 1 must stay pixel-identical to today, because every oracle comparison against PPSSPP runs at scale 1.

**Architecture:** The runtime draws each guest framebuffer address into its own GL FBO (`runtime/src/psp_ge_draw.cpp`, pool of 4), currently fixed at 480x272. Through-mode (2D) vertices are already mapped to NDC (`psp_ge_vertex.cpp:612`, `/240 - 1`, `1 - /136`), and transform-mode draws go through `ge_compute_viewport_depth`. So only the pixel-space quantities need the scale factor: FBO sizes, `glViewport`, `glScissor`, the present blit source rectangle, and the screenshot readbacks. The two pieces of math (viewport and scissor) are pure, unit-tested functions in `psp_ge_viewport.cpp`. The GL wiring is one file. There is no render-to-texture in the runtime (branch `wt-rtt` is unmerged), so no texture path changes.

**Tech Stack:** C++17, OpenGL 3.3 core, SDL2, ctest (standalone test executables), Rust `cargo test` (recompiler), debug socket TCP 9999.

**Spec:** `docs/superpowers/specs/2026-10-07-patapon3-native-port-design.md`. Line 13 puts resolution enhancements "out of scope until after M7". The user overrode that on 2026-10-09: the roadmap agreed with them is recorded at the end of `docs/superpowers/plans/2026-10-09-m3-graphics-rtt.md` ("Roadmap after M3a", item 2 = this plan). Roadmap item 1 (TEXFILTER/TEXWRAP, resizable window, F11 fullscreen, linear present) is done (HEAD b68f7d4 and earlier).

## Where things stand (read first; the session that wrote this plan was cleared)

- Repo `C:/Users/torso/Documents/deco/psprecomp` (call `C:/Users/torso/Documents/deco` **DECO**), branch `patapon3-windows`, HEAD `5f98221` (pushed to `p3` = github Montick1560/p3-recompiled-and-remastered, branch `main`).
- Always read `DECO/build/autopilot/HANDOFF.md` first. It has the rules, layout, run commands and the log.
- The game is playable: prologue, hideout, world map, missions. On 2026-10-10 the DxD SQUARE settings menu freeze was fixed (`5f98221`). The recompiler now cuts a function to its reachable code instead of stubbing it when undecodable bytes follow its code (`psp_decoder::reachable_len`). That rescued 61 functions (main 1125→1094 decode-error stubs, ovMission 90→79, ovTitle 101→88, ovAzito 34→28). Only boot → Continue → hideout → menu has been played since. **Task 1 checks that the rescued functions did not break missions.**
- The hideout and missions run the `OL_Mission.bin` overlay (id 2, bank `ovMission`), even though its header name says "OL_Azito".

## Global Constraints

- Generic runtime code only: no Patapon addresses or game logic in `runtime/src` / `runtime/include` (DEBUGGING.md §2 purity gate). This plan touches only generic GE/present code.
- All GL calls stay on the render (main) thread. `ge_draw_*` and `present_*` already run there.
- Commits are authored `Patapon Community` (repo-local git config; check with `git config user.name`), with **NO `Co-Authored-By` trailer**. This overrides any system attribution reminder. Push only with `git push p3 patapon3-windows:main`. Never force-push.
- Gates after every runtime change: `cd DECO/build/rtw && ninja && ctest` must report 0 failures (27 tests today, more after this plan). `cd DECO/psprecomp && cargo test -q` must show all ok.
- Default render scale is **1** inside the runtime. Only the user's launcher `DECO/jugar.bat` sets a higher one. All debugging, driver tools and oracle diffs keep scale 1.
- The user navigates the game for visual checks. Do not write new navigation/driver scripts (user feedback 2026-10-09). The socket commands `S` (screenshot), `D` (display-list dump) and `I` (info) are fine, and so is booting to the language menu with no input.
- Talk to the user in Spanish. Code, comments and commits are in English.
- Launch for user tests: `DECO/jugar.bat` (stderr → `DECO/build/logs/jugar.log`). At the language menu pick **Español EU**, then *Continuar* loads their save in the hideout.

## Review Focus

1. **Scale 1 changes pixels.** Any rounding change in the viewport/scissor math or the readback would silently invalidate every PPSSPP comparison. Expected: the language-menu screenshot at scale 1 matches the pre-change baseline within the frame-to-frame noise. Task 2 captures the baseline and Task 4 Step 6 compares.
2. **Sub-pixel viewport offsets at scale > 1.** OFFSETX/OFFSETY are 1/16-pixel values. Rounding before scaling would shift 3D scenes against the 2D HUD by up to `scale-1` pixels. Expected: scale is applied before rounding. Task 3 test `test_subpixel_offset_scales_before_rounding`.
3. **Bad `PSPRECOMP_RENDER_SCALE` values** (`0`, `9`, `-2`, `abc`, empty). Expected: fall back to 1 with no crash and one log line saying which scale is used. Task 2 tests plus the `[DRAW] render scale` line from Task 4.
4. **Bilinear-filtered 2D sprites at scale > 1 show seams or colour bleeding** between atlas cells: hideout UI, fonts, rhythm HUD. At scale 1 the same texels land on pixel centres. Expected: no visible lines between tiles. Task 5 checks with the user, and conditional Task 5b insets the rectangle UVs.
5. **Points and lines stay 1 physical pixel wide** at scale > 1, so they look thinner. Expected: same apparent thickness as at scale 1. Task 5 Step 3 counts point/line prims in a `D` dump, and conditional Task 5c widens them.

---

### Task 1: Regression gate for the reachable-cut change (5f98221)

Read-only for code. Record the result at the end of this file under "Task 1 result".

**Files:**
- Create (scratch, not committed): `DECO/build/logs/reachable_cuts.txt`

- [ ] **Step 1: List every function the cut rescued.** The recompile prints one INFO line per cut function. Run the four recompiles into scratch dirs so the real outputs stay untouched:

```bash
cd /c/Users/torso/Documents/deco/psprecomp
cargo build --release -q
OUTS=../build/tmp_cutcheck; rm -rf $OUTS; mkdir -p $OUTS
extra=(); for n in Azito Mission Title; do extra+=(--extra-entries ../build/ovl/ov${n}_output/main_entries.json); done
PSPRECOMP_CROSS_MID=1 ./target/release/psprecomp recompile data/patapon3/analysis.json \
    --config games/patapon3/game.toml "${extra[@]}" -o $OUTS/main 2>&1 | grep "reachable bytes" | sed 's/^/main /' > ../build/logs/reachable_cuts.txt
for n in Azito Mission Title; do l=$(echo $n | tr A-Z a-z)
  PSPRECOMP_CROSS_MID=1 ./target/release/psprecomp recompile data/patapon3/ov_$l/analysis.json \
      --config games/patapon3/game.toml --bank ov$n -o $OUTS/$l 2>&1 | grep "reachable bytes" | sed "s/^/$l /" >> ../build/logs/reachable_cuts.txt
done
rm -rf $OUTS
wc -l ../build/logs/reachable_cuts.txt
```
Expected: about 61 lines (31 main, 11 mission, 13 title, 6 azito), each like `mission ... FUN_08BC428C @ 0x08BC428C: cut to its 0x28 reachable bytes (data follows)`.

- [ ] **Step 2: Skim the list for data that was cut into "code".** For each line, check the cut length. A cut of 4–8 bytes is a lone `jr ra; nop`, which is harmless. For anything 0x40 bytes or longer, disassemble it to confirm it looks like real code (a prologue, `jr ra` at the end):

```bash
python -I tools/mdis.py <start hex> <start+len hex> /c/Users/torso/Documents/deco/INFN00001_EBOOT.BIN      # main
python -I tools/mdis.py <start hex> <start+len hex> /c/Users/torso/Documents/deco/build/ovl/mission.elf    # banks (azito.elf / title.elf)
```
For addresses in an overlay's data section, `mdis.py` prints `--` because the ELF lacks the data. Read live memory instead with the runtime running (debug socket `R <hexaddr> <len>`). Write any address that looks like data (ASCII words, no control flow) into the result section. No fix is needed unless Step 3 shows a problem.

- [ ] **Step 3: Ask the user to play.** In Spanish: "Abre `jugar.bat`, elige Español EU → Continuar, entra a una misión desde el obelisco y termínala; luego vuelve al hideout, abre el menú con CUADRADO y ciérralo. Dime 'listo' o qué falló." While they play, nothing needs to run on our side.

- [ ] **Step 4: Check the log.**

```bash
cd /c/Users/torso/Documents/deco
grep -a -E "LOOKUP_MISS\]|FATAL|abort|Unhandled|exception" build/logs/jugar.log | grep -v DEADBEEF | sort | uniq -c | head
```
Expected: no lines (the `0xDEADBEEF` miss is the runtime self-test). If the user reports a freeze or crash: ask them not to close the window, take `python -I build/tools/info.py` (threads and wait states), and check `K <tid>` for the stuck thread. Compare that thread's functions with `reachable_cuts.txt` before suspecting anything else.

- [ ] **Step 5: Record the result** under "Task 1 result" (date, what the user played, log result, suspicious cuts if any) and add one line to the HANDOFF.md log. If a rescued function is the culprit, stop this plan and fix that first (TDD in `crates/psp-decoder/src/tests/reachable_tests.rs`).

### Task 2: Baseline capture, diff tool and the `PSPRECOMP_RENDER_SCALE` parser

**Files:**
- Create: `tools/tgadiff.py`
- Modify: `runtime/include/psp_present.h`, `runtime/src/psp_present.cpp`
- Test: `runtime/tests/test_present.cpp` (already registered in `runtime/CMakeLists.txt` as `present_tests`)

**Interfaces:**
- Produces: `int present_render_scale(const char* env);` returns 1..8, falling back to 1. Produces `tools/tgadiff.py <a.tga> <b.tga>`, which prints `differing pixels: N of M` and exits 0 (or 2 when the sizes differ).

- [ ] **Step 1: Capture the scale-1 baseline BEFORE any runtime change.** Use the current exe (built from `5f98221`), boot with no input and screenshot the language menu twice, 1 s apart:

```bash
cd /c/Users/torso/Documents/deco/build/rtw
taskkill //IM psprecomp_runtime.exe //F 2>/dev/null
PSPRECOMP_DISC0=C:/Users/torso/Documents/deco/disc0 ./psprecomp_runtime.exe > ../logs/m3b_base.log 2>&1 &
until grep -q "DATAMS.BND" ../logs/m3b_base.log; do sleep 1; done; sleep 8
for n in base_a base_b; do
  python -I -c "import socket,sys;s=socket.create_connection(('127.0.0.1',9999));s.sendall(('S C:/Users/torso/Documents/deco/build/shots/m3b_%s.tga\n'%sys.argv[1]).encode());print(s.recv(64))" $n
  sleep 1
done
taskkill //IM psprecomp_runtime.exe //F
```
Expected: `b'OK 0\n'` twice, and `DECO/build/shots/m3b_base_a.tga` and `m3b_base_b.tga` exist (480x272).

- [ ] **Step 2: Write `tools/tgadiff.py`.**

```python
"""tgadiff.py <a.tga> <b.tga>: count differing pixels between two uncompressed TGAs
(as written by the runtime's `S` screenshot). Exit 2 if the sizes differ."""
import struct
import sys


def load(path):
    d = open(path, 'rb').read()
    idl, bpp = d[0], d[16]
    w, h = struct.unpack('<HH', d[12:16])
    n = w * h * (bpp // 8)
    return w, h, bpp // 8, d[18 + idl:18 + idl + n]


def main():
    wa, ha, ba, pa = load(sys.argv[1])
    wb, hb, bb, pb = load(sys.argv[2])
    if (wa, ha, ba) != (wb, hb, bb):
        print(f'size differs: {wa}x{ha}x{ba} vs {wb}x{hb}x{bb}')
        sys.exit(2)
    diff = sum(1 for i in range(0, len(pa), ba) if pa[i:i + ba] != pb[i:i + ba])
    print(f'differing pixels: {diff} of {wa * ha}')


if __name__ == '__main__':
    main()
```

- [ ] **Step 3: Measure the noise level.**

Run: `python -I tools/tgadiff.py ../build/shots/m3b_base_a.tga ../build/shots/m3b_base_b.tga` (from `DECO/psprecomp`).
Expected: `differing pixels: N0 of 130560`. N0 is the animation noise (0 if the menu is static). Write N0 into "Task 1 result" → "baseline noise".

- [ ] **Step 4: Write the failing parser tests.** In `runtime/tests/test_present.cpp`, add these lines before `if (failures == 0) std::printf(...)`:

```cpp
    CHECK(present_render_scale(nullptr) == 1, "unset render scale -> 1");
    CHECK(present_render_scale("") == 1, "empty render scale -> 1");
    CHECK(present_render_scale("1") == 1, "render scale 1");
    CHECK(present_render_scale("4") == 4, "render scale 4");
    CHECK(present_render_scale("8") == 8, "render scale 8");
    CHECK(present_render_scale("0") == 1, "render scale 0 -> 1");
    CHECK(present_render_scale("9") == 1, "render scale 9 -> 1");
    CHECK(present_render_scale("-2") == 1, "negative render scale -> 1");
    CHECK(present_render_scale("abc") == 1, "garbage render scale -> 1");
```

- [ ] **Step 5: Run it to verify it fails.**

Run: `cd /c/Users/torso/Documents/deco/build/rtw && ninja test_present`
Expected: compile error, `'present_render_scale' was not declared`.

- [ ] **Step 6: Implement.** In `runtime/include/psp_present.h`, append:

```cpp
// Internal render scale from PSPRECOMP_RENDER_SCALE: 1..8, anything else -> 1.
// 1 = native 480x272, the setting every PPSSPP comparison uses.
int present_render_scale(const char* env);
```
In `runtime/src/psp_present.cpp`, append:

```cpp
int present_render_scale(const char* env) {
    if (env == nullptr || env[0] == '\0') return 1;
    const int s = std::atoi(env);
    return (s >= 1 && s <= 8) ? s : 1;
}
```

- [ ] **Step 7: Run the tests.**

Run: `cd /c/Users/torso/Documents/deco/build/rtw && ninja && ctest -R present_tests --output-on-failure`
Expected: `test_present: all passed`, 1/1 tests passed.

- [ ] **Step 8: Commit.**

```bash
cd /c/Users/torso/Documents/deco/psprecomp
git add tools/tgadiff.py runtime/include/psp_present.h runtime/src/psp_present.cpp runtime/tests/test_present.cpp
git commit -m "feat(present): PSPRECOMP_RENDER_SCALE parser; tools/tgadiff.py"
```

### Task 3: Scaled viewport and scissor math

**Files:**
- Modify: `runtime/include/psp_ge_draw.h:4-23` (`GeViewportDepth` block), `runtime/src/psp_ge_viewport.cpp`
- Test: `runtime/tests/test_ge_viewport.cpp` (registered as `ge_viewport_tests`; it links `src/psp_ge_viewport.cpp` only)

**Interfaces:**
- Consumes: nothing from Task 2.
- Produces:
  - `GeViewportDepth ge_compute_viewport_depth(float vp_x_scale, float vp_y_scale, float vp_x_center, float vp_y_center, float vp_z_scale, float vp_z_center, uint32_t off_x_raw, uint32_t off_y_raw, int fb_height, int scale = 1);`. `fb_height` stays in PSP pixels (272), and `x, y, w, h` come back in FBO pixels (multiplied by `scale` before rounding).
  - `struct GeScissor { bool enabled; int x, y, w, h; };` and `GeScissor ge_compute_scissor(uint32_t scissor1, uint32_t scissor2, int fb_height, int scale);`

- [ ] **Step 1: Write the failing tests.** In `runtime/tests/test_ge_viewport.cpp`, add these functions above `int main()` and call them from `main()` after `test_reversed_z_range();`:

```cpp
// Render scale (M3b): outputs are FBO pixels = PSP pixels * scale.
static void test_fullscreen_scale2() {
    GeViewportDepth v = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f, 32767.5f, 32767.5f,
        1808u * 16u, 1912u * 16u, FBH, 2);
    ASSERT_EQ(v.x, 0, "scale2 fullscreen x");
    ASSERT_EQ(v.y, 0, "scale2 fullscreen y");
    ASSERT_EQ(v.w, 960, "scale2 fullscreen w");
    ASSERT_EQ(v.h, 544, "scale2 fullscreen h");
    ASSERT_NEAR(v.near_z, 0.0f, "scale does not touch depth (near)");
    ASSERT_NEAR(v.far_z, 1.0f, "scale does not touch depth (far)");
}

// Centred half-size viewport: PSP rect x 120..359, y 68..203 (top-left
// origin) -> GL (bottom-left) x=120 y=68 w=240 h=136; scale 3 triples all.
static void test_half_viewport_scale3() {
    GeViewportDepth v = ge_compute_viewport_depth(
        120.0f, -68.0f, 2048.0f, 2048.0f, 32767.5f, 32767.5f,
        1808u * 16u, 1912u * 16u, FBH, 3);
    ASSERT_EQ(v.x, 360, "scale3 half x");
    ASSERT_EQ(v.y, 204, "scale3 half y");
    ASSERT_EQ(v.w, 720, "scale3 half w");
    ASSERT_EQ(v.h, 408, "scale3 half h");
}

// A quarter-pixel offset rounds away at scale 1 (left = -0.25 -> 0) but is
// a whole FBO pixel at scale 4 (-1): scale must apply before rounding.
static void test_subpixel_offset_scales_before_rounding() {
    GeViewportDepth s1 = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f, 32767.5f, 32767.5f,
        1808u * 16u + 4u, 1912u * 16u, FBH, 1);
    GeViewportDepth s4 = ge_compute_viewport_depth(
        240.0f, -136.0f, 2048.0f, 2048.0f, 32767.5f, 32767.5f,
        1808u * 16u + 4u, 1912u * 16u, FBH, 4);
    ASSERT_EQ(s1.x, 0, "quarter-pixel offset at scale 1");
    ASSERT_EQ(s4.x, -1, "quarter-pixel offset at scale 4");
}

static void test_scissor_never_set_is_disabled() {
    GeScissor s = ge_compute_scissor(0u, 0u, FBH, 2);
    ASSERT_EQ(s.enabled, false, "both registers 0 -> disabled");
}

static void test_scissor_fullscreen_scaled() {
    const uint32_t s2 = 479u | (271u << 10);
    GeScissor a = ge_compute_scissor(0u, s2, FBH, 1);
    ASSERT_EQ(a.enabled, true, "full scissor enabled");
    ASSERT_EQ(a.x, 0, "full x"); ASSERT_EQ(a.y, 0, "full y");
    ASSERT_EQ(a.w, 480, "full w"); ASSERT_EQ(a.h, 272, "full h");
    GeScissor b = ge_compute_scissor(0u, s2, FBH, 2);
    ASSERT_EQ(b.w, 960, "full w scale2"); ASSERT_EQ(b.h, 544, "full h scale2");
}

// PSP inclusive rect (10,20)-(109,69): 100x50, GL y = 272-1-69 = 202.
static void test_scissor_partial_scaled() {
    const uint32_t s1 = 10u | (20u << 10);
    const uint32_t s2 = 109u | (69u << 10);
    GeScissor a = ge_compute_scissor(s1, s2, FBH, 1);
    ASSERT_EQ(a.x, 10, "partial x"); ASSERT_EQ(a.y, 202, "partial y");
    ASSERT_EQ(a.w, 100, "partial w"); ASSERT_EQ(a.h, 50, "partial h");
    GeScissor b = ge_compute_scissor(s1, s2, FBH, 2);
    ASSERT_EQ(b.x, 20, "partial x scale2"); ASSERT_EQ(b.y, 404, "partial y scale2");
    ASSERT_EQ(b.w, 200, "partial w scale2"); ASSERT_EQ(b.h, 100, "partial h scale2");
}

static void test_scissor_inverted_is_empty() {
    GeScissor s = ge_compute_scissor(50u, 10u | (100u << 10), FBH, 2);
    ASSERT_EQ(s.enabled, true, "inverted still enabled");
    ASSERT_EQ(s.w, 0, "x2 < x1 -> zero width");
}
```
And in `main()`:

```cpp
    test_fullscreen_scale2();
    test_half_viewport_scale3();
    test_subpixel_offset_scales_before_rounding();
    test_scissor_never_set_is_disabled();
    test_scissor_fullscreen_scaled();
    test_scissor_partial_scaled();
    test_scissor_inverted_is_empty();
```

- [ ] **Step 2: Run to verify it fails.**

Run: `cd /c/Users/torso/Documents/deco/build/rtw && ninja test_ge_viewport`
Expected: compile errors (too many arguments to `ge_compute_viewport_depth`; `GeScissor` / `ge_compute_scissor` undeclared).

- [ ] **Step 3: Declare.** In `runtime/include/psp_ge_draw.h`, change the declaration's last parameter line from `int fb_height);` to:

```cpp
    int fb_height, int scale = 1);
```
Extend its doc comment with: `/// fb_height is in PSP pixels; scale (render scale, >= 1) multiplies the returned x/y/w/h, applied before rounding so sub-pixel offsets survive at scale > 1.`. Then add right after it:

```cpp
/// GE scissor (SCISSOR1/2: inclusive x1,y1 / x2,y2, 10 bits each, PSP
/// top-left origin) -> glScissor rect in FBO pixels (GL bottom-left origin)
/// for a target `scale` times the PSP size. Both registers 0 = never set:
/// disabled. fb_height is in PSP pixels.
struct GeScissor {
    bool enabled;
    int x, y, w, h;
};
GeScissor ge_compute_scissor(uint32_t scissor1, uint32_t scissor2, int fb_height, int scale);
```

- [ ] **Step 4: Implement.** In `runtime/src/psp_ge_viewport.cpp`, change the definition's signature to end with `int fb_height, int scale) {` (no default in the definition). Replace the four `out.x/y/w/h` lines with:

```cpp
    const float s = static_cast<float>(scale);
    out.x = static_cast<int>(std::lround(left * s));
    out.y = static_cast<int>(std::lround(gl_y * s));
    out.w = static_cast<int>(std::lround(w * s));
    out.h = static_cast<int>(std::lround(h * s));
```
Then append:

```cpp
GeScissor ge_compute_scissor(uint32_t scissor1, uint32_t scissor2, int fb_height, int scale) {
    if (scissor1 == 0 && scissor2 == 0) return {false, 0, 0, 0, 0};
    const int x1 = static_cast<int>(scissor1 & 0x3FF);
    const int y1 = static_cast<int>((scissor1 >> 10) & 0x3FF);
    const int x2 = static_cast<int>(scissor2 & 0x3FF);
    const int y2 = static_cast<int>((scissor2 >> 10) & 0x3FF);
    const int w = x2 >= x1 ? x2 - x1 + 1 : 0;
    const int h = y2 >= y1 ? y2 - y1 + 1 : 0;
    return {true, x1 * scale, (fb_height - 1 - y2) * scale, w * scale, h * scale};
}
```

- [ ] **Step 5: Run the tests.**

Run: `cd /c/Users/torso/Documents/deco/build/rtw && ninja && ctest --output-on-failure`
Expected: `test_ge_viewport: N/N PASS`, and every ctest passes. `psp_ge_draw.cpp` still compiles unchanged because of the default argument.

- [ ] **Step 6: Commit.**

```bash
cd /c/Users/torso/Documents/deco/psprecomp
git add runtime/include/psp_ge_draw.h runtime/src/psp_ge_viewport.cpp runtime/tests/test_ge_viewport.cpp
git commit -m "feat(ge): viewport and scissor math take a render scale"
```

### Task 4: Render at the configured scale

**Files:**
- Modify: `runtime/src/psp_ge_draw.cpp`. The pixel sites today are lines ~154-166 (`capture_fbo_to_tga`), 214-226 (`PSP_FB_*`, `compute_state_viewport_depth`), 250 and 259 (`make_target`), 323-334 (`ge_draw_init`), 388-402 (shutdown auto-capture), 438 (`ge_draw_begin_list`), 590-606 (scissor), 733-737 (viewport), 927-940 (`present_service_screenshot`) and 972 (`present_blit`).

**Interfaces:**
- Consumes: `present_render_scale(const char*)` (Task 2), the scaled `ge_compute_viewport_depth(..., int scale)` and `ge_compute_scissor(...)` (Task 3).
- Produces: env `PSPRECOMP_RENDER_SCALE`, the log line `[DRAW] render scale N (WxH)`, and screenshots (`S`, `PSPRECOMP_SCREENSHOT`, shutdown auto-capture) at the FBO size `480N x 272N`.

- [ ] **Step 1: Add the scale state.** Move the `PSP_FB_WIDTH/PSP_FB_HEIGHT` constants (and their comment) from line ~214 up to just after the includes, before `write_tga` (the readbacks at line ~154 need them). Rewrite the comment to say the FBO is `PSP size x render scale`. Add below them:

```cpp
// Internal render scale (PSPRECOMP_RENDER_SCALE, 1..8; ge_draw_init). Every
// FBO-pixel quantity (target size, viewport, scissor, blit, readback) goes
// through it; through-mode vertices are already in NDC.
static int g_render_scale = 1;
static int fb_w() { return PSP_FB_WIDTH * g_render_scale; }
static int fb_h() { return PSP_FB_HEIGHT * g_render_scale; }
```

- [ ] **Step 2: One readback helper (replaces three copies).** Add after `write_tga`:

```cpp
/// Read a render target back as top-down RGBA rows and write it as TGA at
/// the FBO size. Render (GL) thread only.
static bool write_fbo_tga(GLuint fbo, const char* path) {
    const int w = fb_w(), h = fb_h();
    const size_t row = static_cast<size_t>(w) * 4;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    std::vector<uint8_t> pixels(row * h);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::vector<uint8_t> flipped(pixels.size());
    for (int y = 0; y < h; y++) {  // GL origin is bottom-left
        std::memcpy(flipped.data() + y * row, pixels.data() + (h - 1 - y) * row, row);
    }
    return write_tga(path, flipped.data(), w, h);
}
```
Then:
- `capture_fbo_to_tga` body becomes `return write_fbo_tga(front_fbo_id(), path);`
- in `ge_draw_shutdown`, replace the `glBindFramebuffer … write_tga(out_path, flipped.data(), 480, 272);` lines with `write_fbo_tga(front_fbo_id(), out_path);` (keep the `out_path` selection and the log line)
- in `present_service_screenshot`, replace the readback lines with `write_fbo_tga(fbo, g_screenshot_path);`

- [ ] **Step 3: Size the targets.** In `make_target`, use `fb_w(), fb_h()` in `glTexImage2D` and `glRenderbufferStorage` instead of `480, 272`. Update the function comment ("480x272" → "PSP size x render scale").

- [ ] **Step 4: Viewport, scissor, blit.**
- `compute_state_viewport_depth`: pass `PSP_FB_HEIGHT, g_render_scale` as the last two arguments.
- `ge_draw_begin_list` and the through-mode `else` branch in `ge_draw_prim`: use `glViewport(0, 0, fb_w(), fb_h());`
- Replace the scissor block in `ge_draw_prim` with:

```cpp
    {
        const GeScissor sc = ge_compute_scissor(state.scissor1, state.scissor2,
                                                PSP_FB_HEIGHT, g_render_scale);
        if (!sc.enabled) {
            glDisable(GL_SCISSOR_TEST);  // never set: whole buffer
        } else {
            glEnable(GL_SCISSOR_TEST);
            glScissor(sc.x, sc.y, sc.w, sc.h);
        }
    }
```
- `present_blit`: `glBlitFramebuffer(0, 0, fb_w(), fb_h(), r.x, r.y, r.x + r.w, r.y + r.h, GL_COLOR_BUFFER_BIT, present_filter());`. Fix its comment ("Letterbox the 480x272 image" → "Letterbox the rendered image").

- [ ] **Step 5: Read the env at init.** At the top of `ge_draw_init()` (before any target exists):

```cpp
    g_render_scale = present_render_scale(std::getenv("PSPRECOMP_RENDER_SCALE"));
    std::fprintf(stderr, "[DRAW] render scale %d (%dx%d)\n",
                 g_render_scale, fb_w(), fb_h());
```
Check that no `480`/`272` literal is left except the `PSP_FB_*` definitions and the `win_w/win_h` fallback in `present_blit`:
`grep -n "\b480\b\|\b272\b" runtime/src/psp_ge_draw.cpp`

- [ ] **Step 6: Build, test, scale-1 regression check.**

```bash
cd /c/Users/torso/Documents/deco/build/rtw && ninja && ctest 2>&1 | tail -2
taskkill //IM psprecomp_runtime.exe //F 2>/dev/null
PSPRECOMP_DISC0=C:/Users/torso/Documents/deco/disc0 ./psprecomp_runtime.exe > ../logs/m3b_s1.log 2>&1 &
until grep -q "DATAMS.BND" ../logs/m3b_s1.log; do sleep 1; done; sleep 8
python -I -c "import socket;s=socket.create_connection(('127.0.0.1',9999));s.sendall(b'S C:/Users/torso/Documents/deco/build/shots/m3b_s1.tga\n');print(s.recv(64))"
taskkill //IM psprecomp_runtime.exe //F
grep "render scale" ../logs/m3b_s1.log
cd ../../psprecomp && python -I tools/tgadiff.py ../build/shots/m3b_base_a.tga ../build/shots/m3b_s1.tga
```
Expected: all ctests pass; `[DRAW] render scale 1 (480x272)`; differing pixels ≤ the baseline noise N0 from Task 2 Step 3 (allow up to 2×N0 for animation phase). A larger diff means a rounding change: compare `m3b_base_a` and `m3b_s1` visually and fix before going on.

- [ ] **Step 7: Scale 2 and 4 smoke test.** Repeat the boot of Step 6 with `PSPRECOMP_RENDER_SCALE=2`, then with `=4`, taking `S` to `m3b_s2.tga` / `m3b_s4.tga`. Then convert and view:

```bash
cd /c/Users/torso/Documents/deco/build
python -I -c "import sys;sys.path.insert(0,'tools');import shoot;[shoot.tga2png(f'shots/m3b_{n}.tga',f'shots/m3b_{n}.png') for n in ('s1','s2','s4')]"
python -I ../psprecomp/tools/tgadiff.py shots/m3b_s1.tga shots/m3b_s2.tga   # expect: size differs: 480x272x4 vs 960x544x4
```
Expected: logs say `render scale 2 (960x544)` and `render scale 4 (1920x1088)`. Open the PNGs with the Read tool: the same language menu, sharper at higher scale, nothing cropped or offset. Also run once with `PSPRECOMP_RENDER_SCALE=abc` and check the log says `render scale 1`.

- [ ] **Step 8: Commit.**

```bash
cd /c/Users/torso/Documents/deco/psprecomp
git add runtime/src/psp_ge_draw.cpp
git commit -m "feat(ge): render targets at PSPRECOMP_RENDER_SCALE x the PSP resolution"
```

### Task 5: Visual check with the user at scale > 1 (and conditional fixes)

**Files:** `DECO/jugar.bat` (user launcher, outside the repo). Conditional: `runtime/src/psp_ge_vertex.cpp`, `runtime/include/psp_ge_vertex.h`, `runtime/tests/test_ge_vertex.cpp`, `runtime/src/psp_ge_draw.cpp`.

- [ ] **Step 1: Enable the scale in the launcher.** In `DECO/jugar.bat`, after the `PSPRECOMP_OSK_TEXT` line, add:

```bat
rem Resolucion interna: 1 = PSP original (480x272); 2..8 = mas nitido.
rem 4 = 1920x1088 (ideal para pantalla completa 1080p con F11).
set PSPRECOMP_RENDER_SCALE=4
```

- [ ] **Step 2: Ask the user to play** (in Spanish). They should go through the opening movie or title, the hideout (also open the SQUARE menu and the barracks), the world map, and one mission with the rhythm HUD, in a window and in fullscreen (F11). Ask them to look for thin lines or seams between pieces of 2D art, blurry or bleeding edges on fonts and icons, misaligned HUD, and anything that looks worse than at scale 1. At each spot they flag, take `S` (full-res screenshot) and `D 1` (one display-list dump into `jugar.log`).

- [ ] **Step 3: Points/lines census** from the `D` dumps:

```bash
grep -a "^\[DL\] prim" /c/Users/torso/Documents/deco/build/logs/jugar.log | sed -n 's/.* p=\([0-9]\) .*/\1/p' | sort | uniq -c
```
`p=0` (points), `p=1` (lines) and `p=2` (line strips) counted in visible draws mean Task 5c applies. Only p=3..6 means skip 5c.

- [ ] **Step 4: Record** the user's findings under "Task 5 result" and decide 5b/5c. If neither applies, go to Task 6.

#### Task 5b (only if seams/bleeding on 2D sprites): inset rectangle UVs by half a texel at scale > 1

- [ ] **Step 1: Failing test** in `runtime/tests/test_ge_vertex.cpp` (call it from `main()`):

```cpp
// Through-mode sprite UVs are normalized texel coords; at render scale > 1 a
// linear-filtered sprite samples half a texel outside its atlas cell unless
// the corners move half a texel inward.
static void test_inset_rect_uv() {
    DecodedVertex a{}, b{};
    a.uv[0] = 0.0f;          a.uv[1] = 0.0f;           // texel (0,0)
    b.uv[0] = 16.0f / 64.0f; b.uv[1] = 8.0f / 32.0f;   // texel (16,8), 64x32 texture
    ge_inset_rect_uv(a, b, 64.0f, 32.0f);
    CHECK_NEAR(a.uv[0], 0.5f / 64.0f);
    CHECK_NEAR(a.uv[1], 0.5f / 32.0f);
    CHECK_NEAR(b.uv[0], 15.5f / 64.0f);
    CHECK_NEAR(b.uv[1], 7.5f / 32.0f);
    // Mirrored sprite (b left of a): still inward.
    DecodedVertex c{}, d{};
    c.uv[0] = 16.0f / 64.0f; d.uv[0] = 0.0f;
    ge_inset_rect_uv(c, d, 64.0f, 32.0f);
    CHECK_NEAR(c.uv[0], 15.5f / 64.0f);
    CHECK_NEAR(d.uv[0], 0.5f / 64.0f);
}
```
Use the file's existing assertion macros. If there is no `CHECK_NEAR`, add `#define CHECK_NEAR(a, e) CHECK(std::fabs((a) - (e)) < 1e-6f, #a)` next to the file's `CHECK`.

- [ ] **Step 2: Run:** `ninja test_ge_vertex` → compile error (`ge_inset_rect_uv` undeclared).
- [ ] **Step 3: Implement.** Declare in `psp_ge_vertex.h` next to `ge_expand_rectangle`:

```cpp
/// Move both rectangle corners' (normalized) UVs half a texel toward each
/// other. Used at render scale > 1 for linear-filtered through-mode sprites
/// so they never sample outside their atlas cell.
void ge_inset_rect_uv(DecodedVertex& a, DecodedVertex& b, float texw, float texh);
```
Define it in `psp_ge_vertex.cpp` after `ge_expand_rectangle`:

```cpp
void ge_inset_rect_uv(DecodedVertex& a, DecodedVertex& b, float texw, float texh) {
    const float half[2] = {0.5f / texw, 0.5f / texh};
    for (int k = 0; k < 2; k++) {
        const float d = (b.uv[k] >= a.uv[k]) ? half[k] : -half[k];
        a.uv[k] += d;
        b.uv[k] -= d;
    }
}
```
In `psp_ge_draw.cpp`, in the RECTANGLES loop before `ge_expand_rectangle(...)`, copy the two corners and inset them when `g_render_scale > 1 && state.texture_enable && ge_vtype_through(state.vertex_type)` and the mag filter is linear (`(state.tex_filter >> 8) & 1`). Compute `texw = float(1u << (state.tex_size[0] & 0xFF))` and `texh = float(1u << ((state.tex_size[0] >> 8) & 0xFF))`, the same as `psp_ge_vertex.cpp:607-610`. Scale 1 is untouched by construction.
- [ ] **Step 4: Run** `ninja && ctest`: all pass. Ask the user to re-check the spot. Repeat Task 4 Step 6 (scale 1 must still match).
- [ ] **Step 5: Commit** `fix(ge): half-texel UV inset for filtered sprites at render scale > 1`.

#### Task 5c (only if points/lines are used): keep their apparent width

- [ ] In `ge_draw_prim`, before `glDrawArrays` for `GE_PRIM_LINES`/`GE_PRIM_LINE_STRIP`, call `glLineWidth(static_cast<float>(g_render_scale));`. For `GE_PRIM_POINTS`, add `uniform float u_point_size;` + `gl_PointSize = u_point_size;` to the vertex shader in `psp_ge_shader.cpp`, set it to `g_render_scale`, and `glEnable(GL_PROGRAM_POINT_SIZE)` once in `ge_draw_init`. Check `glGetError()` after the first wide line. Core profiles may reject a width > 1 with `GL_INVALID_VALUE`; if that happens, log it once and leave lines at 1 px. Verify with the user at the spot from Step 3, then commit `fix(ge): lines and points keep their width at render scale > 1`.

### Task 6: Docs, handoff, push

**Files:** `README.md` (around line 230), `docs/GRAPHICS.md` (lines ~24, ~29, ~373, ~427-428, ~452), `DEBUGGING.md` (the env flag list near `PSPRECOMP_FUNC_ARGS_MAX`), `DECO/build/autopilot/HANDOFF.md`.

- [ ] **Step 1: README.** After the `PSPRECOMP_WINDOW_SCALE` / `PSPRECOMP_PRESENT_FILTER` sentence, add: "`PSPRECOMP_RENDER_SCALE=N` (1–8, default 1) renders internally at N× 480x272 (4 = 1920x1088) for a sharper image in large windows and fullscreen."
- [ ] **Step 2: docs/GRAPHICS.md.** Every "480x272 FBO" becomes "480x272 × render scale FBO (`PSPRECOMP_RENDER_SCALE`, default 1)". At line ~428, fix the stale "`GL_NEAREST`": the present blit is `GL_LINEAR` unless `PSPRECOMP_PRESENT_FILTER=nearest`. Add one paragraph on what scales (FBO, viewport via `ge_compute_viewport_depth(..., scale)`, scissor via `ge_compute_scissor`, blit, readback) and what does not (through-mode NDC, textures, guest VRAM). If Task 5b/5c landed, describe them too.
- [ ] **Step 3: DEBUGGING.md.** Under the env flags add: "`PSPRECOMP_RENDER_SCALE=<1..8>`: internal resolution (default 1). Oracle comparisons with PPSSPP, `tools/gelist.py` diffs and `tools/tgadiff.py` baselines must run at 1. `S` screenshots are written at the FBO size."
- [ ] **Step 4: ARCHITECTURE.md:** only if it states the FBO size (`grep -n "480" ARCHITECTURE.md`; today it does not). README must stay true per CLAUDE.md §8.
- [ ] **Step 5: Gates and commit.**

```bash
cd /c/Users/torso/Documents/deco/build/rtw && ninja && ctest 2>&1 | tail -2
cd /c/Users/torso/Documents/deco/psprecomp && cargo test -q 2>&1 | grep -c "test result: ok"
git add README.md docs/GRAPHICS.md DEBUGGING.md
git commit -m "docs: internal render scale (PSPRECOMP_RENDER_SCALE)"
git push p3 patapon3-windows:main
git ls-remote p3 refs/heads/main   # must equal git rev-parse HEAD
```
- [ ] **Step 6: HANDOFF.md.** Mark M3b done in "Status / next steps" (what the user verified, the jugar.bat default), add a log line, and set the next item: the HD texture pack plan (below).

---

## After this plan

1. **HD textures (roadmap item 4):** write `docs/superpowers/plans/<date>-m3c-hd-textures.md` from the roadmap section of `2026-10-09-m3-graphics-rtt.md`. It is a PPSSPP-format loader (XXH64 hash, `textures.ini`), and the pack is read from a user folder (`PSPRECOMP_TEXTURES`) and never vendored. Pack: <https://github.com/Lin-zl522/Patapon-3-HD-Texture-Pack> (supports INFN00001). Start with the hash-match gate.
2. **Screenshot pairs:** ask the user for PPSSPP-vs-runtime pairs of the hideout, a mission with the HUD and the world map, now at scale 4 for the runtime, to order the remaining graphics bugs.
3. **Backlog** (no user-visible bug yet unless noted):
   - barracks preview light box (open, M3a)
   - the first SQUARE after reaching the hideout via `build/tools/to_square.py` sometimes does not register; a second press works. Check whether a human press shows it too before investigating.
   - Title bank in-code missing static targets; DxD cave `0x08A59094`
   - CPU-lock "waited 5 s for a FREE CPU"
   - `sceUtilityGetSystemParamInt` ID mapping (8=LANGUAGE, 9=BUTTON_PREF)
   - branch `wt-rtt` (RTT matcher, unmerged): merge only when a screen needs RTT

## Task 1 result

2026-10-10. `build/logs/reachable_cuts.txt`: 61 cuts (main 31, mission 11, title 13, azito 6); 58 are 0x8-0x28 bytes. Of the three >= 0x40 (all main): 0x08A6BFFC (0x4C) is real code (two DxD caves back to back); 0x08A60288 (0x6C) and 0x08A6F618 (0x6C) are DATA (a float table 0.4..1.9 / 0.01f plus code pointers, and a table of {-1, code ptr, 0}). Both were empty stubs before and now emit their bytes as code; harmless unless something calls them.
User played (scale 1, ~10:37-10:42): Continue -> hideout -> a mission to the end -> SQUARE menu open/close: "no se bloquea, todo bien". Log: no LOOKUP_MISS/FATAL during play; one `[CRASH] 0xC0000005` AFTER `[RT] Shutdown complete` (window closed, detached guest threads) - the same already appears in `jugar_watch.log` of 2026-10-09 (before 5f98221), so it is a pre-existing teardown race, not the cut.
Baseline noise N0 (language menu, two shots 1 s apart): 0 of 130560.

## Task 5 result

2026-10-10. Before asking the user, the language menu at scale 4 showed a 1-FBO-pixel black seam at the box's right corners. Cause: a GEOMETRY gap, not texture bleed: the body strip ends at x = 328 (s16 verts) and the corner sprite starts at x = 328.196 (float verts); 1x pixel-centre coverage hides it. 5b (UV inset) would not fix that, so a "5d" landed instead (48ad9cf): `ge_snap_through_positions` snaps through-mode fills to the PSP pixel grid (edge -> ceil(x - 0.5), the exact 1x coverage) at scale > 1 only; seam pixels 48 -> 0, scale 1 still 0 diff.
Census (GE summary of the user's scale-1 session; no `D` dump): points 9280, lines 722, line strips 9280 -> 5c done (d52eff7) as glLineWidth/glPointSize = scale in ge_draw_init (driver line range 1..10, accepted).
User at scale 4 (10:54-, hideout, SQUARE menu, barracks, world map, a mission, window + F11): "Todo bien"; the graphics bugs they see predate this change (engine/M3 fidelity). 5b not needed.
