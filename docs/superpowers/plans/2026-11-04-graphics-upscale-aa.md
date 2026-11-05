# Texture Upscaling (xBRZ) and Screen Anti-Aliasing (FXAA / SMAA) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** two new launcher options for players without the HD pack:
- *Escalado de texturas* (off / xBRZ 2x / 3x / 4x);
- *Suavizado* (off / FXAA / SMAA).
Both are cheap enough for weak PCs and change nothing when off.

**Architecture:**
- **Upscaling** hooks the texture cache's miss path in `runtime/src/psp_ge_texture.cpp`, in the branch that decodes
  a texture from guest RAM: the HD pack, patched textures and shared pack textures keep winning.
  - The decoded RGBA goes to a worker queue (`TexUpscaleQueue`) that runs xBRZ.
  - Until the result is Ready, the original is uploaded, and the cache entry retries on later hits when the queue's
    epoch changes: the same pattern as `replace_pending` / `ge_texrep_epoch()` for the pack.
  - Textures whose content changes often (palette animation) are skipped (`TexChangeTracker`).
  - Scaled bytes count against the GL budget (`account_slot` / `trim_gl_budget`).
- **Anti-aliasing** runs in `ge_draw_present` (`psp_ge_draw.cpp:~1330`, before `glBlitFramebuffer`). With AA on:
  - the frame's colour texture is drawn through the FXAA or SMAA passes into a post FBO at the internal resolution;
  - the existing blit then scales that FBO to the window, unchanged.
  - Overlays (OSK, prompts) are drawn after, as now. Screenshots (`present_service_screenshot`) stay pre-AA, so the
    tour gate is unaffected.
- **Settings:**
  - runtime core keys in `psp_settings.cpp`: `display.texture_upscale` → `PSPRECOMP_TEX_UPSCALE`,
    `display.antialiasing` → `PSPRECOMP_AA`;
  - `docs/contracts/settings-v1.md`;
  - the launcher's data-driven catalog `src-tauri/core_options.json` (tab `graphics`).

**Tech Stack:** C++ runtime (OpenGL 3.3 core / GLSL ES 3.0 under ANGLE via `psp_glsl_header()`), xBRZ 1.8 (GPL-3.0,
vendored), SMAA 1.0 (MIT, vendored), FXAA (MIT port); launcher Tauri (Rust + React); ctest, cargo test, vitest.

**Spec:** `docs/superpowers/specs/2026-11-04-graphics-upscale-aa-design.md`.

## Global Constraints

- **Defaults off.** With both options off, the output is identical to today. Check with a 30 fps tour against
  `build/tour/mask_new`; the only allowed difference is the known `19_mission_prep` 0.54 %.
- **No hitch on the render thread.** xBRZ never runs on the render thread. A texture miss with upscaling on costs no
  more than today's miss plus one copy into the queue.
- **ANGLE / OpenGL ES 3.0.**
  - Every new shader uses `psp_glsl_header(api, fragment)`.
  - Boot once with `PSPRECOMP_GL_API=es` to confirm FXAA and SMAA compile.
- **Licences.**
  - xBRZ goes to `runtime/third_party/xbrz/` with its GPL-3.0 header and a README line. The exe is already GPL-3.0
    (README "License").
  - SMAA goes to `runtime/third_party/smaa/` with its MIT notice.
  - The FXAA file header credits its MIT source.
- **Never ship game art.**
- **Lab.** Port 9998 only, never 9999. Run `python -I build/infra/diskquiet.py 30 300` before measuring. Close only
  processes this run started.
- **Git.**
  - psprecomp: branch `graphics-options` from `effect-select` (51ad229).
  - Launcher: branch `graphics-options` from `gles` (1cf7b1b, 0.3.0-beta.11).
  - Author "Patapon Community" `<patapon-community@users.noreply.github.com>`, no Co-Authored-By. Never push without
    the user's word.
- **Workflow.** Native execution. Spanish with the user; code, comments and commits in English.
- **Budget.** 45 min or 3 attempts per step, then ledger `build/autopilot/LEDGER_graphics_2026-11-04.md` and move on.

## Review Focus

1. **Palette-animated textures** (FEVER worm, flashing UI): never re-scaled every frame. Check with the trace counter
   from Task 3: scale jobs per second stay low in a FEVER window.
2. **VRAM:** xBRZ 4x on a weak GPU must not crash. Scaled textures over the GL budget get trimmed like pack textures;
   a texture whose scaled side would exceed 2048 is not scaled.
3. **HD pack on and upscaling on:** pack textures are never upscaled, and pack loading still works (`hd_textures`).
4. **Render scale 1 + FXAA/SMAA at 480×272:** the help-bar text stays readable. If it is not, AA runs before the
   overlays only (it already does). Ledger what the text looks like.
