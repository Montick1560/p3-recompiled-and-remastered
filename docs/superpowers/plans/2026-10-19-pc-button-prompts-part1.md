# PC button prompts, part 1 — the help bars — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The help-bar icons of Patapon 3 show the key / Xbox / DualShock 4 / DualSense button the player
uses (last input wins, launcher remap respected), in a Gris or Color style.

**Architecture:** A generic *texture patch* seam in `ge_texture_bind` lets a registered patcher recognise a
texture by content, mix a *variant* (device + style) into its cache hash and redraw rectangles of the decoded
(or HD-pack) image on a cache miss. The generic button-prompt patcher (runtime) owns device tracking, icon
choice from the `ControlMap`, the embedded Kenney icons and the cell drawing; Patapon 3's module only
registers its atlas: size/format, a recognition region + hash, and the cell table.

**Tech Stack:** C++17 runtime (SDL2, GL 3.3), stb_image (already used by `psp_texrep_png`), CMake/Ninja,
the test-exe-per-file pattern of `runtime/tests/`.

**Spec:** `docs/superpowers/specs/2026-10-19-pc-button-prompts-part1-design.md`

## Global Constraints

- No game data in `runtime/src` (CLAUDE.md §3): cell rectangles, the atlas hash and its format live in
  `games/patapon3/runtime/p3_prompts.cpp`.
- GL calls only on the render/GE thread (the texture bind already runs there).
- Icons: Kenney Input Prompts 1.5, CC0; vendor only the icons used, plus Kenney's `License.txt`.
- Devices: keyboard; Xbox style (Xbox, Steam Deck, any pad SDL does not identify); DualShock 4; DualSense.
- Launcher option `Iconos de botones`: `Gris` (default) / `Color`, env `PSPRECOMP_BUTTON_ICONS=gray|color`.
- An action with several keys shows the **first** one.
- Every icon fits its original cell; the bar layout never changes.
- Never show an icon that is not the button that performs the action: when no faithful icon exists, the
  cell keeps the game's own PSP glyph.
- No steady-state frame-time cost: work happens only on a texture-cache miss.
- Commits on `patapon3-windows`, author "Patapon Community", no co-author line; nothing is pushed.

## Recon facts the tasks rely on (dumped live 2026-10-19, DxD 1.1.4, Spanish, hideout, port 9998)

- Atlas `0x092E5300`, 256×128, CLUT4 (`tf=4`), swizzled, CLUT `0x092E9340`: index 0 = `FF171717` (dark,
  opaque), 1 = `FF575757`, 2..13 = gray `7F7F7F` with an alpha ramp, 14/15 = red `FF4C4D`. The bar draws
  with texfunc modulate, vertex colour white: **the bar gray is 0x7F**, glyph interiors 0x17.
- Cells (texels, inclusive x ranges; glyph row y 117..127): □ 19..27, ○ 30..38, ✕ 41..49, △ 52..60,
  D-pad all-red 63..71, D-pad up/down-red 74..82, D-pad up-red 85..93, `L/R` 96..111, analog stick with
  up/down arrows 115..123 (spec §3 called it ✚; it is the stick). Pills START x 1..37, SELECT x 41..78,
  y 100..113. `:` `/` `&` (x 0..16) stay.
- **The game rotates cells through UVs**: the hideout bar's "L/R/✚" draws the up/down D-pad cell rotated
  90° (two vertices with the same x have different u). So a cell's drawing must stay correct when rotated
  90°: D-pad and stick shapes are symmetric; keyboard arrows are drawn as one keycap with arrow marks
  (rotates to the left/right version), never Kenney's inverted-T.

## File Structure

