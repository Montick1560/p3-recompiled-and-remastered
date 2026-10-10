# M3a — Graphics: missing backgrounds and render-to-texture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Task 1 is a diagnosis gate: do not start Task 2 until its decision is recorded.

**Goal:** The first graphics bugs the user reported (2026-10-09) render as in PPSSPP. These are the Title-overlay hero pages, whose sky/background gradient comes out black, and the Barracks (Cuartel) hero preview, which shows a light box behind the hero.

**Architecture:** The runtime draws each GE framebuffer address (FRAMEBUFPTR) into its own 480x272 GL FBO (`psp_ge_draw.cpp`, pool of 4). Textures are always decoded from guest RAM (`psp_ge_texture.cpp`). GL rendering is never written back to guest VRAM, and block transfers (`psp_ge_transfer.cpp`) are memory-only. So any effect that renders into a VRAM buffer and then samples that buffer as a texture, which PPSSPP handles in its FramebufferManager, samples stale or zero bytes. The leading hypothesis for both bugs is that missing render-to-texture (RTT). Task 1 confirms or refutes it with the existing `D` display-list dump. Tasks 2–5 add RTT: a pure address-matching helper plus a GL copy of the target region into a scratch texture.

**Tech Stack:** C++17, OpenGL 3.3 core (glBlitFramebuffer, texture swizzle), ctest unit tests, debug socket (TCP 9999), PPSSPP 1.20.4 at `DECO/PPSSPP` as the oracle.

**Spec:** `docs/superpowers/specs/2026-10-07-patapon3-native-port-design.md` (M3 = graphics fidelity).

## Evidence so far (user screenshots, 2026-10-09)

| Screen | PPSSPP (oracle) | Runtime |
|---|---|---|
| Title → hero pages, "Lady Meden" (`Desktop/h1.png` vs `Desktop/mal.png`) | dark-navy→teal sky gradient, stars, blue silhouettes, white/blue halo rings | **sky is black**; stars, silhouettes, rocks, text OK; halo rings greyer |
| Same pages, "Sukopon" (`Desktop/h2.png`, PPSSPP only) | orange cave background, blue night sky with moon, yellow rings | not captured yet (expect black background) |
| Barracks / Cuartel equipment screen (`Desktop/h3.png`, runtime only) | not captured yet | blurred brown background OK-looking; **light box behind the hero preview** |

## Global Constraints

- Generic runtime code only: no Patapon addresses in `runtime/src` / `runtime/include` (purity gate, DEBUGGING.md §2). Game-specific code goes in `games/patapon3/runtime/`.
- All GL calls stay on the main (render) thread. `ge_draw_prim` and `ge_texture_bind` already run there.
- Commits are authored `Patapon Community` (repo git config) with NO `Co-Authored-By` trailer. Push only with `git push p3 patapon3-windows:main`, never force.
- Gates after every runtime change: `cd DECO/build/rtw && ninja && ctest` (all pass), and `cargo test` in `psprecomp` (393 pass, unchanged).
- The user navigates the game; never write navigation/driver scripts (user feedback 2026-10-09). Ask the user to bring the game to a screen, then use socket commands only (`D`, `S`, `I`, `K`).
- Launch for diagnosis: `DECO/jugar.bat` writes stderr to `DECO/build/logs/jugar.log`. Pick "Español EU" at the language menu to see "Continuar".
- Scale-ready pixel code: write every FBO-pixel quantity this plan touches (target size, blit rects in Task 3) through one render-scale factor (`static int g_render_scale = 1;` in `psp_ge_draw.cpp`, multiply 480/272 and x/y/w/h by it), so the internal-resolution upscaling milestone (M3b, below) needs no rework of the RTT path. Diagnosis and oracle comparisons always run at scale 1.
- A/B switch: every new behaviour gets an env kill-switch (`PSPRECOMP_NO_RTT=1`) so a regression can be bisected in one run.

## Review Focus