5. **Window resize / fullscreen toggle (F11) with AA on:** the post FBO follows the internal resolution
   (`fb_w()`/`fb_h()`), not the window, and is rebuilt if those change.

---

### Task 1: xBRZ wrapper (pure)

**Files:**
- Create: `runtime/third_party/xbrz/{xbrz.h,xbrz.cpp,xbrz_config.h,xbrz_tools.h,License.txt}`, xBRZ 1.8 as PPSSPP
  vendors it (github.com/hrydgard/ppsspp `ext/xbrz`, the version matching `tools/ppsspp-src`). Plus a `README.md`
  line in `runtime/third_party/`.
- Create: `runtime/include/psp_tex_upscale.h`, `runtime/src/psp_tex_upscale.cpp`.
- Test: `runtime/tests/test_tex_upscale.cpp` (+ `runtime/CMakeLists.txt` test target; `xbrz.cpp` also joins the
  runtime sources).

**Interfaces:**
- Produces:
  - `int tex_upscale_factor_parse(const char* env)`: "xbrz2" → 2, "xbrz3" → 3, "xbrz4" → 4, anything else → 0.
  - `bool tex_upscale_xbrz(int factor, const uint8_t* rgba, int w, int h, std::vector<uint8_t>* out)`: RGBA8 rows
    in and out. False when the factor is not 2..4, w or h ≤ 0, or `w*factor` / `h*factor` > 2048.

- [ ] **Step 1:** Write `test_tex_upscale.cpp`:
  - parse: `xbrz2/3/4` → 2/3/4; `""`, `nullptr`, `off`, `xbrz5` → 0;
  - a 4×4 single colour (`10 20 30 FF`) at 2x gives 8×8, every pixel equal;
  - a 4×4 with a transparent left half keeps alpha 0 on the left and 255 on the right of the 2x output;
  - output size `w*f × h*f × 4`;
  - rejects a factor of 5, a 0 width, and 1024×8 at 3x (3072 > 2048).
- [ ] **Step 2:** `ninja test_tex_upscale && ./test_tex_upscale` in `build/rtw`.
  Expected: build fails (no `psp_tex_upscale.h`).
- [ ] **Step 3:** Implement. xBRZ takes `uint32_t` ARGB (`xbrz::ColorFormat::ARGB`): pack each RGBA pixel to ARGB,
  scale, unpack. Use the PPSSPP call shape in `GPU/Common/TextureScalerCommon.cpp:651`.
- [ ] **Step 4:** Rerun the test.
  Expected: `test_tex_upscale: all passed`.
- [ ] **Step 5:** Commit "feat(runtime): xBRZ texture upscaling helper (vendored xBRZ 1.8, GPL-3.0)".

### Task 2: Change tracker and upscale queue (pure)

**Files:**
- Create: `runtime/include/psp_tex_upscale_queue.h`, `runtime/src/psp_tex_upscale_queue.cpp`.
- Test: `runtime/tests/test_tex_upscale_queue.cpp`.

**Interfaces:**
- Consumes: Task 1's `tex_upscale_xbrz`.
- Produces:
  - `class TexChangeTracker`:
    - `bool changes_often(uint32_t addr, uint32_t hash, uint32_t list)` records the hash seen at `addr` in display
      list `list`;
    - returns true when that address got a new hash at least twice within the last 60 lists;
    - bounded map, 4096 addresses, oldest evicted.
  - `class TexUpscaleQueue`:
    - `TexUpscaleQueue(ScaleFn fn, size_t budget_bytes)` with
      `using ScaleFn = std::function<bool(int f, const uint8_t*, int w, int h, std::vector<uint8_t>*)>`;
    - `State request(uint64_t key, int factor, const uint8_t* rgba, int w, int h, const TexUpscaleImage** out)`,
      with `State` Pending / Ready / Failed. The source is copied only on the first request of a key;
    - `uint64_t ready_epoch() const`; `void wait_idle()`; `void release(uint64_t key)`;
    - one worker thread, results kept under a byte budget (reuse `TexrepLru` from `psp_texrep_lru.h`). Same
      threading rules as `TexrepImageQueue`: the worker only inserts, eviction happens on the render thread.
  - `struct TexUpscaleImage { int w, h; std::vector<uint8_t> rgba; }`.