| File | Responsibility |
|---|---|
| `runtime/include/psp_controls.h`, `src/psp_controls.cpp` | + reverse lookup: first key name / pad button name of an action |
| `runtime/include/psp_input_device.h`, `src/psp_input_device.cpp` | device enum, names, SDL controller type → family, the global last-input device + env force |
| `runtime/src/psp_event_loop.cpp` | feeds key / pad / axis events to the device tracker; exposes the `ControlMap` |
| `runtime/src/psp_debug_socket.cpp`, `include/psp_debug_socket.h`, `src/main.cpp` | `P <keyboard\|xbox\|ps4\|ps5>` forces the device (tours) |
| `runtime/include/psp_prompts.h`, `src/psp_prompts.cpp` | pure icon choice: (glyph, device, style, ControlMap) → icon names |
| `runtime/include/psp_prompt_raster.h`, `src/psp_prompt_raster.cpp` | pure RGBA helpers: area resample, hole fill, neutral tint, keycap-arrows drawing, cell drawing |
| `runtime/assets/button_prompts/*.png`, `License.txt` | vendored Kenney icons |
| `runtime/cmake/embed_files.cmake` | build-time PNG → C++ byte arrays (`prompt_icons_data.cpp` in the build dir) |
| `runtime/include/psp_ge_texpatch.h`, `src/psp_ge_texpatch.cpp` | generic patch seam: registry, per-list latch, recognition memo, compose dispatch |
| `runtime/src/psp_ge_texture.cpp`, `src/psp_ge_draw.cpp` | call the seam (bind / list start) |
| `runtime/include/psp_prompt_atlas.h`, `src/psp_prompt_atlas.cpp` | the button-prompt patcher: atlas spec type, recognition, compose at scale |
| `games/patapon3/runtime/p3_prompts.cpp` (+ `.h`) | Patapon 3's atlas spec, registered from `patapon3_register_hooks` |
| `games/patapon3/game_options.json` | option `button_icons` |
| `runtime/tests/test_input_device.cpp`, `test_prompts.cpp`, `test_prompt_raster.cpp`, `test_ge_texpatch.cpp`, `test_prompt_atlas.cpp` | unit tests |

## Review Focus

- A pad connected at start but the player uses the keyboard (and vice versa): the first real input wins; a
  connect event alone only sets the start-up device before any input. Test in Task 2.
- A remap that leaves an action without a key or without a pad button: that device keeps the PSP glyph for
  it, never another button's icon. Test in Task 3.
- The HD pack replacing the atlas, and its PNG still decoding (Pending): the composed texture is built on the
  pack image when ready and on the original meanwhile, and is never shared with other slots under the
  pack's path. Test in Task 5 (fake patcher + shared/pending flags) and live in Task 7.
- A different language / release whose glyph row differs: recognition fails and the atlas is drawn untouched
  (no garbage). Test in Task 6.
- A device switch in the middle of a display list: the variant is latched at list start, so every bar PRIM
  of a frame uses one device. Test in Task 5.

---

### Task 1: ControlMap reverse lookup

**Files:**
- Modify: `runtime/include/psp_controls.h`, `runtime/src/psp_controls.cpp`
- Test: `runtime/tests/test_controls.cpp`

**Interfaces:**
- Produces: `std::string ControlMap::key_name(uint32_t mask) const` — the first key the action with that
  PSP mask kept (SDL key name exactly as configured, e.g. `"X"`, `"Return"`), `""` when none;
  `std::string ControlMap::pad_name(uint32_t mask) const` — its pad button name (`"a"`, `"leftshoulder"`…),
  `""` when none.

- [ ] **Step 1: Failing tests** `reverse_lookup_defaults` (`key_name(0x4000)=="X"`, `key_name(0x0008)=="Space"`,
  `key_name(0x0001)=="V"`, `pad_name(0x4000)=="a"`, `pad_name(0x0100)=="leftshoulder"`) and
  `reverse_lookup_remap` (spec `"cross=K|y;circle=|"` → `key_name(0x4000)=="K"`, `pad_name(0x4000)=="y"`,
  `key_name(0x2000)==""`, `pad_name(0x2000)==""`; a key dropped as a duplicate is not returned).
- [ ] **Step 2:** build `test_controls`, run → FAIL (no member).
- [ ] **Step 3:** store per action the kept key names and the kept pad name in `build()` (beside `desc_`).
- [ ] **Step 4:** `ninja test_controls && ./test_controls` → PASS.
- [ ] **Step 5:** commit `feat(runtime): ControlMap reverse lookup - first key and pad button of an action`.

### Task 2: Input device tracking