1. **CPU-written VRAM after GL rendered it** (e.g. a movie or software-drawn image in a buffer GL once drew into). A reasonable person expects the CPU's image, not a stale FBO. Task 3 adds the guest-hash fallback, and Task 4 verifies the opening movie still plays.
2. **Texture window larger than or partly outside the 480x272 target** (a 512x512 texture over a 480x272 buffer). The texels outside must be transparent black, with no GL error. Task 3 clears the scratch texture before the blit, and Task 2's out-of-window cases pin the matcher.
3. **Texel-size mismatch or CLUT texture over a framebuffer** (depal). The texture must fall back to the RAM decode with one log line, and must never crash. Task 2 tests pin the mismatch and CLUT cases.
4. **VRAM aliases**: `0x44xxxxxx` (uncached) and the `+2 MB` mirror must match the same target. Task 2 tests pin this.
5. **Self-read** (drawing into target A while sampling A). Copying before the draw gives the previous content with no GL feedback loop. This holds by construction, because Task 3 always copies. Task 4 checks that the barracks blur still looks right.

---

### Task 1: Diagnosis gate: what draws the sky and does it sample a render target?

**Files:** none (read-only). Record the result at the end of this file under "Task 1 result".

- [ ] **Step 1: Ask the user** to launch `DECO/jugar.bat` and stop on the "Lady Meden" hero page (black sky). Then ask them to say "listo" and not touch anything for 5 s.
- [ ] **Step 2: Dump three display lists and grab a frame** (run from `DECO`):

```bash
python -c "import socket;s=socket.create_connection(('127.0.0.1',9999));s.sendall(b'D 3\n');print(s.recv(64))"
python -c "import socket;s=socket.create_connection(('127.0.0.1',9999));s.sendall(b'S C:/Users/torso/Documents/deco/build/shots/meden.tga\n');print(s.recv(64))"
```
Expected: `OK 0` twice. `build/logs/jugar.log` now contains `[DL] prim=` lines and three `[DL] ---- list end (fb=0x......)` lines.

- [ ] **Step 3: Summarise the dump**:

```bash
cd /c/Users/torso/Documents/deco
grep -c "^\[DL\] prim" build/logs/jugar.log
grep "^\[DL\] ---- list end" build/logs/jugar.log | sort | uniq -c
# render targets vs textures sampled from VRAM (0x04xxxxxx / 0x44xxxxxx):
grep "^\[DL\] prim" build/logs/jugar.log | sed -n 's/.* fb=\(0x[0-9A-F]*\) .* tex=\([01]\) ta=\(0x[0-9A-F]*\) tf=\([0-9]*\).*/fb=\1 tex=\2 ta=\3 tf=\4/p' | sort | uniq -c | sort -rn | head -40
grep -E "FBO target created|FBO pool overflow" build/logs/jugar.log | sort | uniq -c
```
- [ ] **Step 4: Find the sky draw.** Look for an early PRIM in the list for the displayed fb. It is a through-mode (`thr=1`) quad or strip covering 480x272, either with `tex=0` and per-vertex colours (`[DL]   v.. rgba=` lines differing top/bottom) or with `tex=1` and a VRAM `ta`. Note its line.
- [ ] **Step 5: Decide and record** (write the decision and the supporting log lines under "Task 1 result"):
  - **(A) RTT confirmed:** some `tex=1` draw has `ta` inside VRAM (`0x04......`/`0x44......`) at or just after an fb key that other lists render into. → Do Tasks 2–5.
  - **(B) Sky is a colour-only draw that comes out black:** its `[DL] v rgba` values are right (non-black) → GL state problem. Compare `blend/bm/fa/fb2/at/zt/zw/st` with what PPSSPP's GE debugger shows for that draw (ask the user to open *Depuración → GE debugger* on the same page and read the draw's state). If the rgba values are black → vertex colour decode problem (`psp_ge_vertex.cpp`); write a failing `test_ge_vertex.cpp` case with the vertex bytes from `R <va> 64`. Do not do Tasks 2–5. Write a new task here instead.
  - **(C) No sky draw at all** → guest side. Check `LOOKUP_MISS`/`first miss` lines in the log and the overlay's `recompile_report.json` missing targets (the Title bank has 16 real ones from heuristic callers, HANDOFF 10 Oct). Do not do Tasks 2–5.
- [ ] **Step 6: Repeat Steps 1–3 on the Barracks hero preview** (Cuartel equipment screen). Ask the user for the PPSSPP screenshot of the same screen. Record whether the preview box is an RTT draw (offscreen fb, then a textured quad whose `ta` is that fb) and what alpha it should have.

### Task 2: Pure render-target matcher (`psp_ge_rtt`)

Delegable: brief it to Grok as written. It is self-contained.

