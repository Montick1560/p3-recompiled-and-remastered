# GE stencil (FEVER worm fire mask) — implementation plan

Written 2026-10-14 (Opus 5.5, end of the relevo review) for the next session. Status: not started.

## Problem

User report (own save, a miracle unlocked): during FEVER the fire texture that should fill only the
FEVER worm is drawn as a full rectangle behind the worm (screenshot in the 2026-10-14 conversation;
top-left, ~580x430 px at scale 4). Not a relevo regression: the runtime has **no GE stencil at all**.

- Render targets are created with `GL_DEPTH_COMPONENT24` only (`runtime/src/psp_ge_draw.cpp`,
  `make_target`, ~line 261): no stencil bits.
- `GE_CMD_STENCILTESTENABLE` / `GE_CMD_STENCILTEST` / `GE_CMD_STENCILOP` are parsed into `GeState`
  (`runtime/src/psp_ge.cpp:658/696/699`) and never applied. `GL_STENCIL_TEST` is tracked by the GL
  shadow (`psp_gl_state.cpp`) but nothing turns it on.
- The clear path already maps clear flag bit 1 to `GL_STENCIL_BUFFER_BIT` (`psp_ge_draw.cpp:737`) but
  there is no stencil buffer to clear and no `glClearStencil`.

On the PSP the stencil **is the alpha channel of the color buffer** (8888: 8 bits, 4444: 4, 5551: 1,
565: none). A game masks a draw by writing stencil (STENCILOP REPLACE, often with RGB masked) and then
drawing with the stencil test (or a DST_ALPHA blend, since alpha == stencil). The worm fire is almost
certainly one of those two. Step 1 decides which before any code is written.

## Reference (PPSSPP, `tools/ppsspp-src`)

- Register decode: `GPU/GPUState.h:274-279` (func = stenciltest & 7, ref = >>8 & 0xFF,
  mask = >>16 & 0xFF; sfail/zfail/zpass = stencilop bytes 0/1/2, & 7).
- GL state: `GPU/Common/GPUStateUtils.cpp:1405` `ConvertStencilFuncState`: write mask =
  `~pmska & 0xFF` (MASKALPHA is the stencil write mask, inverted); the REPLACE/REPLACE/REPLACE
  "write only stencil" case switches the test to ALWAYS when RGB is masked and depth write is off;
  per-format tweaks (565: no writes; 5551: `ConvertStencilMask5551` / `ConvertStencilFunc5551`).
- Stencil value into color alpha: `GPUStateUtils.cpp:183` `ReplaceAlphaWithStencil` and `:207`
  `ReplaceAlphaWithStencilType` (KEEP / ZERO / ONE / UNIFORM ref / INVERT / INCR/DECR by format);
  the fragment shader writes that value as alpha (`GPU/Common/FragmentShaderGenerator.cpp:169`,
  `FS_BIT_REPLACE_ALPHA_WITH_STENCIL_TYPE`). `IsStencilTestOutputDisabled` (`:34`) says when not to.
- Software reference for exact semantics: `GPU/Software/DrawPixel.cpp:252` `ApplyStencilOp`.

## Steps

1. **Diagnose (no code).** Test save: copy of the user's `DECO/savedata` in `DECO/build/save_milagro`
   (if the copy is missing, ask the user to run the copy command; never point a test at `savedata/`
   itself — p3drive copies the save it is given into the instance, so pass the copy via `P3DRIVE_SAVE`).
   Instance on port 9998, scale 1, no pack. Reach FEVER in a mission (drum.py; at scale 4 the beat
   detector failed today, use scale 1). Dump the display list of a FEVER frame (`D <n>` on the debug
   socket / `gfx_tour.gelist_dump`) and the same frame in PPSSPP (`build/ppsspp_mp`, same save; ask the
   user before closing any PPSSPP). Find the worm draws: expect a stencil-write pass (STE=1,
   STENCILOP REPLACE, MASKRGB set) followed by the fire quad with STE=1 func EQUAL/NOTEQUAL — or a
   DST_ALPHA blend. Write the finding into this file.
2. **Stencil buffer.** `GL_DEPTH24_STENCIL8` renderbuffer attached as `GL_DEPTH_STENCIL_ATTACHMENT`.
   Clear: when clear flag bit 1 is set, `glClearStencil(clear alpha)` with the stencil write mask, so
   alpha and stencil start equal. Check the present/blit and FBO pool paths still work.
3. **Pure mapper, TDD.** `include/psp_ge_stencil.h` (+ `tests/test_ge_stencil.cpp`): from
   (stencil enable, STENCILTEST, STENCILOP, MASKALPHA, MASKRGB, depth test/write, framebuf format)
   → { enabled, GL func, ref, read mask, sfail/zfail/zpass GL ops, write mask, stencil-to-alpha
   type }. Port `ConvertStencilFuncState`, the 5551 conversions and `ReplaceAlphaWithStencilType`
   case by case, with one test per case citing the PPSSPP line.
4. **Apply in `ge_draw_prim`.** Through the GL shadow: add stencil func/op/write-mask tracking to
   `GlShadow` (pure, extend `test_gl_shadow.cpp`), set `GL_STENCIL_TEST` via `gl_state_set_cap`.
   Clear-mode draws keep their own path.
5. **Stencil → alpha.** When the mapper says stencil is written, the uber-shader outputs alpha =
   stencil value (uniform: type + ref) so later DST_ALPHA blends and the present see what the PSP
   would. Masked RGB already works (`ge_color_write_mask`).
6. **Gates.** ninja + ctest; `rt_regress` with `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00` (static points
   must stay 0.00 %; sukopon/obelisco move with animation phase, see BITACORA 2026-10-14); full
   PPSSPP tour + `gfx_score` no worse than `build/tour/relevo_base_pp`; a FEVER-with-miracle capture
   (runtime vs PPSSPP, same save) showing the fire only inside the worm; `perf_hideout` unchanged.
   Then the user plays a mission with FEVER.

## Risks

- Alpha == stencil coupling: anything that today writes vertex/texture alpha into the target and later
  reads it (DST_ALPHA blends, the present) can change once stencil writes replace alpha. That is the
  correct PSP behaviour, but check the hideout sky/fades and menus against PPSSPP.
- 4444/5551 targets: Patapon 3 renders 8888 as far as the tours have seen; log the format of any
  stencil draw and keep 5551 per PPSSPP.
- Line endings: the repo is CRLF on this checkout; prefer the Edit tool (see the PPSSPP AGENTS.md note
  on `newline=''`).