**Files:**
- Create: `runtime/include/psp_input_device.h`, `runtime/src/psp_input_device.cpp`, `runtime/tests/test_input_device.cpp`
- Modify: `runtime/src/psp_event_loop.cpp`, `runtime/include/psp_debug_socket.h`, `runtime/src/psp_debug_socket.cpp`, `runtime/src/main.cpp`, `runtime/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class InputDevice : uint8_t { Keyboard, Xbox, DualShock4, DualSense };`
  - `const char* input_device_name(InputDevice)` → `"keyboard" | "xbox" | "ps4" | "ps5"`;
    `bool input_device_parse(const char* s, InputDevice* out)` (those names, case-insensitive).
  - `InputDevice input_device_from_controller_type(int sdl_type)` — SDL_GameControllerType ints:
    PS4 (4) and PS3 (3) → DualShock4, PS5 (7) → DualSense, anything else → Xbox.
  - `class InputDeviceTracker { void on_key(); void on_pad(InputDevice); void on_connect(InputDevice);
    InputDevice current() const; }` — pure: `on_connect` only changes the device while no key/pad input has
    happened yet; `on_key`/`on_pad` always win.
  - Globals: `InputDevice psp_input_device()` (relaxed atomic read, any thread);
    `void psp_input_device_init()` reads `PSPRECOMP_INPUT_DEVICE` (a valid name forces it and freezes it);
    `void psp_input_device_note_key()`, `void psp_input_device_note_pad(InputDevice)`,
    `void psp_input_device_note_connect(InputDevice)`, `void psp_input_device_force(InputDevice)` (socket).
  - `const ControlMap& psp_control_map()` in `psp_event_loop.cpp` (built before the game starts; read-only after).
- [ ] **Step 1: Failing tests** in `test_input_device.cpp`: names round-trip; `parse("PS5")`; controller types
  `0,1,2,3,4,5,6,7` → `Xbox,Xbox,Xbox,DS4,DS4,Xbox,Xbox,DualSense`; tracker: start Keyboard,
  `on_connect(DualSense)` → DualSense, `on_key()` → Keyboard, `on_connect(Xbox)` → still Keyboard,
  `on_pad(DualShock4)` → DualShock4.
- [ ] **Step 2:** register `test_input_device` in CMake (pattern of `test_controls`), run → FAIL.
- [ ] **Step 3:** implement; event loop: `SDL_KEYDOWN` (not repeat) → note_key; controller button down,
  trigger past `pad_trigger_pressed`, or left/right stick past `kStickDeadzone` → note_pad(family of
  `SDL_GameControllerGetType` of that controller); `SDL_CONTROLLERDEVICEADDED` → note_connect. Socket
  `P <name>` → `hooks.input_device`; reply `OK 0`, bad name `ERR usage: P keyboard|xbox|ps4|ps5`.
  One `[PROMPTS] input device: <name>` log line per change.
- [ ] **Step 4:** `ninja && ctest -R "input_device|controls|debug_socket"` → PASS.
- [ ] **Step 5:** commit `feat(runtime): track the last input device (keyboard / Xbox / DualShock 4 / DualSense)`.

### Task 3: Icon choice