**Files:**
- Create: `runtime/include/psp_ge_rtt.h`, `runtime/src/psp_ge_rtt.cpp`
- Test: `runtime/tests/test_ge_rtt.cpp`
- Modify: `runtime/CMakeLists.txt` (add `src/psp_ge_rtt.cpp` to the runtime sources next to `src/psp_ge_transfer.cpp`; register the test after `ge_transfer_tests`)

**Interfaces:**
- Produces: `struct GeRttTarget { bool valid; uint32_t key; uint32_t stride; uint32_t fb_fmt; };`, `struct GeRttHit { int index; int x; int y; };`, `GeRttHit ge_rtt_find(const GeRttTarget* targets, int count, uint32_t tex_addr, int tex_fmt);`

- [ ] **Step 1: Write the failing test** `runtime/tests/test_ge_rtt.cpp`:

```cpp
// Unit tests for the render-target matcher (psp_ge_rtt.h): which GE render
// target, and which texel origin inside it, a texture address samples.

#include "psp_ge_rtt.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    const GeRttTarget t[3] = {
        {true, 0x000000, 512, 3},  // display buffer 0x04000000, 8888
        {true, 0x088000, 512, 3},  // second buffer 0x04088000, 8888
        {true, 0x110000, 256, 1},  // offscreen 0x04110000, 5551, stride 256
    };
    GeRttHit h = ge_rtt_find(t, 3, 0x04088000, 3);
    CHECK(h.index == 1 && h.x == 0 && h.y == 0, "texture at a buffer base samples that buffer");
    h = ge_rtt_find(t, 3, 0x04088000 + (10 * 512 + 20) * 4, 3);
    CHECK(h.index == 1 && h.x == 20 && h.y == 10, "texture inside a buffer gets its texel origin");
    CHECK(ge_rtt_find(t, 3, 0x44088000, 3).index == 1, "uncached VRAM alias matches");
    CHECK(ge_rtt_find(t, 3, 0x04288000, 3).index == 1, "VRAM mirror at +2 MB matches");
    h = ge_rtt_find(t, 3, 0x04110000 + (5 * 256 + 7) * 2, 1);
    CHECK(h.index == 2 && h.x == 7 && h.y == 5, "16-bit texture on a 16-bit buffer uses its stride");
    CHECK(ge_rtt_find(t, 3, 0x04110000 + 64, 0).index == 2, "5650 texture on a 5551 buffer matches (same texel size)");
    CHECK(ge_rtt_find(t, 3, 0x04110000, 3).index == -1, "8888 texture over a 16-bit buffer does not match");
    CHECK(ge_rtt_find(t, 3, 0x04088000, 5).index == -1, "CLUT8 texture never matches (depal not implemented)");
    CHECK(ge_rtt_find(t, 3, 0x04088000, 8).index == -1, "DXT1 texture never matches");
    CHECK(ge_rtt_find(t, 3, 0x08A00000, 3).index == -1, "main-RAM texture never matches");

    const GeRttTarget one[1] = {{true, 0x000000, 512, 3}};
    CHECK(ge_rtt_find(one, 1, 0x04000000 + (300 * 512) * 4, 3).index == -1,
          "texture below the 272-line window does not match");
    CHECK(ge_rtt_find(one, 1, 0x04000000 + 490 * 4, 3).index == -1,
          "texture right of the 480-pixel window does not match");
    h = ge_rtt_find(one, 1, 0x04000000 + (271 * 512 + 479) * 4, 3);
    CHECK(h.index == 0 && h.x == 479 && h.y == 271, "last visible texel matches");

    const GeRttTarget empty[1] = {{false, 0x088000, 512, 3}};
    CHECK(ge_rtt_find(empty, 1, 0x04088000, 3).index == -1, "slot without GL content never matches");
    const GeRttTarget nostride[1] = {{true, 0x088000, 0, 3}};
    CHECK(ge_rtt_find(nostride, 1, 0x04088000, 3).index == -1, "stride 0 never matches (no divide by zero)");

    if (failures == 0) std::printf("test_ge_rtt: all passed\n");
    return failures == 0 ? 0 : 1;
}
```

Register it in `runtime/CMakeLists.txt` right after `add_test(NAME ge_transfer_tests ...)`:

```cmake
# Render-target matcher (texture address -> render target + texel origin).
add_executable(test_ge_rtt tests/test_ge_rtt.cpp src/psp_ge_rtt.cpp)
target_include_directories(test_ge_rtt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_ge_rtt PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ge_rtt_tests COMMAND test_ge_rtt)
```