- [ ] **Step 1:** Write the tests.
  - Tracker: same hash every list → false; a new hash at lists 10 and 20 → true at 20; new hashes at lists 10 and
    100 → false (apart by more than 60).
  - Queue, with a fake ScaleFn that doubles w/h and counts calls:
    - first request Pending; after `wait_idle` Ready with 2w × 2h; `ready_epoch` increased;
    - a second request of the same key does not call ScaleFn again;
    - a ScaleFn returning false gives Failed;
    - with a budget of one image, a second Ready key evicts the first (its next request is Pending again).
- [ ] **Step 2:** Run.
  Expected: build fails (no header).
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run.
  Expected: all pass.
- [ ] **Step 5:** Commit "feat(runtime): background texture upscale queue and change tracker".

### Task 3: Upscale textures in the cache

**Files:**
- Modify: `runtime/src/psp_ge_texture.cpp`:
  - init (`ge_texture_init`: parse `PSPRECOMP_TEX_UPSCALE`, create the queue, log `[TEX] upscale: xBRZ <f>x` or
    nothing when off);
  - `TexCacheEntry` gets `upscale_pending` and `upscale_epoch`;
  - the miss path's decode branch (the `else` that calls `ge_decode_texture`);
  - the hit path: retry next to the `replace_pending` retry.
- Modify: `docs/ENV_FLAGS.md` (`PSPRECOMP_TEX_UPSCALE`, plus `PSPRECOMP_TEX_UPSCALE_TRACE=1`: one line per second
  with jobs queued, Ready uploads, skipped "changes often", skipped too large).

**Interfaces:**
- Consumes: Task 1 and Task 2 APIs.
- Key: `uint64_t(psp_addr) << 32 ^ content_hash`, mixed with the factor.