**Files:**
- Create: `runtime/include/psp_prompts.h`, `runtime/src/psp_prompts.cpp`, `runtime/tests/test_prompts.cpp`
- Modify: `runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `ControlMap::key_name/pad_name` (Task 1), `InputDevice` (Task 2).
- Produces:
  - `enum class PromptGlyph : uint8_t { Cross, Circle, Square, Triangle, LR, Start, Select, DpadAll,
    DpadVertical, DpadUp, StickVertical };`
  - `enum class PromptStyle : uint8_t { Gray, Color };` `PromptStyle prompt_style_from_env(const char*)`
    (`"color"` → Color, anything else → Gray).
  - `std::vector<std::string> prompt_icons(PromptGlyph, InputDevice, PromptStyle, const ControlMap&)` —
    icon names (asset basenames without `.png`, or the synthetic names `@arrows_all`, `@arrows_vertical`,
    `@arrows_up`); **empty = keep the game's glyph**. `LR` returns two names (L then R) or empty if either
    is missing.
  - `std::string prompt_key_icon(const std::string& sdl_key_name, bool wide)` — Kenney keyboard icon for an
    SDL key name; `wide` (pills) prefers word keys (`keyboard_space`, `keyboard_tab`, `keyboard_enter`,
    `keyboard_shift`…), narrow cells use symbols (`keyboard_space_icon`, `keyboard_tab_icon`,
    `keyboard_return`, `keyboard_backspace_icon`, `keyboard_shift_icon`, `keyboard_arrow_*`);
    letters/digits/F-keys → `keyboard_<lower>`; `Left `/`Right ` prefixes dropped; unknown → `""`.
- Pad name → icon, per family (Xbox / PS, PS-only names differ for DS4 vs DualSense):
  `a,b,x,y` → `xbox_button_{a,b,x,y}` / `playstation_button_{cross,circle,square,triangle}` (Color:
  `xbox_button_color_*`, `playstation_button_color_*`); `leftshoulder,rightshoulder` → `xbox_lb,xbox_rb` /
  `playstation_trigger_l1,_r1`; `start` → `xbox_button_menu` / `playstation4_button_options` /
  `playstation5_button_options`; `back` → `xbox_button_view` / `playstation4_button_share` /
  `playstation5_button_create`; `leftstick,rightstick` → `xbox_ls,xbox_rs` / `playstation_button_l3,_r3`;
  `dpup..dpright` → `xbox_dpad_up..` / `playstation_dpad_up..`; anything else → `""`.
- D-pad glyphs: pad → `<family>_dpad_all` / `_dpad_vertical` / `_dpad_up` only when up/down/left/right are
  mapped to `dpup/dpdown/dpleft/dpright`; keyboard → `@arrows_*` only when they are `Up/Down/Left/Right`;
  otherwise empty. `StickVertical`: pad → `xbox_stick_l_vertical` / `playstation_stick_l_vertical`;
  keyboard → empty (the analog stick has no key).
- [ ] **Step 1: Failing tests** (defaults built with the test's key/pad tables, as in `test_controls`):
  keyboard defaults ✕→`keyboard_x` (first of `X, Return`), ○→`keyboard_z`, LR→`{keyboard_q,keyboard_w}`,
  Start→`keyboard_space`, Select→`keyboard_v`, DpadVertical→`@arrows_vertical`, StickVertical→empty;
  Xbox Gray ✕→`xbox_button_a`, Color ✕→`xbox_button_color_a`, Start→`xbox_button_menu`;
  DS4 Select→`playstation4_button_share`, DualSense Select→`playstation5_button_create`;
  remap `cross=Return|b` → keyboard ✕ `keyboard_return`, Xbox ✕ `xbox_button_b`, DualSense ✕
  `playstation_button_circle`; remap `circle=|` → pad ○ empty, keyboard ○ empty;
  remap `up=W|dpup` → keyboard DpadAll empty, pad DpadAll `xbox_dpad_all`;
  `prompt_key_icon("Return",false)=="keyboard_return"`, `("Space",true)=="keyboard_space"`,
  `("Space",false)=="keyboard_space_icon"`, `("Left Shift",false)=="keyboard_shift_icon"`,
  `("Keypad 7",false)==""`.
- [ ] **Step 2:** register `test_prompts` (sources: `psp_prompts.cpp psp_controls.cpp psp_pad.cpp
  psp_input_device.cpp`), run → FAIL.
- [ ] **Step 3:** implement as tabulated above.
- [ ] **Step 4:** `ctest -R prompts` → PASS.
- [ ] **Step 5:** commit `feat(runtime): button prompt icon choice per device, style and control map`.

### Task 4: Icon assets and raster helpers

**Files:**
- Create: `runtime/assets/button_prompts/` (every icon name Task 3 can return + `License.txt`),
  `runtime/cmake/embed_files.cmake`, `runtime/include/psp_prompt_raster.h`, `runtime/src/psp_prompt_raster.cpp`,
  `runtime/tests/test_prompt_raster.cpp`
- Modify: `runtime/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct RgbaImage { int w = 0, h = 0; std::vector<uint8_t> px; };` (RGBA8, top row first)
  - `const RgbaImage* prompt_icon_image(const std::string& name)` — embedded PNG decoded once (mutex-guarded
    cache, any thread); `@arrows_*` drawn at 64×64; `nullptr` for unknown names.
  - `RgbaImage rgba_resample_area(const RgbaImage& src, int w, int h)` — box filter on premultiplied alpha.
  - `void rgba_fill_holes(RgbaImage& img, uint8_t r, uint8_t g, uint8_t b)` — transparent pixels not
    4-connected to the image border (a < 32 counts as transparent) become opaque (r,g,b).
  - `void rgba_tint_neutral(RgbaImage& img, uint8_t gray)` — pixels with max(r,g,b)−min(r,g,b) < 24 get
    rgb scaled by gray/255; coloured pixels (Kenney's red D-pad highlight, Color buttons) are kept.
  - `void rgba_draw_cell(RgbaImage& dst, int x, int y, int w, int h, const std::vector<const RgbaImage*>& icons)`
    — clears the rectangle to transparent, then fits the icons side by side (two icons: left, a 1-px-wide
    gray `/` gap of w/5, right), each scaled to fit by its alpha bounding box, centred, aspect kept.
  - Embedding: `embed_files.cmake` turns every `assets/button_prompts/*.png` into
    `${CMAKE_BINARY_DIR}/prompt_icons_data.cpp` with `struct EmbeddedFile { const char* name; const unsigned
    char* data; unsigned size; }; extern const EmbeddedFile g_prompt_icons[]; extern const unsigned
    g_prompt_icons_count;` (declared in `psp_prompt_raster.cpp`), regenerated when a PNG changes.
- Style values: Gray = tint 0x7F, holes 0x17 (the atlas's own gray and glyph interior); Color = no tint on
  coloured pixels, neutral pixels tinted 0x7F as in Gray, holes 0x17.
- [ ] **Step 1: Failing tests** (synthetic images only): a 64×64 opaque square resampled to 8×8 stays
  opaque with the same colour; a white ring with a transparent centre → `rgba_fill_holes` fills only the
  centre; `rgba_tint_neutral(…, 0x7F)` turns white into 0x7F and leaves (255,76,77) alone; `rgba_draw_cell`
  never writes outside its rectangle (a canary image filled with 0xAB stays 0xAB around the cell) and
  clears the inside; two icons land left and right of the centre; `prompt_icon_image("xbox_button_a")` is
  64×64 and `prompt_icon_image("@arrows_vertical")` non-null; `prompt_icon_image("nope")==nullptr`; every
  name `prompt_icons` can return for every glyph × device × style with the default map resolves non-null.
- [ ] **Step 2:** copy the icons (from the Kenney zip `Default/` folders) + `License.txt`; write the embed
  script; register `test_prompt_raster`; run → FAIL.
- [ ] **Step 3:** implement (decode with `ge_texrep_decode_png`).
- [ ] **Step 4:** `ctest -R prompt_raster` → PASS.
- [ ] **Step 5:** commit `feat(runtime): embedded Kenney prompt icons and RGBA cell drawing`.

### Task 5: Texture patch seam in the GE texture path

**Files:**
- Create: `runtime/include/psp_ge_texpatch.h`, `runtime/src/psp_ge_texpatch.cpp`, `runtime/tests/test_ge_texpatch.cpp`
- Modify: `runtime/src/psp_ge_texture.cpp`, `runtime/src/psp_ge_draw.cpp` (`ge_draw_begin_list`), `runtime/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  struct GeTexPatcher {
      // Cheap pre-filter on the decode parameters (size, format); false = never this texture.
      bool (*candidate)(const GeTexDecodeParams& p);
      // Decoded original (RGBA8 at p.width x p.height): is this the texture? Memoised per content hash.
      bool (*recognise)(const GeTexDecodeParams& p, const uint8_t* rgba);
      // Variant latched at list start (0 = do not patch this list).
      uint32_t (*variant)();
      // Redraw `img` (the original, or the pack image at img.w / p.width scale) for `variant`.
      void (*apply)(uint32_t variant, const GeTexDecodeParams& p, RgbaImage& img);
  };
  void ge_texpatch_register(const GeTexPatcher& p);          // at most 4; game module, before boot
  void ge_texpatch_begin_list();                             // GE thread, list start: latch variants
  // GE thread, every bind after the content hash: the latched variant of the patcher that recognises the
  // texture, 0 if none. Decodes the original once per new content hash (memo, 512 entries, cleared when full).
  uint32_t ge_texpatch_variant(const uint8_t* rdram, const GeTexDecodeParams& p, uint32_t content_hash);
  void ge_texpatch_apply(uint32_t variant, const GeTexDecodeParams& p, RgbaImage& img);
  int ge_texpatch_scale();                                   // compose scale without a pack: render scale clamped to 1..4
  ```
  The patcher index travels in the variant's top 4 bits (`(index + 1) << 28 | patcher_variant`).
- Bind integration (`ge_texture_bind`): after the content hash, `v = ge_texpatch_variant(...)`; when
  `v != 0`, `content_hash ^= v * 0x27D4EB2Fu`, skip `try_share`, and on the miss / pending-retry paths
  build the image to upload — the pack image copied into an `RgbaImage` when Ready, else the decoded
  original upscaled ×`ge_texpatch_scale()` (bilinear when the sampler's mag filter is linear, else nearest)
  — then `ge_texpatch_apply(v, params, img)` and `glTexImage2D` it. A patched slot never calls
  `g_rep_share.adopt` and keeps `rep_path` empty; it still calls `ge_texrep_image_uploaded`.
  `ge_draw_begin_list()` calls `ge_texpatch_begin_list()`.
- [ ] **Step 1: Failing tests** with a fake patcher (counts calls): `variant` is read once per
  `begin_list` (switching the fake's device mid-list keeps the latched value until the next
  `begin_list`); `recognise` runs once per content hash (two binds of the same hash = one decode);
  a non-candidate texture returns 0 without decoding; a recognised texture with latched variant 0 returns 0;
  two patchers get distinct top bits.
- [ ] **Step 2:** register `test_ge_texpatch` (sources: `psp_ge_texpatch.cpp psp_ge_texdecode.cpp`),
  run → FAIL.
- [ ] **Step 3:** implement the seam and the bind integration.
- [ ] **Step 4:** `ninja && ctest` → all PASS (texture-cache tests unchanged).
- [ ] **Step 5:** commit `feat(runtime): texture patch seam - registered patchers redraw recognised textures on a cache miss`.

### Task 6: The button-prompt atlas patcher

**Files:**
- Create: `runtime/include/psp_prompt_atlas.h`, `runtime/src/psp_prompt_atlas.cpp`, `runtime/tests/test_prompt_atlas.cpp`
- Modify: `runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 2–5.
- Produces:
  ```cpp
  struct PromptCell { PromptGlyph glyph; int x, y, w, h; };      // texels of the original atlas
  struct PromptAtlasSpec {
      int width, height, format;                 // GE texture size and GE_TFMT_*
      int sig_x, sig_y, sig_w, sig_h;            // recognition region
      std::vector<uint64_t> sig_hashes;          // accepted prompt_atlas_signature() values
      std::vector<PromptCell> cells;
  };
  // XXH3 of the region's alpha mask (a >= 0x40 -> 1), row by row.
  uint64_t prompt_atlas_signature(const uint8_t* rgba, int w, int x, int y, int rw, int rh);
  void psp_prompts_register_atlas(PromptAtlasSpec spec);  // registers a GeTexPatcher; reads PSPRECOMP_BUTTON_ICONS
  // Pure compose used by the patcher's apply: draws every cell whose prompt_icons() is non-empty,
  // scaled by img.w / spec.width; cells with no icon keep their pixels.
  void prompt_atlas_compose(const PromptAtlasSpec& spec, InputDevice d, PromptStyle s,
                            const ControlMap& map, RgbaImage& img);
  ```
  Variant = `1 + device * 2 + style` (never 0); recognition = `prompt_atlas_signature` of the spec region in
  the decoded original ∈ `sig_hashes`. When `PSPRECOMP_BUTTON_ICONS=off` the patcher is not registered
  (kill switch, not in the launcher). Logs `[PROMPTS] atlas recognised (WxH) at 0x…` once per address.
- [ ] **Step 1: Failing tests**: a synthetic 256×128 atlas whose glyph region carries a known pattern →
  signature stable, a one-pixel change in the region changes it, a change outside does not;
  `prompt_atlas_compose` at scale 1 and on a 1024×512 image (scale 4): pixels outside every cell are
  byte-identical to the input, an Xbox ✕ cell becomes non-transparent with green pixels in Color and only
  neutral pixels in Gray; a cell whose glyph has no icon (keyboard StickVertical) is byte-identical.
- [ ] **Step 2:** register `test_prompt_atlas`, run → FAIL.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** `ctest -R prompt_atlas` → PASS.
- [ ] **Step 5:** commit `feat(runtime): button-prompt atlas patcher - device / style variants of a recognised atlas`.

### Task 7: Patapon 3 atlas, launcher option, live verification

**Files:**
- Create: `games/patapon3/runtime/p3_prompts.cpp`, `games/patapon3/runtime/p3_prompts.h`
- Modify: `games/patapon3/runtime/hooks_main.cpp` (`patapon3_register_hooks`), `games/patapon3/game_options.json`,
  `ARCHITECTURE.md` (the texture patch seam and the prompt patcher), `README.md` (feature + Kenney credit)

**Interfaces:**
- Consumes: `psp_prompts_register_atlas` (Task 6).
- Produces: `void p3_prompts_register()` — the spec: 256×128, `GE_TFMT_CLUT4`, region x 0..127 y 99..127,
  the cells from "Recon facts" (✕ `Cross` 41,117,9,11; ○ `Circle` 30; □ `Square` 19; △ `Triangle` 52;
  `DpadAll` 63; `DpadVertical` 74; `DpadUp` 85; `LR` 96,117,16,11; `StickVertical` 115,117,9,11;
  `Start` 1,100,37,14; `Select` 41,100,38,14), `sig_hashes` = the value(s) measured live.
- game option: `{"section":"game","key":"button_icons","type":"enum","values":["gray","color"],"default":"gray",
  "tab":"general","env":"PSPRECOMP_BUTTON_ICONS","label":{"es":"Iconos de botones","en":"Button icons"},
  "description":{"es":"Los iconos de las barras de ayuda muestran tu teclado o mando.","en":"Help bar icons show your keyboard or controller."},
  "display":{"gray":{"es":"Gris","en":"Gray"},"color":{"es":"Color","en":"Color"}}}`
- [ ] **Step 1:** measure the signature live: `PSPRECOMP_PROMPTS_SIG_LOG=1` makes the patcher log
  `[PROMPTS] candidate 0x… sig=<hex>` for every candidate it rejects; boot to the hideout on 9998, read the
  hash, put it in the spec. Repeat with the English language and with DxD 1.1.5b / vanilla EU when their
  disc0 is on the PC; add each distinct hash.
- [ ] **Step 2:** `ninja && ctest` (settings test validates the new option) → PASS.
- [ ] **Step 3: Live (9998)**: hideout screenshots for `P keyboard`, `P xbox`, `P ps4`, `P ps5` × Gris /
  Color, scale 4, with and without the HD pack; a remap (`PSPRECOMP_CONTROLS="cross=Return|b"`) shows ↵ /
  B; toggle `P xbox`/`P keyboard` every list for 10 s with `D` logging → no frame with mixed devices.
- [ ] **Step 4:** gates: `rt_regress.py` (runs with the default device keyboard: the bar changes, so the
  hideout points are expected to move only inside the bar — inspect, and run the base comparison with
  `PSPRECOMP_BUTTON_ICONS=off` to prove everything else is identical: PASS); `perf_hideout.py` before/after
  (no ge ms change).
- [ ] **Step 5:** commit `feat(p3): help-bar button prompts - keyboard / Xbox / PlayStation icons, Gris / Color`;
  README credits Kenney (CC0); memory + handoff note for the user's play check. Do not push.

## Self-review notes

- Spec §2 goals → Tasks 2 (device), 1+3 (mapping), 6+7 (style option), 7 (languages/releases/pack).
- Spec §4.5 table → Task 3; D-pad rotation found in recon → `@arrows_*` keycaps (Task 4).
- Deviations from the spec, decided here: pills show Kenney's English word keys (`SPACE`, `TAB`) instead of
  a translated word (no text renderer needed, same as PC keyboards); PS3 pads count as DualShock 4; WASD-style
  D-pad remaps keep the PSP glyph (no faithful icon fits a 9×11 cell); the launcher credits screen does not
  exist yet (part 6 of the launcher), so Kenney is credited in the README and the vendored License.txt.