- [ ] **Step 2: Run it to verify it fails.** Run `cd DECO/build/rtw && cmake . && ninja test_ge_rtt`. Expected: a compile error, because `psp_ge_rtt.h` does not exist yet.
- [ ] **Step 3: Implement** `runtime/include/psp_ge_rtt.h`:

```cpp
#pragma once
#include <cstdint>

// A GE render target as the texture path sees it (one per FBO pool slot).
struct GeRttTarget {
    bool valid;       // the slot holds GL-rendered content newer than guest RAM
    uint32_t key;     // VRAM offset of the buffer: addr & 0x001FFFF0
    uint32_t stride;  // FRAMEBUFWIDTH & 0x7FC, in pixels
    uint32_t fb_fmt;  // FRAMEBUFPIXFORMAT: 0=5650 1=5551 2=4444 3=8888
};

struct GeRttHit {
    int index;  // matching target, -1 = the texture is not inside any target
    int x, y;   // texel origin of the texture inside the target
};

// Texture formats 0..3 (5650/5551/4444/8888) sample a render target when
// tex_addr lies inside the target's visible 480x272 window and the texel
// size matches the pixel size (16-bit on 16-bit, 8888 on 8888). CLUT and DXT
// formats never match (depal is not implemented). The nearest target at or
// below tex_addr wins. Addresses are VRAM in any alias (0x04/0x44, +2 MB).
GeRttHit ge_rtt_find(const GeRttTarget* targets, int count, uint32_t tex_addr, int tex_fmt);
```

and `runtime/src/psp_ge_rtt.cpp`:

```cpp
#include "psp_ge_rtt.h"

namespace {

constexpr uint32_t kFbWidth = 480, kFbHeight = 272;

bool in_vram(uint32_t addr) {
    return ((addr & 0x07FFFFFFu) & 0xFF800000u) == 0x04000000u;
}

uint32_t texel_bytes(int tex_fmt) {
    if (tex_fmt == 3) return 4;
    return (tex_fmt >= 0 && tex_fmt <= 2) ? 2 : 0;
}

}  // namespace

GeRttHit ge_rtt_find(const GeRttTarget* targets, int count, uint32_t tex_addr, int tex_fmt) {
    GeRttHit best{-1, 0, 0};
    const uint32_t tbytes = texel_bytes(tex_fmt);
    if (tbytes == 0 || !in_vram(tex_addr)) return best;
    const uint32_t off = tex_addr & 0x001FFFFFu;
    uint32_t best_delta = 0xFFFFFFFFu;
    for (int i = 0; i < count; ++i) {
        const GeRttTarget& t = targets[i];
        if (!t.valid || t.stride == 0) continue;
        const uint32_t pbytes = t.fb_fmt == 3 ? 4u : 2u;
        if (pbytes != tbytes || off < t.key) continue;
        const uint32_t delta = off - t.key;
        const uint32_t pixel = delta / pbytes;
        const uint32_t y = pixel / t.stride, x = pixel % t.stride;
        if (x >= kFbWidth || y >= kFbHeight || delta >= best_delta) continue;
        best = {i, static_cast<int>(x), static_cast<int>(y)};
        best_delta = delta;
    }
    return best;
}
```

- [ ] **Step 4: Run the tests.** Run `cd DECO/build/rtw && ninja && ctest`. Expected: `ge_rtt_tests` passes and every other test still passes.
- [ ] **Step 5: Commit.** Subject: `feat(ge): render-target matcher for texture addresses (psp_ge_rtt)`.

### Task 3: Sample render targets as textures (GL wiring)

Main model (subtle GL state). Not delegated.

**Files:**
- Modify: `runtime/src/psp_ge_draw.cpp` (GeTarget fields, per-PRIM bookkeeping, `bind_texture_from_target`, scratch FBO, shutdown cleanup)

**Interfaces:**
- Consumes: `ge_rtt_find` and `GeRttTarget` from Task 2.
- Produces: env `PSPRECOMP_NO_RTT=1` (disables the RTT path) and log lines `[RTT] tex 0x%08X fmt %d %dx%d -> target key=0x%06X slot %d at (%d,%d)` (first 20 hits, then every 600th).