- [ ] **Step 1:** In the decode branch, upload the scaled image when the queue says Ready, and use the factor when
  accounting the bytes. Otherwise upload the original as today, set `upscale_pending` (unless Failed, "changes
  often" or too large) and `upscale_epoch = ready_epoch()`.
- [ ] **Step 2:** In the hit path, when `upscale_pending` and the epoch moved: request again. If Ready, upload,
  `account_slot`, `trim_gl_budget`, clear `upscale_pending`.
- [ ] **Step 3:** `ninja && ctest --output-on-failure` in `build/rtw`.
  Expected: all pass.
- [ ] **Step 4:** Lab, 1.1.4, Wooyari mission (`build/wooyari/wy_run.sh`; the env list is passed through),
  `PSPRECOMP_RENDER_SCALE=2`, recorded with `rec_win.py`. Runs: `PSPRECOMP_TEX_UPSCALE=xbrz2`, `xbrz4`, off.
  Expected:
  - trace: jobs settle (no steady stream in FEVER);
  - the frame log has no frame > 50 ms that the off run lacks;
  - side-by-side frames show sharper edges.
  Save the comparison PNGs to `build/graficos/`.
- [ ] **Step 5:** With `PSPRECOMP_TEXTURES` (HD pack) on plus `xbrz2`: pack textures load as before (`[TEXREP]`
  lines); the trace shows no job for them.
- [ ] **Step 6:** Commit "feat(runtime): optional xBRZ upscaling of the game's own textures (PSPRECOMP_TEX_UPSCALE)".

### Task 4: Screen anti-aliasing (FXAA, then SMAA)

**Files:**
- Create: `runtime/include/psp_present_aa.h`, `runtime/src/psp_present_aa.cpp`;
  `runtime/third_party/smaa/{SMAA.hlsl,AreaTex.h,SearchTex.h,LICENSE}` (iryoku/smaa, MIT).
- Modify: `runtime/src/psp_ge_draw.cpp` `ge_draw_present`: when AA is on, the blit reads the post FBO instead of
  `fbo`.
- Test: `runtime/tests/test_present_aa.cpp` (parse only).

**Interfaces:**
- Produces:
  - `enum class PspAa { Off, Fxaa, Smaa }`;
  - `PspAa psp_aa_parse(const char* env)` ("fxaa" / "smaa", any case; else Off);
  - `GLuint psp_present_aa_apply(PspAa mode, GLuint color_tex, int w, int h)`: returns the FBO to blit from (0 when
    Off or on failure, and then the caller blits `fbo` as today).

- [ ] **Step 1:** Write `test_present_aa.cpp` for `psp_aa_parse`. Run it and watch it fail; implement the parse;
  watch it pass.
- [ ] **Step 2:** FXAA.
  - One full-screen-triangle pass at w × h into a lazily created post FBO (RGBA8, linear).
  - Rebuilt when w/h change (Review Focus 5).
  - Shader port of the MIT `glsl-fxaa` (mattdesl), header from `psp_glsl_header`.
  - Compile errors log once and fall back to Off.
- [ ] **Step 3:** SMAA.
  - Three passes, all at w × h: luma edge detection, blending weights (AreaTex / SearchTex uploaded once),
    neighbourhood blending into the post FBO.
  - `SMAA_PRESET_MEDIUM`, GLSL through `SMAA_GLSL_3`; ES 3.0 needs `precision highp float` (already in the header).
- [ ] **Step 4:** `ninja && ctest` in `build/rtw`.
  Expected: all pass.
- [ ] **Step 5:** Lab: Wooyari mission at `PSPRECOMP_RENDER_SCALE=1` and `2` with `PSPRECOMP_AA=fxaa`, `smaa`, off,
  recorded with `rec_win.py` (window capture, so post-AA).
  - Comparison PNG crops of the same edges go to `build/graficos/`.
  - `[PERF]` lines: GPU ms/frame of each.
  - One boot with `PSPRECOMP_GL_API=es`: no shader error, AA visible.
  - Review Focus 4: the help-bar text at scale 1.
- [ ] **Step 6:** Commit "feat(runtime): FXAA and SMAA screen anti-aliasing (PSPRECOMP_AA)".

### Task 5: Settings and launcher options

**Files:**
- Modify: `runtime/src/psp_settings.cpp` (`kCore`), `runtime/tests/test_settings.cpp`,
  `docs/contracts/settings-v1.md`.
- Modify (launcher repo): `src-tauri/core_options.json`, and the launcher tests that list the graphics options.

**Interfaces:**
- `{"display","texture_upscale","PSPRECOMP_TEX_UPSCALE",Kind::Enum,"off",0,0,"off,xbrz2,xbrz3,xbrz4"}`.
- `{"display","antialiasing","PSPRECOMP_AA",Kind::Enum,"off",0,0,"off,fxaa,smaa"}`.
- Launcher text (es / en):

| Option | Label | Values | Description |
| --- | --- | --- | --- |
| `texture_upscale` | "Escalado de texturas" / "Texture upscaling" | "Apagado", "xBRZ 2x", "xBRZ 3x", "xBRZ 4x" | "Agranda las texturas del juego con bordes nítidos (como PPSSPP). Úsalo si no usas el pack HD. 2x es ligero; 4x pide más memoria de vídeo." |
| `antialiasing` | "Suavizado" / "Anti-aliasing" | "Apagado", "FXAA", "SMAA" | "Suaviza los bordes dentados. FXAA es el más ligero; SMAA se ve mejor y cuesta un poco más." |

- [ ] **Step 1:** Add test cases to `test_settings.cpp`:
  - `texture_upscale = xbrz3` → `PSPRECOMP_TEX_UPSCALE=xbrz3`;
  - an invalid value is logged and sets nothing;
  - the same two checks for `antialiasing`.
  Run and watch them fail.
- [ ] **Step 2:** Add the keys; tests pass. Update the contract doc (the ini sample and the key table).
- [ ] **Step 3:** Launcher:
  - add both entries to `core_options.json` after `texture_filter`;
  - `cargo test` (src-tauri) and `npm test` pass;
  - the Options screen shows them on the graphics tab (launcher dev run, screenshot).
- [ ] **Step 4:** Commit in each repo: "feat(settings): texture upscaling and anti-aliasing options".

### Task 6: Gate, releases, user kit

- [ ] **Step 1:** Rebuild `build/rtw`, `build/v115b/rtw`, `build/vanilla/rtw`; ctest in each.
- [ ] **Step 2:** 30 fps tour with both options off:
  `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-22T08:48:00 python -I build/tools/gfx_tour.py --only rt --out
  C:/Users/torso/Documents/deco/build/tour/graphics`, then `rt_regress.py build/tour/mask_new build/tour/graphics`.
  Expected: PASS, or only `19_mission_prep` 0.54 %.
- [ ] **Step 3:** Docs:
  - `ARCHITECTURE.md` (texture cache + present path paragraph);
  - README third-party licences (xBRZ GPL-3.0, SMAA MIT, FXAA MIT);
  - `docs/ENV_FLAGS.md` (`PSPRECOMP_AA`).
- [ ] **Step 4:** User kit in `build/graficos/`:
  - `probar_graficos_{xbrz,fxaa,smaa,todo,nada}.bat` (1.1.4, copy of `save_wooyari`, port 9996, render scale 2,
    no HD pack);
  - `QUE_PROBAR.md` (Spanish): what to look at, and a note to try render scale 1 as a weak PC would;
  - the comparison images, sent with SendUserFile.
- [ ] **Step 5:** CHANGELOG "Actualización 17 (en preparación)": both options, in plain Spanish. Memory + hand-off.
  Report to the user; packaging u17 and pushing wait for their word.
