# Render to texture (RTT) — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the user's usual
> "nativo" mode) or superpowers:subagent-driven-development to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking. **Task 3 is a gate:** Tasks 4-6 run only on the
> branch its evidence picks.

Written 2026-10-16 (Opus 5.5). Nothing implemented yet.

**Goal:** when Patapon 3 draws into a VRAM buffer and then uses that buffer as a texture, copies it
with a block transfer, or the CPU touches it, the runtime shows what PPSSPP shows instead of the
stale or empty guest bytes.

**Architecture:** the runtime renders every FRAMEBUFPTR into its own GL FBO (pool of 4 in
`runtime/src/psp_ge_draw.cpp`) and never writes GL pixels back to guest VRAM. Textures always decode
from guest RAM (`psp_ge_texture.cpp`), and block transfers are memory-only (`psp_ge_transfer.cpp`).
So a texture that is a render target samples bytes the GE never wrote. PPSSPP handles this in
`TextureCacheCommon::MatchFramebuffer` (`DECO/tools/ppsspp-src/GPU/Common/TextureCacheCommon.cpp:1214`)
and `FramebufferManagerCommon` (`BindFramebufferAsColorTexture` :1262, `CopyFramebufferForColorTexture`
:1336, `NotifyBlockTransferBefore` :2622, `ReadFramebufferToMemory` :3299).

**No evidence yet that Patapon 3 needs RTT.** Every log under `DECO/build/logs` shows only the two
display buffers (`FBO target created key=0x000000` / `key=0x044000`, about 460 times each). No
offscreen FRAMEBUFPTR, no `[GE] block transfer` line, and no logged texture in the framebuffer
region (the VRAM textures are CLUT4 at `0x040F4F40`.. and above). The graphics tour matches PPSSPP
almost pixel for pixel on every menu. The 2026-10-09 "black sky" and "light box" reports that first
pointed at RTT turned out to be a guest-side `$ra` bug (M3a plan, "Task 1 result"). PPSSPP's
`compat.ini` has no framebuffer flag for Patapon 3. So this plan **detects first** (Tasks 1-2) and
covers the game with the user's main save (Task 3), then implements only what the evidence needs.

- **Branch A** (most likely if anything): a texture samples a render target. Task 4: copy the
  target window into a scratch texture before the draw.
- **Branch B**: a block transfer or the CPU reads a GL-written target. Task 5: GL→guest readback.
- **Branch C**: the CPU writes a buffer that GL then presents or samples. Task 6: guest→GL upload.
- **Branch D**: nothing in the whole session. Stop and ask the user (see Task 3 Step 6).

**Tech Stack:** C++17 runtime, OpenGL 3.3 core (`glBlitFramebuffer`, `GL_TEXTURE_SWIZZLE_A`),
CTest unit tests, Python tools in `DECO/build/tools`, PPSSPP 1.20.4 as the oracle.

**Spec:** the user, 2026-10-16: *"haz el plan para el render to texture, ese lo haremos ahora,
intentaré meter mi save principal"*. This comes after Claude's maturity review, which named RTT
as the largest known graphics gap. Background spec: `docs/superpowers/specs/2026-10-07-patapon3-native-port-design.md`
(M3 = graphics fidelity). Earlier design: `docs/superpowers/plans/2026-10-09-m3-graphics-rtt.md`
Tasks 2-3 (matcher on branch `wt-rtt`, commit `9520081`, unmerged). This plan supersedes those two tasks.

`DECO` = `C:/Users/torso/Documents/deco`. Repo = `DECO/psprecomp`, branch `patapon3-windows`.

## State when this plan was written

- `patapon3-windows` = `01824e9` (local; carries the icon commit `f639180`), `p3/main` = `b924d29`.
  `ctest --test-dir DECO/build/rtw`: 70/70.
- `wt-rtt` (`9520081`) adds `psp_ge_rtt.{h,cpp}` and `test_ge_rtt.cpp`. Its matcher ignores the
  texture stride and PPSSPP's sub-area rules. Task 1 re-implements it here; do not cherry-pick.
- `GeState` has no ZBUFPTR/ZBUFWIDTH fields (`psp_ge.cpp:790` ignores both commands).
- GL state: scissor and colour mask are raw GL calls set per PRIM (`psp_ge_draw.cpp:679-690` scissor,
  `:1030-1036` mask). The texture bind happens between them (`:1004`). The clear path sets its own
  `glClearColor` (`:773`). Texture/sampler binds go through the shadow (`gl_state_bind_texture`,
  `gl_state_apply_sampler`; a new texture binding forgets the sampler, `psp_gl_shadow.h:112`).
- The user's game: `DECO/jugar.bat` (30 fps) / `jugar_60fps.bat`, port 9999, stderr to
  `DECO/build/logs/jugar.log`. The user is bringing their **main save** for Task 3.

## Global Constraints

- Generic runtime code only. No Patapon addresses in `runtime/src` / `runtime/include`; RTT is
  PPSSPP's generic GE behaviour.
- All GL calls on the render thread (`ge_draw_prim`, `ge_present_frame` and the GE command loop in
  `psp_ge.cpp` already run there).
- Never edit `build/p3_output/generated/*.cpp`.
- Test instances only on **port 9998** (`TOUR_RT_PORT=9998`), always `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`
  for tours and `rt_regress`. Never set `PSPRECOMP_NO_AUDIO=1`. Never touch the user's game (9999)
  or `DECO/savedata`. Tours use copies (`build/save_arena`, `build/save_main`). Kill only your own
  instances, by PID. Before a two-sided tour, check `tasklist | grep -i ppsspp`: `gfx_tour.py` kills
  every PPSSPP.