- [ ] **Step 1: Bookkeeping on `GeTarget`.** Add `uint32_t stride = 0; uint32_t fb_fmt = 0; bool gl_written = false; bool hashed_this_list = false; uint32_t guest_hash = 0;`. In `ge_draw_begin_list`, set `hashed_this_list = false` on every slot. In `ge_draw_prim`, after a non-clear or clear PRIM is drawn into `g_current_target`, set `stride = state.framebuf_width & 0x7FC`, `fb_fmt = state.framebuf_format` and `gl_written = true`. On the first PRIM of the list into that slot, also set `hashed_this_list = true` and `guest_hash` = FNV-1a of the first 4096 guest bytes at `0x04000000 + key`. `rdram` is a `ge_draw_prim` parameter. Use a local copy of the FNV-1a loop from `psp_ge_texture.cpp:22`.
- [ ] **Step 2: Scratch copy.** Add `static GLuint g_rtt_fbo = 0, g_rtt_tex = 0; static int g_rtt_w = 0, g_rtt_h = 0;`, then add `static GLuint copy_target_region(int slot, int x, int y, int w, int h, bool opaque)`. It works as follows:
  - It creates or resizes `g_rtt_tex`: RGBA8 `w`x`h`, NEAREST, CLAMP_TO_EDGE. It sets `GL_TEXTURE_SWIZZLE_A` to `GL_ONE` when `opaque` is set (5650 has no alpha) and to `GL_ALPHA` otherwise.
  - It attaches the texture to `g_rtt_fbo` and clears it to (0,0,0,0).
  - It saves `glIsEnabled(GL_SCISSOR_TEST)` and disables scissor.
  - It blits `READ=g_targets[slot].fbo` rect `(x, 272 - y) → (x + w, 272 - y - h)` to `DRAW=g_rtt_fbo` rect `(0,0) → (w,h)` with `GL_COLOR_BUFFER_BIT, GL_NEAREST`. The reversed source Y puts PSP row `y` at texture row 0, which is how decoded PSP textures are laid out. First clamp the source rect to `[0,480]x[0,272]` and shift the destination rect by the same amount, so the texels outside stay transparent.
  - It restores `glBindFramebuffer(GL_FRAMEBUFFER, g_targets[g_current_target].fbo)` and the scissor enable.
  - It returns `g_rtt_tex`.
- [ ] **Step 3: Hook it.** Add `static bool bind_texture_from_target(uint8_t* rdram, const GeState& s)`:
  - Return false when `PSPRECOMP_NO_RTT=1`.
  - Build `GeRttTarget arr[GE_MAX_TARGETS]` from the pool, with `valid = fbo != 0 && gl_written`.
  - Compute the texture address as `ge_texture_bind` does: `tex_addr[0] | ((tex_bufw[0] >> 16) & 0xFF) << 24`. Get `w = 1 << (tex_size[0] & 0xFF)` and `h = 1 << ((tex_size[0] >> 8) & 0xFF)`, then call `ge_rtt_find`.
  - On a miss, return false.
  - On a hit, compare the current FNV-1a of the guest bytes at the target with `guest_hash`. If they differ, the CPU wrote that VRAM after GL did: set `gl_written = false`, log `[RTT] target key=... rewritten by CPU, using guest RAM` once per key, and return false.
  - Otherwise call `copy_target_region(..., /*opaque=*/s.tex_format == 0)`, bind the result on `GL_TEXTURE0`, log the hit, and return true.
  - In `ge_draw_prim`, replace `ge_texture_bind(rdram, g_frame_counter);` with `if (!bind_texture_from_target(rdram, state)) ge_texture_bind(rdram, g_frame_counter);`.
Reference code for Steps 1–3 (adapt names to the file as it is then; keep the behaviour). Add `#include "psp_ge_rtt.h"` and `#include <algorithm>`:

```cpp
// FNV-1a of the first 4 KB of a target's VRAM: lets the RTT path notice a
// CPU write (movie frame, software blit) that happened after GL drew there.
static uint32_t vram_hash(const uint8_t* rdram, uint32_t key) {
    uint32_t h = 2166136261u;
    const uint8_t* p = rdram + 0x04000000u + key;
    for (int i = 0; i < 4096; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static GLuint g_rtt_fbo = 0, g_rtt_tex = 0;
static int g_rtt_w = 0, g_rtt_h = 0;

// Copy the w x h texel window at (x, y) of a target into a scratch texture
// laid out like a decoded PSP texture (PSP row y -> texture row 0). Texels
// outside the 480x272 target stay transparent black.
static GLuint copy_target_region(int slot, int x, int y, int w, int h, bool opaque) {
    if (g_rtt_fbo == 0) glGenFramebuffers(1, &g_rtt_fbo);
    if (g_rtt_tex == 0) glGenTextures(1, &g_rtt_tex);
    glBindTexture(GL_TEXTURE_2D, g_rtt_tex);
    if (w != g_rtt_w || h != g_rtt_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_rtt_w = w;
        g_rtt_h = h;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, opaque ? GL_ONE : GL_ALPHA);

    // Blits ignore the colour mask but obey the scissor; glClear obeys both.
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean mask[4];
    GLfloat clear[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_rtt_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rtt_tex, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const int x1 = std::min(x + w, 480), y1 = std::min(y + h, 272);
    if (x1 > x && y1 > y) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_targets[slot].fbo);
        // Reversed source Y: GL row 271 - y (PSP row y) lands on texture row 0.
        glBlitFramebuffer(x, 272 - y, x1, 272 - y1, 0, 0, x1 - x, y1 - y,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_targets[g_current_target].fbo);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, g_rtt_tex);
    return g_rtt_tex;
}

// Texture inside a render target GL has drawn into: sample the FBO content
// (PPSSPP FramebufferManager semantics) instead of the stale guest bytes.
static bool bind_texture_from_target(uint8_t* rdram, const GeState& s) {
    static const bool disabled = [] {
        const char* e = std::getenv("PSPRECOMP_NO_RTT");
        return e && e[0] == '1';
    }();
    if (disabled || g_current_target < 0) return false;
    GeRttTarget arr[GE_MAX_TARGETS];
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        const GeTarget& t = g_targets[i];
        arr[i] = {t.fbo != 0 && t.gl_written, t.key, t.stride, t.fb_fmt};
    }
    const uint32_t addr = s.tex_addr[0] | (((s.tex_bufw[0] >> 16) & 0xFFu) << 24);
    const int w = 1 << (s.tex_size[0] & 0xFF), h = 1 << ((s.tex_size[0] >> 8) & 0xFF);
    const GeRttHit hit = ge_rtt_find(arr, GE_MAX_TARGETS, addr, static_cast<int>(s.tex_format));
    if (hit.index < 0) return false;
    GeTarget& t = g_targets[hit.index];
    if (vram_hash(rdram, t.key) != t.guest_hash) {
        t.gl_written = false;
        std::fprintf(stderr, "[RTT] target key=0x%06X rewritten by CPU, using guest RAM\n", t.key);
        return false;
    }
    copy_target_region(hit.index, hit.x, hit.y, w, h, s.tex_format == 0);
    static int hits = 0;
    if (++hits <= 20 || hits % 600 == 0) {
        std::fprintf(stderr, "[RTT] tex 0x%08X fmt %u %dx%d -> target key=0x%06X slot %d at (%d,%d)\n",
                     addr, s.tex_format, w, h, t.key, hit.index, hit.x, hit.y);
    }
    return true;
}
```

Per-PRIM bookkeeping (Step 1), placed in `ge_draw_prim` after the draw (or clear) into the current target:

```cpp
    GeTarget& cur = g_targets[g_current_target];
    cur.stride = state.framebuf_width & 0x7FC;
    cur.fb_fmt = state.framebuf_format;
    cur.gl_written = true;
    if (!cur.hashed_this_list) {
        cur.hashed_this_list = true;
        cur.guest_hash = vram_hash(rdram, cur.key);
    }
```

- [ ] **Step 4: Pool size.** If Task 1 showed `FBO pool overflow` lines, raise `GE_MAX_TARGETS` from 4 to 8. Evicting a slot destroys its RTT content.
- [ ] **Step 5: Shutdown.** In `ge_draw_shutdown`, delete `g_rtt_fbo`/`g_rtt_tex` when they are non-zero.
- [ ] **Step 6: Build and gates.** Run `cd DECO/build/rtw && ninja && ctest`. Expected: all tests pass. Then run a boot smoke run of 60 s to the title (`DECO/jugar.bat`, or the HANDOFF run line). Expected: title visible, no new `LOOKUP_MISS`, and `[RTT]` lines in the log if the title uses RTT.
- [ ] **Step 7: Commit.** Subject: `feat(ge): sample render targets as textures (render-to-texture)`.

### Task 4: Verify against the oracle (user-driven)