- Before every build: `netstat -ano | grep ":9999 .*LISTEN"`. If the user is playing, ask first
  (the exe is locked and the user's session would end).
- Kill switch for every behaviour change: `PSPRECOMP_NO_RTT=1` turns off Tasks 4-6 (the detector
  in Task 2 only logs and stays on).
- Scale-ready: every FBO pixel quantity goes through `g_render_scale` (`psp_ge_draw.cpp:120`).
  Oracle comparisons run at scale 1 (tours do); also check once at the user's `jugar.bat` scale.
- Line endings: `runtime/` sources, `DEBUGGING.md` and the docs are CRLF in the working tree
  (`core.autocrlf=true`). Use the Edit tool; `sed -i` from Git Bash rewrites them as LF.
- Commits: author "Patapon Community" (repo identity; do not touch git config), **no
  `Co-Authored-By`**, English, one commit per task, ending with `Verified: ...` and `Agent: ...`.
  Talk to the user in Spanish.
- **No push** until the user says so; when they do, use the worktree cherry-pick recipe at the end
  of `2026-10-16-arena-healthbar-flicker.md`. `push-p3`, `push-p3-2` and `push-p3-3` are used, so
  the next one is **push-p3-4**. Never push `f639180`.
- Stuck 45 min or 3 attempts on one step: revert that step, write what you saw under "Findings" at
  the end of this plan, and ask the user.
- Gates per runtime change: `cmake --build DECO/build/rtw` (no new warnings in `runtime/`),
  `ctest --test-dir DECO/build/rtw` all pass, `rt_regress` PASS against a baseline taken in the same
  session, and `build/tools/perf_hideout.py` within 5 % of its pre-change run (the detector runs
  on every textured PRIM).

## Review Focus

1. **The CPU rewrote a buffer after GL drew it** (a movie frame, a software-drawn image, a memset
   clear). A reasonable person expects the CPU's image, not a stale FBO. The detector marks the
   target `cpu_newer` (Task 2). The matcher only sees targets with `gl_written && !cpu_newer`
   (Task 4 Step 3), and the opening movie is part of Task 4's check.
2. **Texture window larger than, or partly outside, the 480x272 target** (512x512 over a
   480x272 buffer). Texels outside must be transparent black, with no GL error. Pinned by
   `ge_rtt_blit_rect` tests (Task 1) and the scratch clear before the blit (Task 4).
3. **Format or stride mismatch, CLUT or DXT over a framebuffer.** These fall back to the RAM decode,
   log one `[RTT] sample ... match=-1` line, and never crash. Pinned by the matcher tests (Task 1).
   CLUT-over-framebuffer (PPSSPP "depal") is out of scope until the detector shows it.
4. **VRAM aliases:** `0x44xxxxxx` and the `+2 MB` mirror must match the same target. Pinned by
   matcher and overlap tests (Task 1).
5. **A draw that samples its own target** (feedback). The window is copied before the draw, so
   there is no GL feedback loop and the draw sees the previous content. Holds by construction
   (Task 4 always copies); the detector's ` self` flag says whether the game does it.

---

### Task 1: Pure RTT helpers — matcher, overlap, texture span, blit rect (TDD)

**Files:**
- Create: `runtime/include/psp_ge_rtt.h`, `runtime/src/psp_ge_rtt.cpp`, `runtime/tests/test_ge_rtt.cpp`
- Modify: `runtime/CMakeLists.txt` (source list next to `src/psp_ge_transfer.cpp`, ~line 143;
  test block after the `ge_transfer_tests` block, ~line 611)

**Interfaces:**
- Produces (all pure, no GL, used by Tasks 2 and 4):
  ```cpp
  struct GeRttTarget { bool valid; uint32_t key; uint32_t stride; uint32_t fb_fmt; };
  struct GeRttHit { int index; int x, y; };
  GeRttHit ge_rtt_find(const GeRttTarget* targets, int count, uint32_t tex_addr, int tex_fmt,
                       uint32_t tex_bufw, int tex_h);
  int ge_rtt_overlap(const GeRttTarget* targets, int count, uint32_t addr, uint32_t bytes);
  uint32_t ge_rtt_tex_bytes(int tex_fmt, uint32_t tex_bufw, int tex_h);
  struct GeRttBlit { bool empty; int src_x0, src_y0, src_x1, src_y1, dst_w, dst_h; };
  GeRttBlit ge_rtt_blit_rect(int x, int y, int w, int h, int scale);
  ```

- [ ] **Step 1: Write the failing test** `runtime/tests/test_ge_rtt.cpp`:
  ```cpp
  // Unit tests for the render-to-texture helpers (psp_ge_rtt.h): which GE render target a
  // texture samples (PPSSPP TextureCacheCommon.cpp MatchFramebuffer, colour channel, no CLUT),
  // which target a byte span overlaps, and the GL blit rect of a texel window.

  #include "psp_ge_rtt.h"

  #include <cstdio>

  static int failures = 0;
  #define CHECK(cond, msg) \
      do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

  static void find_rules() {
      const GeRttTarget t[3] = {
          {true, 0x000000, 512, 3},  // 0x04000000, 8888
          {true, 0x088000, 512, 3},  // 0x04088000, 8888
          {true, 0x110000, 256, 1},  // 0x04110000, 5551, stride 256
      };
      GeRttHit h = ge_rtt_find(t, 3, 0x04088000, 3, 512, 256);
      CHECK(h.index == 1 && h.x == 0 && h.y == 0, "texture at a buffer base samples that buffer");
      h = ge_rtt_find(t, 3, 0x04088000 + (10 * 512 + 20) * 4, 3, 512, 64);
      CHECK(h.index == 1 && h.x == 20 && h.y == 10, "texture inside a buffer gets its texel origin");
      CHECK(ge_rtt_find(t, 3, 0x44088000, 3, 512, 256).index == 1, "uncached VRAM alias matches");
      CHECK(ge_rtt_find(t, 3, 0x04288000, 3, 512, 256).index == 1, "VRAM mirror at +2 MB matches");
      h = ge_rtt_find(t, 3, 0x04110000 + (5 * 256 + 7) * 2, 1, 256, 64);
      CHECK(h.index == 2 && h.x == 7 && h.y == 5, "16-bit texture on a 16-bit buffer uses its stride");
      CHECK(ge_rtt_find(t, 3, 0x04110000 + 64, 0, 256, 64).index == 2,
            "5650 texture on a 5551 buffer matches (same texel size)");
      CHECK(ge_rtt_find(t, 3, 0x04110000, 3, 256, 64).index == -1, "8888 texture over a 16-bit buffer: no");
      CHECK(ge_rtt_find(t, 3, 0x04088000, 5, 512, 256).index == -1, "CLUT8 never matches (no depal)");
      CHECK(ge_rtt_find(t, 3, 0x04088000, 8, 512, 256).index == -1, "DXT1 never matches");
      CHECK(ge_rtt_find(t, 3, 0x08A00000, 3, 512, 256).index == -1, "main-RAM texture never matches");
      CHECK(ge_rtt_find(t, 3, 0x04088000, 3, 256, 256).index == -1,
            "stride mismatch: texture bufw 256 on a 512 buffer does not match (PPSSPP)");
      CHECK(ge_rtt_find(t, 3, 0x04088000 + 40 * 4, 3, 64, 1).index == 1,
            "one-row texture ignores the stride (PPSSPP texHeight > 1 rule)");
      CHECK(ge_rtt_find(t, 3, 0x04088000 + (200 * 512) * 4, 3, 512, 512).index == -1,
            "y offset + texHeight/4 past the buffer height does not match");
      CHECK(ge_rtt_find(t, 3, 0x04088000, 3, 512, 512).index == 1,
            "512-high texture at the base of a 272-line buffer matches (lenient, PPSSPP)");
      CHECK(ge_rtt_find(t, 3, 0x04000000 + 490 * 4, 3, 512, 1).index == -1,
            "texture right of the 480-pixel window does not match");
      h = ge_rtt_find(t, 3, 0x04000000 + (271 * 512 + 479) * 4, 3, 512, 1);
      CHECK(h.index == 0 && h.x == 479 && h.y == 271, "last visible texel matches");
      const GeRttTarget high[1] = {{true, 0x120000, 512, 3}};
      CHECK(ge_rtt_find(high, 1, 0x04120000 + (40 * 512) * 4, 3, 512, 16).index == -1,
            "y offset > 32 into a buffer above 0x04110000 is ignored (PPSSPP safety rule)");
      const GeRttTarget empty[1] = {{false, 0x088000, 512, 3}};
      CHECK(ge_rtt_find(empty, 1, 0x04088000, 3, 512, 256).index == -1, "slot without GL content: no");
      const GeRttTarget nostride[1] = {{true, 0x088000, 0, 3}};
      CHECK(ge_rtt_find(nostride, 1, 0x04088000, 3, 512, 256).index == -1, "stride 0: no (no divide)");
  }

  static void overlap_rules() {
      const GeRttTarget t[2] = {
          {true, 0x000000, 512, 1},  // 16-bit: 512*2*272 = 0x44000 bytes
          {true, 0x044000, 512, 1},
      };
      CHECK(ge_rtt_overlap(t, 2, 0x04000000, 64) == 0, "span at a target base overlaps it");
      CHECK(ge_rtt_overlap(t, 2, 0x04043FF0, 16) == 0, "last bytes of target 0 overlap it");
      CHECK(ge_rtt_overlap(t, 2, 0x04044000, 16) == 1, "first byte of target 1 overlaps target 1");
      CHECK(ge_rtt_overlap(t, 2, 0x04088000, 0x1000) == -1, "span past both targets: none");
      CHECK(ge_rtt_overlap(t, 2, 0x44044000, 16) == 1, "uncached alias overlaps");
      CHECK(ge_rtt_overlap(t, 2, 0x04244000, 16) == 1, "+2 MB mirror overlaps");
      CHECK(ge_rtt_overlap(t, 2, 0x08800000, 0x100000) == -1, "main RAM never overlaps");
      CHECK(ge_rtt_overlap(t, 2, 0x04000000, 0) == -1, "empty span never overlaps");
      const GeRttTarget off[1] = {{false, 0x000000, 512, 1}};
      CHECK(ge_rtt_overlap(off, 1, 0x04000000, 64) == -1, "invalid target never overlaps");
  }

  static void tex_bytes_rules() {
      CHECK(ge_rtt_tex_bytes(1, 512, 272) == 512u * 272 * 2, "5551: 2 bytes per texel");
      CHECK(ge_rtt_tex_bytes(3, 512, 272) == 512u * 272 * 4, "8888: 4 bytes per texel");
      CHECK(ge_rtt_tex_bytes(4, 128, 64) == 128u * 64 / 2, "CLUT4: half a byte per texel");
      CHECK(ge_rtt_tex_bytes(5, 128, 64) == 128u * 64, "CLUT8: one byte per texel");
      CHECK(ge_rtt_tex_bytes(8, 64, 64) == 64u * 64 / 2, "DXT1: 4 bits per texel");
      CHECK(ge_rtt_tex_bytes(11, 64, 64) == 0, "unknown format: no span");
      CHECK(ge_rtt_tex_bytes(3, 0, 1) == 4, "bufw 0 counts as one texel");
  }

  static void blit_rules() {
      GeRttBlit b = ge_rtt_blit_rect(0, 0, 64, 64, 1);
      CHECK(!b.empty && b.src_x0 == 0 && b.src_x1 == 64 && b.src_y0 == 272 && b.src_y1 == 208 &&
                b.dst_w == 64 && b.dst_h == 64,
            "top-left 64x64: GL rows 272..208 flipped so texture row 0 is PSP row 0");
      b = ge_rtt_blit_rect(460, 250, 64, 64, 1);
      CHECK(!b.empty && b.src_x0 == 460 && b.src_x1 == 480 && b.src_y0 == 22 && b.src_y1 == 0 &&
                b.dst_w == 20 && b.dst_h == 22,
            "window clipped to the 480x272 target (outside texels stay cleared)");
      b = ge_rtt_blit_rect(10, 20, 32, 16, 4);
      CHECK(!b.empty && b.src_x0 == 40 && b.src_x1 == 168 && b.src_y0 == 1008 && b.src_y1 == 944 &&
                b.dst_w == 128 && b.dst_h == 64,
            "render scale 4 multiplies every rect value");
      CHECK(ge_rtt_blit_rect(480, 0, 16, 16, 1).empty, "window right of the target: empty");
      CHECK(ge_rtt_blit_rect(0, 272, 16, 16, 1).empty, "window below the target: empty");
      CHECK(ge_rtt_blit_rect(0, 0, 16, 16, 0).empty, "scale 0: empty");
  }

  int main() {
      find_rules();
      overlap_rules();
      tex_bytes_rules();
      blit_rules();
      if (failures == 0) std::printf("test_ge_rtt: all passed\n");
      return failures == 0 ? 0 : 1;
  }
  ```
  Wire it in `runtime/CMakeLists.txt` after the `ge_transfer_tests` block:
  ```cmake
  # Render-to-texture helpers (matcher, overlap, texture span, blit rect). Pure, no GL.
  add_executable(test_ge_rtt tests/test_ge_rtt.cpp src/psp_ge_rtt.cpp)
  target_include_directories(test_ge_rtt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(test_ge_rtt PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
  add_test(NAME ge_rtt_tests COMMAND test_ge_rtt)
  ```
  Add `src/psp_ge_rtt.cpp` to `RUNTIME_SOURCES` right after `src/psp_ge_transfer.cpp`. Create the
  header with the interface above, and `psp_ge_rtt.cpp` with stubs so the test fails on behaviour,
  not on linking:
  ```cpp
  #include "psp_ge_rtt.h"
  GeRttHit ge_rtt_find(const GeRttTarget*, int, uint32_t, int, uint32_t, int) { return {-1, 0, 0}; }
  int ge_rtt_overlap(const GeRttTarget*, int, uint32_t, uint32_t) { return -1; }
  uint32_t ge_rtt_tex_bytes(int, uint32_t, int) { return 0; }
  GeRttBlit ge_rtt_blit_rect(int, int, int, int, int) { return {true, 0, 0, 0, 0, 0, 0}; }
  ```
  The header (`runtime/include/psp_ge_rtt.h`):
  ```cpp
  #pragma once
  #include <cstdint>

  // Render-to-texture helpers, pure (no GL). PPSSPP references:
  // GPU/Common/TextureCacheCommon.cpp MatchFramebuffer (colour channel, non-CLUT formats).

  // A GE render target as the texture path sees it (one per FBO pool slot).
  struct GeRttTarget {
      bool valid;       // GL holds content newer than guest RAM (gl_written && !cpu_newer)
      uint32_t key;     // VRAM offset of the buffer: addr & 0x001FFFF0
      uint32_t stride;  // FRAMEBUFWIDTH & 0x7FC, in pixels
      uint32_t fb_fmt;  // FRAMEBUFPIXFORMAT: 0=5650 1=5551 2=4444 3=8888
  };

  struct GeRttHit {
      int index;  // matching target, -1 = sample guest RAM as usual
      int x, y;   // texel origin of the texture inside the target
  };

  // Texture formats 0..3 sample a render target when the texel size equals the pixel size, the
  // strides agree (unless the texture is one row high) and the origin passes PPSSPP's sub-area
  // rules: x inside the 480-pixel window, y < 512, y + texHeight/4 inside 272 lines when y > 0,
  // y <= 32 for buffers above 0x04110000. CLUT and DXT never match (depal is not implemented).
  // The nearest target at or below tex_addr wins. Any VRAM alias (0x04/0x44, +2 MB) works.
  GeRttHit ge_rtt_find(const GeRttTarget* targets, int count, uint32_t tex_addr, int tex_fmt,
                       uint32_t tex_bufw, int tex_h);

  // First valid target whose 272-line VRAM window [key, key + stride*bpp*272) overlaps the byte
  // span [addr, addr + bytes); -1 if none or addr is not VRAM. Any format (detector use).
  int ge_rtt_overlap(const GeRttTarget* targets, int count, uint32_t addr, uint32_t bytes);

  // Bytes a texture spans in memory: bufw (at least 1) x h texels at the format's bit depth
  // (16/16/16/32, CLUT 4/8/16/32, DXT1 4, DXT3/5 8). 0 for an unknown format or h <= 0.
  uint32_t ge_rtt_tex_bytes(int tex_fmt, uint32_t tex_bufw, int tex_h);

  // GL blit of the w x h texel window at PSP (x, y) of a 480x272 target rendered at `scale`
  // (bottom-left GL origin) into a scratch texture whose row 0 is PSP row y. The window is
  // clipped to the target; dst starts at (0, 0). empty = nothing of the window is inside.
  struct GeRttBlit { bool empty; int src_x0, src_y0, src_x1, src_y1, dst_w, dst_h; };
  GeRttBlit ge_rtt_blit_rect(int x, int y, int w, int h, int scale);
  ```
- [ ] **Step 2: Run it.** `cmake --build DECO/build/rtw --target test_ge_rtt && DECO/build/rtw/test_ge_rtt.exe`.
  Expected: many `FAIL:` lines (every positive case), exit 1. The negative cases pass against
  the stubs; that is expected.
- [ ] **Step 3: Implement** `runtime/src/psp_ge_rtt.cpp`:
  ```cpp
  #include "psp_ge_rtt.h"

  #include <algorithm>

  namespace {

  constexpr uint32_t kFbWidth = 480, kFbHeight = 272;

  bool in_vram(uint32_t addr) {
      return ((addr & 0x07FFFFFFu) & 0xFF800000u) == 0x04000000u;
  }

  uint32_t texel_bytes(int tex_fmt) {
      if (tex_fmt == 3) return 4;
      return (tex_fmt >= 0 && tex_fmt <= 2) ? 2 : 0;
  }

  uint32_t pixel_bytes(uint32_t fb_fmt) { return fb_fmt == 3 ? 4u : 2u; }

  }  // namespace

  GeRttHit ge_rtt_find(const GeRttTarget* targets, int count, uint32_t tex_addr, int tex_fmt,
                       uint32_t tex_bufw, int tex_h) {
      GeRttHit best{-1, 0, 0};
      const uint32_t tbytes = texel_bytes(tex_fmt);
      if (tbytes == 0 || !in_vram(tex_addr)) return best;
      const uint32_t off = tex_addr & 0x001FFFFFu;
      uint32_t best_delta = 0xFFFFFFFFu;
      for (int i = 0; i < count; ++i) {
          const GeRttTarget& t = targets[i];
          if (!t.valid || t.stride == 0) continue;
          const uint32_t pbytes = pixel_bytes(t.fb_fmt);
          const uint32_t row = t.stride * pbytes;
          if (pbytes != tbytes || off < t.key) continue;
          if (tex_h > 1 && tex_bufw * tbytes != row) continue;   // PPSSPP stride rule
          const uint32_t delta = off - t.key;
          const uint32_t y = delta / row, x = (delta % row) / pbytes;
          if (x >= kFbWidth || y >= 512) continue;
          if (y > 0 && y + static_cast<uint32_t>(tex_h) / 4 >= kFbHeight) continue;
          if (y > 32 && 0x04000000u + t.key > 0x04110000u) continue;
          if (delta >= best_delta) continue;
          best = {i, static_cast<int>(x), static_cast<int>(y)};
          best_delta = delta;
      }
      return best;
  }

  int ge_rtt_overlap(const GeRttTarget* targets, int count, uint32_t addr, uint32_t bytes) {
      if (bytes == 0 || !in_vram(addr)) return -1;
      const uint32_t lo = addr & 0x001FFFFFu, hi = lo + bytes;
      for (int i = 0; i < count; ++i) {
          const GeRttTarget& t = targets[i];
          if (!t.valid || t.stride == 0) continue;
          const uint32_t end = t.key + t.stride * pixel_bytes(t.fb_fmt) * kFbHeight;
          if (lo < end && hi > t.key) return i;
      }
      return -1;
  }

  uint32_t ge_rtt_tex_bytes(int tex_fmt, uint32_t tex_bufw, int tex_h) {
      static const uint8_t kBits[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};
      if (tex_fmt < 0 || tex_fmt > 10 || tex_h <= 0) return 0;
      const uint32_t w = tex_bufw ? tex_bufw : 1;
      return (w * static_cast<uint32_t>(tex_h) * kBits[tex_fmt] + 7) / 8;
  }

  GeRttBlit ge_rtt_blit_rect(int x, int y, int w, int h, int scale) {
      GeRttBlit b{true, 0, 0, 0, 0, 0, 0};
      const int vw = std::min(w, static_cast<int>(kFbWidth) - x);
      const int vh = std::min(h, static_cast<int>(kFbHeight) - y);
      if (x < 0 || y < 0 || vw <= 0 || vh <= 0 || scale <= 0) return b;
      b.empty = false;
      b.src_x0 = x * scale;
      b.src_x1 = (x + vw) * scale;
      b.src_y0 = (static_cast<int>(kFbHeight) - y) * scale;       // PSP row y (top of window)
      b.src_y1 = (static_cast<int>(kFbHeight) - y - vh) * scale;  // flipped on purpose
      b.dst_w = vw * scale;
      b.dst_h = vh * scale;
      return b;
  }
  ```
- [ ] **Step 4: Run it.** Same command. Expected: `test_ge_rtt: all passed`. Then the whole build
  (`cmake --build DECO/build/rtw`, no new warnings) and `ctest --test-dir DECO/build/rtw`: 71/71.
- [ ] **Step 5: Commit** `feat(ge): render-to-texture helpers (matcher, overlap, blit rect)`, with
  `Verified:` (test RED → GREEN, ctest 71/71) and `Agent:`.

### Task 2: RTT detector — log every texture, transfer and CPU write that touches a render target

No rendering change. It answers, for the whole game, whether RTT matters. Cheap enough to stay on.

**Files:**
- Modify: `runtime/include/psp_ge_transfer.h`, `runtime/src/psp_ge_transfer.cpp` (public rect decode)
- Modify: `runtime/tests/test_ge_transfer.cpp` (rect decode test)
- Modify: `runtime/include/psp_ge.h` (`zbuf_ptr`, `zbuf_width` in `GeState`, after `framebuf_format`)
- Modify: `runtime/src/psp_ge.cpp:790-792` (store ZBUFPTR/ZBUFWIDTH), `:872` (notify before the transfer)
- Modify: `runtime/include/psp_ge_draw.h`, `runtime/src/psp_ge_draw.cpp` (target bookkeeping + detector)
- Modify: `DEBUGGING.md` (one paragraph in the `[DL]` / GE section on `[RTT]` lines)

**Interfaces:**
- Consumes: Task 1's `GeRttTarget`, `ge_rtt_find`, `ge_rtt_overlap`, `ge_rtt_tex_bytes`.
- Produces:
  ```cpp
  struct GeTransferRect { uint32_t src, dst, src_stride, dst_stride, sx, sy, dx, dy, width, height, bpp; };
  GeTransferRect psp_ge_transfer_rect(const GeTransferRegs& r);   // psp_ge_transfer.h
  void ge_draw_note_transfer(const GeTransferRegs& r);             // psp_ge_draw.h
  ```
  `GeTarget` gains `stride`, `fb_fmt`, `gl_written`, `cpu_newer`, `guest_hash` (Task 4 reads them).
  Log lines (Task 3 greps them):
  `[RTT] sample key=0x%06X[ self| depth] ta=0x%08X fmt=%d %dx%d bufw=%u fb_stride=%u fb_fmt=%u match=%d (%d,%d)`,
  `[RTT] xfer src=0x%08X dst=0x%08X %ux%u bpp=%u from_target=%d into_target=%d`,
  `[RTT] cpu-write key=0x%06X count=%u`.

- [ ] **Step 1: Failing test for the rect decode.** Append to `runtime/tests/test_ge_transfer.cpp`,
  before the final `if (failures)`:
  ```cpp
      // Rect decode shared with the RTT detector: same registers as the 32 bpp copy above.
      const GeTransferRect rc = psp_ge_transfer_rect(r);
      CHECK(rc.src == src && rc.dst == dst, "rect: full source and destination addresses");
      CHECK(rc.src_stride == 8 && rc.dst_stride == 16, "rect: strides in pixels");
      CHECK(rc.sx == 1 && rc.sy == 1 && rc.dx == 3 && rc.dy == 2, "rect: positions");
      CHECK(rc.width == 4 && rc.height == 2 && rc.bpp == 4, "rect: size and bytes per pixel");
      CHECK(psp_ge_transfer_rect(h).dst == 0x04000000, "rect: VRAM mirror folded like the copy");
  ```
  In `psp_ge_transfer.h` declare the struct and `psp_ge_transfer_rect` (doc comment: "decoded
  TRANSFER registers, PPSSPP GPUCommon::DoBlockTransfer"). In `psp_ge_transfer.cpp` add a stub
  returning `GeTransferRect{}`. Run
  `cmake --build DECO/build/rtw --target test_ge_transfer && DECO/build/rtw/test_ge_transfer.exe`.
  Expected: the five `rect:` FAIL lines.
- [ ] **Step 2: Implement** `psp_ge_transfer_rect` by moving the decode out of
  `psp_ge_block_transfer` (which then calls it):
  ```cpp
  GeTransferRect psp_ge_transfer_rect(const GeTransferRegs& r) {
      GeTransferRect t;
      t.src = address(r.src, r.srcw);
      t.dst = address(r.dst, r.dstw);
      t.src_stride = stride(r.srcw);
      t.dst_stride = stride(r.dstw);
      t.sx = r.srcpos & 0x3FF; t.sy = (r.srcpos >> 10) & 0x3FF;
      t.dx = r.dstpos & 0x3FF; t.dy = (r.dstpos >> 10) & 0x3FF;
      t.width = (r.size & 0x3FF) + 1; t.height = ((r.size >> 10) & 0x3FF) + 1;
      t.bpp = (r.start & 1) ? 4 : 2;
      return t;
  }

  void psp_ge_block_transfer(uint8_t* rdram, const GeTransferRegs& r) {
      const GeTransferRect t = psp_ge_transfer_rect(r);
      const uint32_t row_bytes = t.width * t.bpp;
      for (uint32_t y = 0; y < t.height; y++) {
          const uint32_t s = (t.src + ((t.sy + y) * t.src_stride + t.sx) * t.bpp) & kAddrMask;
          const uint32_t d = (t.dst + ((t.dy + y) * t.dst_stride + t.dx) * t.bpp) & kAddrMask;
          if (s + uint64_t{row_bytes} > kMemSize || d + uint64_t{row_bytes} > kMemSize) continue;
          std::memmove(rdram + d, rdram + s, row_bytes);
      }
  }
  ```
  Run the test again. Expected: `ge_transfer: all tests passed` (old cases included).
- [ ] **Step 3: Z buffer registers.** In `GeState` (`psp_ge.h`, after `framebuf_format`) add
  `uint32_t zbuf_ptr;   // GE_CMD_ZBUFPTR` and `uint32_t zbuf_width; // GE_CMD_ZBUFWIDTH`
  (`reset()` memsets, so no init). In `psp_ge.cpp` replace the two ignoring cases with
  `case GE_CMD_ZBUFPTR: g_ge_state.zbuf_ptr = data; break;` and
  `case GE_CMD_ZBUFWIDTH: g_ge_state.zbuf_width = data; break;`.
- [ ] **Step 4: Target bookkeeping.** In `psp_ge_draw.cpp`, add `#include "psp_ge_rtt.h"` and
  `#include "xxhash.h"` with the other includes, and extend `GeTarget`:
  ```cpp
      uint32_t stride = 0;        // FRAMEBUFWIDTH & 0x7FC of the last PRIM drawn into it
      uint32_t fb_fmt = 0;        // FRAMEBUFPIXFORMAT of the last PRIM drawn into it
      bool gl_written = false;    // a PRIM was drawn: GL holds pixels guest VRAM does not
      bool cpu_newer = false;     // guest VRAM changed after GL drew: RAM is the truth
      uint64_t guest_hash = 0;    // XXH3 of the guest window at the last check
  ```
  Then add, above `ge_draw_prim`:
  ```cpp
  // ---- RTT detector (logs only) ----
  // Which draws, transfers and CPU writes touch a GL-written target. GL never writes guest
  // VRAM, so a change of a target's guest bytes is the CPU (or a transfer) writing it.
  static int rtt_targets(GeRttTarget* out) {
      for (int i = 0; i < GE_MAX_TARGETS; ++i) {
          const GeTarget& t = g_targets[i];
          out[i] = {t.fbo != 0 && t.gl_written && !t.cpu_newer, t.key, t.stride, t.fb_fmt};
      }
      return GE_MAX_TARGETS;
  }

  static uint32_t rtt_window_bytes(const GeTarget& t) {
      return t.stride * (t.fb_fmt == 3 ? 4u : 2u) * PSP_FB_HEIGHT;
  }

  // Once per list: hash each GL-written target's guest window.
  static void rtt_check_cpu_writes(const uint8_t* rdram) {
      for (int i = 0; i < GE_MAX_TARGETS; ++i) {
          GeTarget& t = g_targets[i];
          if (t.fbo == 0 || !t.gl_written || t.stride == 0) continue;
          const uint32_t base = 0x04000000u + t.key, bytes = rtt_window_bytes(t);
          if (uint64_t{base} + bytes > 0x08000000u) continue;   // 128 MB guest image
          const uint64_t h = XXH3_64bits(rdram + base, bytes);
          if (t.guest_hash != 0 && h != t.guest_hash) {
              t.cpu_newer = true;
              static uint32_t count = 0;
              if (++count <= 16 || count % 300 == 0)
                  std::fprintf(stderr, "[RTT] cpu-write key=0x%06X count=%u\n", t.key, count);
          }
          t.guest_hash = h;
      }
  }

  // One line per distinct (target, texture address, format) the game samples.
  static void rtt_note_sample(const GeState& s) {
      const uint32_t addr = s.tex_addr[0] | (((s.tex_bufw[0] >> 16) & 0xFFu) << 24);
      const int w = 1 << (s.tex_size[0] & 0xF), h = 1 << ((s.tex_size[0] >> 8) & 0xF);
      const uint32_t bufw = s.tex_bufw[0] & 0x7FFu;
      const uint32_t bytes = ge_rtt_tex_bytes(static_cast<int>(s.tex_format), bufw, h);
      GeRttTarget t[GE_MAX_TARGETS];
      const int n = rtt_targets(t);
      int idx = ge_rtt_overlap(t, n, addr, bytes);
      bool depth = false;
      if (idx < 0 && s.zbuf_ptr != 0) {
          const GeRttTarget z = {true, (s.zbuf_ptr & 0x07FFFFFFu) & 0x001FFFF0u,
                                 s.zbuf_width & 0x7FCu, 1};
          if (ge_rtt_overlap(&z, 1, addr, bytes) == 0) { idx = 0; depth = true; }
      }
      if (idx < 0) return;
      static uint64_t seen[64];
      static int nseen = 0;
      const uint64_t k = (static_cast<uint64_t>(depth ? 0xFFu : static_cast<uint32_t>(idx)) << 56) ^
                         (static_cast<uint64_t>(addr) << 8) ^ s.tex_format;
      for (int i = 0; i < nseen; ++i) if (seen[i] == k) return;
      if (nseen < 64) seen[nseen++] = k; else return;
      const GeRttHit hit = depth ? GeRttHit{-1, 0, 0}
          : ge_rtt_find(t, n, addr, static_cast<int>(s.tex_format), bufw, h);
      const GeTarget& g = g_targets[depth ? 0 : idx];
      std::fprintf(stderr,
          "[RTT] sample key=0x%06X%s ta=0x%08X fmt=%u %dx%d bufw=%u fb_stride=%u fb_fmt=%u "
          "match=%d (%d,%d)\n",
          depth ? ((s.zbuf_ptr & 0x07FFFFFFu) & 0x001FFFF0u) : g.key,
          depth ? " depth" : (idx == g_current_target ? " self" : ""),
          addr, s.tex_format, w, h, bufw, depth ? (s.zbuf_width & 0x7FCu) : g.stride,
          depth ? 1u : g.fb_fmt, hit.index, hit.x, hit.y);
  }

  void ge_draw_note_transfer(const GeTransferRegs& r) {
      const GeTransferRect x = psp_ge_transfer_rect(r);
      const uint32_t src_bytes = ((x.sy + x.height - 1) * x.src_stride + x.sx + x.width) * x.bpp;
      const uint32_t dst_bytes = ((x.dy + x.height - 1) * x.dst_stride + x.dx + x.width) * x.bpp;
      GeRttTarget t[GE_MAX_TARGETS];
      const int n = rtt_targets(t);
      const int from = ge_rtt_overlap(t, n, x.src, src_bytes);
      const int into = ge_rtt_overlap(t, n, x.dst, dst_bytes);
      if (into >= 0) g_targets[into].cpu_newer = true;   // guest bytes now differ from GL
      if (from < 0 && into < 0) return;
      static int logged = 0;
      if (++logged <= 64)
          std::fprintf(stderr, "[RTT] xfer src=0x%08X dst=0x%08X %ux%u bpp=%u from_target=%d into_target=%d\n",
                       x.src, x.dst, x.width, x.height, x.bpp, from, into);
  }
  ```
  `ge_draw_note_transfer` needs `#include "psp_ge_transfer.h"` in `psp_ge_draw.cpp` and the
  declaration in `psp_ge_draw.h` (with `#include "psp_ge_transfer.h"` there, or a forward
  declaration `struct GeTransferRegs;`).
- [ ] **Step 5: Call sites.** In `ge_draw_prim`, right after
  `if (drawlog) drawlog_prim(state, list_index, prim_type, count);` (~line 673):
  ```cpp
      if (list_index == 0) rtt_check_cpu_writes(rdram);
      if (g_current_target >= 0) {
          GeTarget& t = g_targets[g_current_target];
          t.stride = state.framebuf_width & 0x7FCu;
          t.fb_fmt = state.framebuf_format & 3u;
          t.gl_written = true;
          // After a CPU write, GL's copy lacks the CPU's pixels until a clear redraws the
          // buffer from scratch (Task 6 would upload them instead).
          if (state.clear_mode) t.cpu_newer = false;
      }
      if (state.texture_enable && !state.clear_mode) rtt_note_sample(state);
  ```
  In `psp_ge.cpp`, `case GE_CMD_TRANSFERSTART`, call `ge_draw_note_transfer(g_transfer);` right
  before `psp_ge_block_transfer(rdram, g_transfer);`. Add a sentence to `DEBUGGING.md` (GE section,
  after the `[DL]` paragraph): "`[RTT] sample|xfer|cpu-write` lines (always on, one per distinct
  case): a texture, a block transfer or a CPU write touched a GL-rendered buffer — the cases where
  the runtime's guest VRAM differs from what the GE drew (render to texture,
  `docs/superpowers/plans/2026-10-16-render-to-texture.md`)."
- [ ] **Step 6: Build and gates.** Check port 9999, then `cmake --build DECO/build/rtw` (no new
  warnings) and `ctest --test-dir DECO/build/rtw`: 71/71. Fresh baseline + after run:
  `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/rtt_det`
  (from `DECO`), then `python -I build/tools/rt_regress.py build/tour/<baseline> build/tour/rtt_det`
  against a baseline taken with the Task 1 build at the start of the session. Expected
  `RT_REGRESS PASS`. A failure on hideout animation alone means: rerun once and compare the two
  runs with each other. `grep "\[RTT\]" build/logs/tour_rt.log`: write the lines (or "none") into
  "Findings". Run `perf_hideout.py` before and after (read its docstring for the command line).
  Expected: within 5 %.
- [ ] **Step 7: Commit** `feat(ge): RTT detector ([RTT] sample/xfer/cpu-write log lines)` with
  `Verified:` (transfer rect RED → GREEN, ctest 71/71, rt_regress PASS, perf_hideout delta, the
  tour's `[RTT]` lines or "none") and `Agent:`.

### Task 3: Coverage run with the user's main save — the gate

**Files:** none in the repo except the "Findings" section of this plan. Outside the repo:
`DECO/build/save_main/` (copy of the main save), `DECO/build/logs/jugar.log` (the user's run).

- [ ] **Step 1: Rebuild for the user** (Task 2's build is already in `build/rtw`; check that
  `build/rtw/psprecomp_runtime.exe` is newer than the Task 2 commit). Ask the user, in Spanish, to put
  their main save in `DECO/savedata` **themselves** (Claude never overwrites `DECO/savedata`), or to
  say where it is so Claude copies it to `DECO/build/save_main` for the tours.
- [ ] **Step 2: Ask the user to play with `jugar.bat`** for as long as they want, and to pass through as
  many of these as their save allows (give them the list in Spanish):
  title screen and hero pages; the whole hideout (every building, the Cuartel equipment screen
  with the hero preview, class change, the forge/upgrade screens, the Altar/Tótem); the
  world map; a normal mission; a boss; the Arena del Valor; a Versus/Dark Hero fight if available;
  FEVER, a summon (Invocar), a Purificar and a level-up; mission clear/fail screens; the pause
  menu; a cutscene; save and load. Ask them to note the time or the place of anything that
  looks wrong or different from PPSSPP.
- [ ] **Step 3: Read the evidence** when they close the game:
  `grep "\[RTT\]\|FBO target created\|FBO pool overflow\|\[GE\] block transfer" DECO/build/logs/jugar.log | sort | uniq -c`.
  For every distinct `[RTT]` line, note the time (line number vs nearby log lines) and match it to
  what the user was doing.
- [ ] **Step 4: Reproduce each hit on both sides.** For each screen that produced `[RTT]` lines,
  bring a test instance there (tour from `build/save_main`, checkpoints written as in
  `build/tools/gfx_arena.json`; the route goes into `build/tools/gfx_rtt.json`), capture
  runtime vs PPSSPP (`--serve`, hook as in the arena plan), and check whether the runtime differs
  at the place the hit describes. A hit with no visible difference (e.g. the game samples a target
  it fully redraws first) is recorded but does not count for the branch.
- [ ] **Step 5: Decide the branch** and write `## Findings` at the end of this plan: the `[RTT]` lines,
  the screens, the side-by-side PNGs (paths), and the branch:
  - **A**: `sample` hits with `match>=0` on a visible difference → Task 4.
  - **A'**: `sample` hits with `match=-1` (CLUT/depal, format reinterpret, depth): write a Task 4b
    in the format of Task 4, from PPSSPP's code for that case (depal: `GPU/Common/DepalettizeShaderCommon.cpp`;
    depth: `MatchFramebuffer` RASTER_DEPTH), with a failing test first, and execute it.
  - **B**: `xfer ... from_target>=0` → Task 5. **C**: `cpu-write` on a buffer that is displayed or
    sampled afterwards, with a visible difference → Task 6.
  - Several apply: A first, then B, then C, one commit each.
- [ ] **Step 6: Branch D (no hit at all).** Do not implement anything blind: it could not be verified
  against PPSSPP. Tell the user, in Spanish, what was covered and that Patapon 3 did not use RTT
  there. Recommend keeping the detector (it will log as soon as some later screen needs RTT) and
  moving to the next item of the maturity list (wider tour coverage). Ask the user whether they
  still want Task 4 written and merged with only the unit tests and A/B evidence. Do what they
  say; record it under "Findings".

### Task 4 (branch A): sample render targets through a scratch copy

**Files:**
- Modify: `runtime/src/psp_ge_draw.cpp` (scratch copy before the PRIM state, bind at the texture step)
- Modify: `runtime/include/psp_ge_texture.h`, `runtime/src/psp_ge_texture.cpp` (public sampler apply)
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_NO_RTT`), `docs/GRAPHICS.md` (RTT paragraph),
  `README.md` (drop "render-to-texture is not implemented" from the Patapon 3 row and the
  status paragraph), `ARCHITECTURE.md` (if it describes the texture path as RAM-only, say where
  RTT plugs in)

**Interfaces:**
- Consumes: Task 1 `ge_rtt_find`, `ge_rtt_blit_rect`; Task 2 `GeTarget` fields and `rtt_targets`.
- Produces: `bool ge_texture_apply_sampler(const GeState& state);` (psp_ge_texture.h).

- [ ] **Step 1: Capture the failing evidence.** On the Task 3 screen, both sides, save
  `build/tour/rtt_before/{rt,pp}_*.png` and the `[DL]` lines (`D 1`) of the sampling PRIM
  (`ta=` in the `[RTT] sample` line). This is the RED: the runtime differs from PPSSPP in the
  sampled area. Write down the pixel test that will prove the fix: a crop box and the
  colour/MAD threshold against PPSSPP (e.g. "box (x0,y0,x1,y1) MAD < 8 vs pp, was > 40").
- [ ] **Step 2: Public sampler apply.** In `psp_ge_texture.h`:
  `/// Apply the GE filter/wrap registers to the texture bound on unit 0 (render-target textures).`
  `bool ge_texture_apply_sampler(const GeState& state);` and in `psp_ge_texture.cpp`, after
  `apply_sampler`: `bool ge_texture_apply_sampler(const GeState& state) { return apply_sampler(state, 0); }`.
- [ ] **Step 3: Scratch copy.** In `psp_ge_draw.cpp`, after the detector block:
  ```cpp
  // ---- Render to texture (PPSSPP MatchFramebuffer + CopyFramebufferForColorTexture) ----
  // A texture that is a GL-rendered target is sampled from that target's FBO: its w x h window
  // is copied into one scratch texture before the PRIM's state is set (no feedback loop when a
  // draw samples its own target; texels outside the 480x272 target stay transparent black).
  // PSPRECOMP_NO_RTT=1 samples guest RAM as before.
  static GLuint g_rtt_fbo = 0, g_rtt_tex = 0;
  static int g_rtt_w = 0, g_rtt_h = 0;

  static bool rtt_disabled() {
      static const bool off = std::getenv("PSPRECOMP_NO_RTT") != nullptr;
      return off;
  }

  // Returns true when the scratch texture holds this PRIM's texture.
  static bool rtt_prepare(const GeState& s) {
      if (rtt_disabled() || !s.texture_enable || s.clear_mode || g_current_target < 0) return false;
      const uint32_t addr = s.tex_addr[0] | (((s.tex_bufw[0] >> 16) & 0xFFu) << 24);
      const int w = 1 << (s.tex_size[0] & 0xF), h = 1 << ((s.tex_size[0] >> 8) & 0xF);
      GeRttTarget t[GE_MAX_TARGETS];
      const int n = rtt_targets(t);
      const GeRttHit hit = ge_rtt_find(t, n, addr, static_cast<int>(s.tex_format),
                                       s.tex_bufw[0] & 0x7FFu, h);
      if (hit.index < 0) return false;
      const int sc = g_render_scale;
      if (g_rtt_tex == 0) {
          glGenTextures(1, &g_rtt_tex);
          glGenFramebuffers(1, &g_rtt_fbo);
      }
      glActiveTexture(GL_TEXTURE0);
      if (g_rtt_w != w * sc || g_rtt_h != h * sc) {
          g_rtt_w = w * sc;
          g_rtt_h = h * sc;
          gl_state_bind_texture(g_rtt_tex);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_rtt_w, g_rtt_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
          glBindFramebuffer(GL_FRAMEBUFFER, g_rtt_fbo);
          glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rtt_tex, 0);
      }
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_rtt_fbo);
      glDisable(GL_SCISSOR_TEST);                          // re-set by the PRIM right after
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);     // re-set by the PRIM right after
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);                // clears set their own colour
      glClear(GL_COLOR_BUFFER_BIT);
      const GeRttBlit b = ge_rtt_blit_rect(hit.x, hit.y, w, h, sc);
      if (!b.empty) {
          glBindFramebuffer(GL_READ_FRAMEBUFFER, g_targets[hit.index].fbo);
          glBlitFramebuffer(b.src_x0, b.src_y0, b.src_x1, b.src_y1, 0, 0, b.dst_w, b.dst_h,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, g_targets[g_current_target].fbo);
      // A 5650 buffer has no alpha: the PSP samples it opaque.
      gl_state_bind_texture(g_rtt_tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A,
                      s.tex_format == GE_TFMT_5650 ? GL_ONE : GL_ALPHA);
      static int logged = 0;
      if (++logged <= 32)
          std::fprintf(stderr, "[RTT] sampling key=0x%06X at (%d,%d) %dx%d into scratch %dx%d\n",
                       g_targets[hit.index].key, hit.x, hit.y, w, h, g_rtt_w, g_rtt_h);
      return true;
  }
  ```
  Check that `GE_TFMT_5650` is the name in `psp_ge_constants.h` (`grep -n "TFMT" runtime/include/psp_ge_constants.h`);
  use the existing name if it differs.
- [ ] **Step 4: Hook it.** In `ge_draw_prim`, right before the `// Scissor (GE_CMD_SCISSOR1/2...`
  comment block (~line 679): `const bool rtt_ready = rtt_prepare(state);`. At the texture step
  (~line 1001) replace `ge_texture_bind(rdram, g_frame_counter, draw_max_v);` with
  ```cpp
          if (rtt_ready) {
              gl_state_bind_texture(g_rtt_tex);
              ge_texture_apply_sampler(state);
          } else {
              ge_texture_bind(rdram, g_frame_counter, draw_max_v);
          }
  ```
  In `ge_draw_shutdown` delete `g_rtt_fbo` / `g_rtt_tex` when non-zero (call
  `gl_state_note_texture_deleted(g_rtt_tex)` first) and zero `g_rtt_w/h`. Check that nothing between
  the scissor block and the texture step returns early for a textured PRIM after binding another
  texture (read the lines; the RTT texture must still be the one bound at the draw).
- [ ] **Step 5: Build and unit gates.** Port 9999 check, `cmake --build DECO/build/rtw` (no new
  warnings), `ctest`: 71/71.
- [ ] **Step 6: GREEN on the screen.** Same capture as Step 1 into `build/tour/rtt_after`. Expected:
  the Step 1 pixel test passes (runtime matches PPSSPP in the box) and `[RTT] sampling` lines
  appear. Then the same run with `PSPRECOMP_NO_RTT=1` in the environment: the old difference comes
  back (proves the switch is the cause). Side by side `build/tour/rtt_after/comparacion.png`
  (PPSSPP / before / after). Also check once at `PSPRECOMP_RENDER_SCALE=4` (the user's
  `jugar.bat` setting): same image, sharper. Look for `GL_INVALID` in the log (a blit with a bad
  rect), expected none.
- [ ] **Step 7: Regression.** `rt_regress` against the session baseline, expected PASS (same
  animation-phase rule as Task 2). Look at the mission checkpoints and the opening movie path
  (Review Focus 1). `perf_hideout.py` within 5 %.
- [ ] **Step 8: Docs.** `docs/ENV_FLAGS.md`: `PSPRECOMP_NO_RTT=1` — sample render targets from guest RAM
  (pre-RTT behaviour; A/B switch). `docs/GRAPHICS.md`: one paragraph — when a texture is a
  GL-rendered target (matcher rules, scratch copy before the draw, 5650 opaque, CLUT/DXT/depth
  not handled). `README.md`: drop the RTT gap; keep the other known gaps. Check
  `ARCHITECTURE.md`: in the Renderer row (~line 179) add "render-target textures sampled from the
  FBO through a scratch copy (matcher pure in `include/psp_ge_rtt.h`, unit-tested)".
- [ ] **Step 9: Commit** `feat(ge): sample render targets as textures (render to texture)` with
  `Verified:` (screen box MAD before/after vs PPSSPP, NO_RTT A/B, ctest 71/71, rt_regress PASS,
  perf delta) and `Agent:`.

### Task 5 (branch B, write when the evidence exists): GL → guest VRAM readback

Write this task only if Task 3 logged `[RTT] xfer ... from_target>=0` (or the CPU visibly reads
a GL-rendered buffer: a screenshot-based effect, a save icon). Model:
`FramebufferManagerCommon::NotifyBlockTransferBefore` (:2622) and `ReadFramebufferToMemory` (:3299).
Before the transfer, `glReadPixels` the source rect of the target FBO (at scale > 1, blit to a
scale-1 FBO first), convert RGBA8 to the target's pixel format (5650/5551/4444/8888; PPSSPP
`ConvertFromRGBA8888` in `Common/Data/Convert/ColorConv.cpp`), write the guest bytes, then run the
memory transfer. The conversion is a pure function: write its failing test first
(`test_ge_rtt.cpp`, known RGBA8 → 5551 words), then the GL part. Verify on the Task 3 screen like
Task 4 Step 6.