**Files:** none.

- [ ] **Step 1:** Ask the user to open the Lady Meden page and then the Sukopon page, without `PSPRECOMP_NO_RTT`. Capture each with `S C:/Users/torso/Documents/deco/build/shots/<name>.tga`. Compare with `Desktop/h1.png` and `h2.png`: the sky gradient and the orange cave must be present. Then repeat once with `PSPRECOMP_NO_RTT=1` and confirm the old black sky, which proves the switch is the cause.
- [ ] **Step 2:** Barracks preview: the user's PPSSPP screenshot against `S` capture. If the light box remains, record the `D` dump and go to the conditional Task 6.
- [ ] **Step 3: Regression walk.** Ask the user to replay one mission and visit the hideout, the obelisk and the barracks. Watch for new black or garbage areas, and grep the log for `[RTT] ... rewritten by CPU` and `FBO pool overflow`. Confirm the opening movie still plays (Review Focus 1).

### Task 5: Documentation

Delegable to DeepSeek flash after Task 4 passes.

**Files:**
- Modify: `docs/GRAPHICS.md` (the "Render targets and render-to-texture" subsection: matcher rules, copy-on-sample, CPU-rewrite fallback, the 4 KB hash limit, no depal, no write-back; remove RTT from "What Is Not Implemented"), `docs/ENV_FLAGS.md` (`PSPRECOMP_NO_RTT`), `README.md` (the Limitations line about render-to-texture).
- [ ] **Step 1:** Write the three edits and verify with `grep -n "PSPRECOMP_NO_RTT" docs/ENV_FLAGS.md docs/GRAPHICS.md`.
- [ ] **Step 2: Commit.** Subject: `docs(graphics): render-to-texture`.

### Task 6 (conditional, only if Task 4 Step 2 still shows the box): framebuffer alpha = stencil

On the PSP, the framebuffer alpha channel is the stencil buffer. Blending never writes alpha; clears and stencil ops do. An RTT composite that uses texture alpha needs our FBO alpha to follow the same rules. Diagnose first with the `D` dump of the preview list: look at the clear's alpha, `st`, the stencil op, and the alpha mask (`psp_ge_mask.h`). Then write the task with a failing unit test in `psp_ge_blend.h`/`psp_ge_mask.h` style before touching GL.

### Task 7 (conditional): GL → guest VRAM download

Write this task only if a block transfer or CPU read of a GL-written target is observed. Candidates are a save-icon screenshot, or a `[GE] TRANSFER` whose source overlaps a target with `gl_written`. Mirror PPSSPP's `FramebufferManager::ReadFramebufferToMemory` (glReadPixels of the rect, converted to the target's pixel format).

---

## Task 1 result

(To be filled in by the executing session: decision A/B/C, the sky draw's `[DL]` line, and the fb/ta summary.)

## Next screenshots to request from the user

After this plan, ask the user for three more side-by-side pairs (PPSSPP vs runtime): the hideout (full view), a mission in progress (with the drum/rhythm HUD), and the world map. Use them to order the next graphics fixes.

## Roadmap after M3a (evaluated 2026-10-09 with the user)

1. **M3a+ (small, right after RTT):** honour `TEXFILTER` (bilinear) and `TEXWRAP` (repeat/clamp). Both are stored but ignored today; the sampler is fixed at NEAREST + CLAMP. Also make the window resizable, add fullscreen, and offer a linear present filter. The user compares against PPSSPP, which renders at window resolution with bilinear filtering, so part of the "looks worse" impression comes from these.
2. **M3b internal resolution (2x–4x, configurable, default from config):** scale the FBOs, viewport (`ge_compute_viewport_depth`), scissor, RTT blit rects, clears and screenshot readback. Watch for atlas seams on 2D sprites (PPSSPP has texture-coordinate snapping for this) and keep scale 1 for oracle diffs.
3. **Remaining graphics bugs** from the user's next screenshot pairs, all written scale-aware.
4. **Late, optional, HD textures:**
   - Mechanism: dump decoded textures plus a replacement lookup keyed by the texture-cache hash. Prefer PPSSPP's `textures.ini` + hash format so a pack works in both.
   - Content: no Patapon 3 pack is known to exist, so it would mean AI-upscaling dumped textures. That is a large content job and needs care with alpha edges, CLUT variants and the DxD mod's own textures.
   - Textures sampled from render targets cannot be replaced.