### Task 6 (branch C, write when the evidence exists): guest → GL upload

Write this task only if Task 3 shows a `cpu-write` buffer that is displayed or sampled with a
visible difference. Model: PPSSPP `FramebufferManagerCommon::DrawPixels` / `NotifyVideoUpload`
(grep `DrawPixels` in `GPU/Common/FramebufferManagerCommon.cpp`). When `cpu_newer` is set and the
buffer is presented or sampled, decode the guest window (existing texture decoder for the
buffer's format, bufw = stride) and draw it into the FBO, then clear `cpu_newer`. Failing test first for
any new pure helper; verify on the screen like Task 4 Step 6.

## Finish

- Final whole-branch review (fresh reviewer on the most capable model; if the user asks for
  "nativo", a self-review recorded as such) over the task commits.
- The play exe is `build/rtw/psprecomp_runtime.exe`; rebuild it so the user's game has the change.
- Tell the user (in Spanish) what Task 3 found, what changed, the gates, and ask them to replay
  the screens from Task 3 with `jugar.bat` and `jugar_60fps.bat`. No push before their OK.

## Findings

(Task 2 Step 6 and Task 3 write here.)

### Task 2 (2026-10-16): detector on the graphics tour

- `gfx_tour.py --only rt` (32 checkpoints, title to mission reward, `build/tour/rtt_det`): **no
  `[RTT]` line at all** — no sample, no xfer, no cpu-write. Only the two display buffers were
  created (`FBO target created key=0x000000` / `key=0x044000`), no `[GE] block transfer` line.
- `rt_regress build/tour/rtt_base build/tour/rtt_det`: PASS (21 gated checkpoints).
- `perf_hideout`: fps 30.0 / 30.0, ge_ms/frame 4.1 / 4.1, CPU 33.8 % / 33.9 % (second sample
  33.6 %). `cpu_wait_ms/s` 111.5 → 140.0 / 131.5: that counter is guest threads waiting for the
  CPU lock (`psp_cpu.cpp`), not the render thread where the detector runs, and earlier
  `build/tour/perf_*` samples range 100-246 for it, so it is noise, not detector cost.
