# Launcher selectors, DualSense in the launcher, borderless window — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (native, the user's choice for this project) or superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every ‹ › selector in the launcher wraps around from either end with ← → on the keyboard or the pad. The launcher reads the PS5 DualSense again, as the game does. The launcher and the game get a third display mode, "Sin bordes" (a borderless window over the whole monitor), chosen in one "Modo de pantalla" option.

**Architecture:** (1) The launcher's step rule (`stepValue` in Options, the Themes carousel) wraps instead of clamping. (2) On Windows the launcher reads pads through the SDL2.dll the game ships, loaded at run time (SDL's GameController API sees XInput pads, DualSense and DualShock 4 over USB and Bluetooth). gilrs (XInput) stays as the fallback when no SDL2.dll is found, and on other systems. SDL is shut down while the game runs, so the game owns the pad. (3) A new settings key `display.window_mode` (`windowed|borderless|fullscreen`) replaces the `display.fullscreen` switch in the UI. When the key is absent, `display.fullscreen` decides as before. The game applies the mode with SDL (borderless = window without border, sized to the bounds of its display, never minimised on focus loss). The launcher applies it with Tauri.

**Tech Stack:** Launcher: Tauri 2 (Rust, `src-tauri/`), React + Vitest (`src/`), `cargo test`, `npm test`. Game: C++17 runtime (`psprecomp/runtime`), SDL2, ninja + ctest in `build/rtw`, `build/v115b/rtw`, `build/vanilla/rtw`.

**Spec:** The user's report of 25 Oct (es):
- "Las flechas de izquierda y derecha del launcher no funcionan como carrusel ... no se reinicia la selección [resolución interna, etc.]; algunos usuarios no pueden regresar porque no detecta la izquierda (mando o teclado); quiero que todas las opciones de este estilo funcionen correctamente."
- "El mando de PS5 ya no lo reconoce el launcher, pero el port del juego sí."
- "Hito: agregar modo ventana sin bordes para el launcher y el port". Chosen form (AskUserQuestion): one option "Modo de pantalla: Ventana / Sin bordes / Pantalla completa" that replaces the current ON/OFF and applies to the launcher and the game.

## Context (recon of 25 Oct — do not redo)

- Keyboard ← → already step the main menu version picker (wraps: `App.tsx` `stepVersion`), the Options values and the OSK skin (wraps: `ControlsEditor.stepSkin`). Checked live on `build/mi_build` with real scan codes (`build/plata/lnav/keys.py PID KEYS`). The defect is the clamp: `src/screens/Options.tsx:26` `stepValue` stops at the first and last value of an enum or int. A user at the first value presses ← and nothing happens ("no detecta la izquierda"). `src/screens/Themes.tsx:47-48` clamps the carousel in the same way.
- PS5 regression: launcher commit `8c41cb7` (23 Oct) switched gilrs to its XInput backend (`src-tauri/Cargo.toml:23`, `features = ["xinput"]`) to fix an XInput clone pad that WGI ignored. XInput never sees a DualSense, so the launcher lost it. The game uses SDL2 GameController (`psprecomp/runtime/src/psp_event_loop.cpp`) and sees both.
- Launcher pad code: `src-tauri/src/gamepad.rs`. It emits `nav` {action, device} and `pad_captured` {name: SDL button name} and holds `StickRepeat`. Its `game_running` drain is driven by `LaunchGuard`.
- The portable folder: `launcher.exe` at the root, `game/<release-id>/` holds `psprecomp_runtime.exe` + `SDL2.dll` (`scripts/make-portable.mjs`). `libloading 0.7.4` is already in `src-tauri/Cargo.lock`.
- Display today: the game's `display.fullscreen` → `PSPRECOMP_FULLSCREEN=1` → `SDL_WINDOW_FULLSCREEN_DESKTOP` (`psp_event_loop.cpp:464`), and F11 / Alt+Enter toggle it (`toggle_fullscreen`, line 64). The launcher's `lib.rs:475` calls `set_fullscreen(true)` when `display.fullscreen` is true. Option catalog: launcher `src-tauri/core_options.json` (+ `options.rs` normalize/effective, fixtures `src-tauri/tests/fixtures/*.ini`). The game catalog is `runtime/src/psp_settings.cpp` `kCore`. The contract is `psprecomp/docs/contracts/settings-v1.md` (+ `fixtures/`).

## Global Constraints

- Never push, never `gh`. Commits are local only: launcher repo `DECO/launcher` branch `main`, psprecomp branch `build-2026-10-25`. Author `Patapon Community <rexgamer1560@gmail.com>` via `git -c user.name=... -c user.email=...`, no `Co-Authored-By`. Each message ends with a `Verified:` line.
- Test game instances only on ports 9996–9998, never 9999. Kill only PIDs you launched. Never write the user's `savedata/` or `build/mi_build/.../settings.ini` from a test (copy the folder under `build/plata/` to test).
- Launcher gates: `cd src-tauri && cargo test` and `npm test` green. Game gates: `ninja` + `ctest` green in the three trees.
- The launcher package is built last with `P3_NO_BETA_GATE=1 npm run tauri build -- --no-bundle`; `cargo test` afterwards rebuilds the exe without it.
- UI copy in Spanish and English through `core_options.json` `label`/`display` (es first). Code and commits in English.
- Old `settings.ini` files (only `display.fullscreen`) keep working in both programs. Saving from the launcher writes `display.window_mode` and also `display.fullscreen = (mode != "windowed")`, for older game exes.

## Review Focus

1. DualSense over Bluetooth and over USB, and the XInput clone pad that `8c41cb7` fixed (2345:E062): all three must navigate the launcher. Only the user can test this (Task 5 checklist). The code side is pinned by the SDL-type → device and button → action tests in Task 2.
2. The pad while the game starts and returns: SDL is closed while the game runs, so the game alone owns the DualSense (no lightbar or report-mode fight). On return the launcher reopens it. Task 2 test `suspend_resume_state`. User check in Task 5.
3. Borderless on the monitor the window is on (second monitor, 125–150 % scaling): it covers that display exactly, with no gap and no title bar. Task 3 test `borderless_rect_is_display_bounds` (pure rect rule). User check in Task 5.
4. An old `settings.ini` with `fullscreen = false` (or no display section at all), and one with a bad `window_mode`: both programs pick windowed / fullscreen / the default exactly as the derived rule says. Task 3 and Task 4 tests.
5. A hand-edited value outside the list or range (e.g. `render_scale = 9`, `present_filter = foo`): ← and → still move to a valid value. Task 1 test `step_unknown_value`.

---

### Task 1: Selectors wrap around (Options values, Themes carousel)

**Files:**
- Modify: `launcher/src/screens/Options.tsx:26-38` (`stepValue`, exported for the test)
- Modify: `launcher/src/screens/Themes.tsx:47-48`
- Test: `launcher/src/screens/Options.test.tsx`, `launcher/src/screens/Themes.test.tsx`

**Interfaces:**
- Produces: `export function stepValue(d: OptionDef, v: string, dir: 1 | -1): string` (module export; behaviour below).

- [ ] **Step 1: Write the failing tests** in `Options.test.tsx` (pure `stepValue`, with defs built inline):
  - enum `["linear","nearest"]`: `stepValue(def,"nearest",1) === "linear"`, `stepValue(def,"linear",-1) === "nearest"`.
  - int min 1 max 8: `stepValue(def,"8",1) === "1"`, `stepValue(def,"1",-1) === "8"`, `stepValue(def,"4",1) === "5"`.
  - `step_unknown_value`: enum value `"foo"` → `dir 1` gives `values[0]`, `dir -1` gives the last value. Int `"9"` (above max 8) → `dir 1` gives `"1"`, `dir -1` gives `"8"`. Int `"abc"` → steps from the default.
  - bool still toggles.
  - Screen test: on Gráficos with `display.window_scale` = `"8"`, `{ArrowRight}` shows `1`.
  - `Themes.test.tsx`: on the last theme `{ArrowRight}` selects the first; on the first, `{ArrowLeft}` selects the last.
- [ ] **Step 2:** `npm test -- Options Themes` → the new cases FAIL (clamped values).
- [ ] **Step 3:** Implement the wrap. Enum: `(i + dir + n) % n`, where an unknown value counts as just before index 0 for `dir 1` and just after the last for `dir -1`. Int: step, and leave the range on one side to arrive at the other end (`max + 1 → min`, `min - 1 → max`). An out-of-range start clamps first, then wraps. Themes left/right: `(sel ± 1 + n) % n`.
- [ ] **Step 4:** `npm test` → all green.
- [ ] **Step 5:** Commit (launcher) `fix(launcher): left/right selectors wrap around at both ends` with `Verified:`.

### Task 2: Launcher pads through the game's SDL2 (DualSense back)

**Files:**
- Create: `launcher/src-tauri/src/sdl_pad.rs` (run-time SDL2 loader + polling)
- Modify: `launcher/src-tauri/src/gamepad.rs` (backend choice, pure SDL mapping), `launcher/src-tauri/src/lib.rs` (`mod sdl_pad`), `launcher/src-tauri/Cargo.toml` (`libloading = "0.7"`, Windows only is fine), `launcher/scripts/make-portable.mjs` (copy the first `--game` folder's `SDL2.dll` next to `launcher.exe`)
- Test: unit tests in `gamepad.rs` and `sdl_pad.rs`

**Interfaces:**
- Produces (in `gamepad.rs`, pure):
  - `pub fn sdl_nav(button: i32) -> Option<&'static str>`: SDL_GameControllerButton → nav action. A=0 confirm, B=1 back, Y=3 reset, LEFTSHOULDER=9 tab_prev, RIGHTSHOULDER=10 tab_next, DPAD_UP=11 up, DOWN=12 down, LEFT=13 left, RIGHT=14 right, others `None`.
  - `pub fn sdl_button_name(button: i32) -> Option<&'static str>`: SDL's own strings, `a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright` for 0..=14. These are what settings.ini `[controls]` stores, the same as `sdl_pad_name` today.
  - `pub fn sdl_device_kind(controller_type: i32) -> &'static str`: PS3=3, PS4=4 and PS5=7 → `"playstation"`, everything else → `"xbox"`.
  - `pub fn pressed_edges(prev: u32, now: u32) -> impl Iterator<Item = i32>`: bits set in `now` and not in `prev` (bit i = button i).
  - Trigger axes (SDL axis 4/5, 0..32767): crossing 16000 upward counts as `tab_prev` / `tab_next`, as gilrs' LeftTrigger2/RightTrigger2 did.
- Produces (in `sdl_pad.rs`):
  - `pub struct SdlPads` with `pub fn load(candidates: &[PathBuf]) -> Option<SdlPads>`. It tries each path to an `SDL2.dll`: the exe directory first, then every `game/*/SDL2.dll` under `settings::root_dir()`. It sets the hints `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` and `SDL_JOYSTICK_HIDAPI=1`, then calls `SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER = 0x2000)`.
  - `pub fn poll(&mut self) -> Vec<PadEvent>` with `pub enum PadEvent { Pressed { button: i32, device: &'static str }, Trigger { right: bool, device: &'static str }, Stick { x: f32, y: f32, device: &'static str } }`. It calls `SDL_GameControllerUpdate`, opens new controllers (`SDL_NumJoysticks` / `SDL_IsGameController` / `SDL_GameControllerOpen`, rescanned every 1 s), drops detached ones (`SDL_GameControllerGetAttached`), and reports button edges and the first pad's left stick. Stick y is reported +up, as gilrs does, so `StickRepeat` is unchanged.
  - `pub fn suspend(&mut self)` closes all controllers and calls `SDL_QuitSubSystem(0x2000)`. `pub fn resume(&mut self)` re-inits. Both are idempotent.
  - Logs one line per connect/disconnect on stderr: `[PAD] connected "<name>" type <n> (<device>)`.
- Consumes: `LaunchGuard::running()`, `PadCapture` (unchanged semantics).

- [ ] **Step 1: Write the failing tests:**
  - `sdl_nav_map`: `sdl_nav(0)=="confirm"`, `sdl_nav(1)=="back"`, `sdl_nav(13)=="left"`, `sdl_nav(14)=="right"`, `sdl_nav(9)=="tab_prev"`, `sdl_nav(5)==None`.
  - `sdl_names`: `sdl_button_name(0)=="a"`, `(4)=="back"`, `(11)=="dpup"`, `(14)=="dpright"`, `(15)==None`.
  - `sdl_types`: `sdl_device_kind(7)=="playstation"`, `(4)=="playstation"`, `(1)=="xbox"`, `(0)=="xbox"`.
  - `edges`: `pressed_edges(0b0001, 0b0101)` yields `[2]`; releases yield nothing.
  - `suspend_resume_state` (in `sdl_pad.rs`, on the state flag alone, no DLL): suspend twice then resume once leaves the state "active".
  - `load_missing_dll`: `SdlPads::load(&[PathBuf::from("Z:/nope/SDL2.dll")])` is `None`.
- [ ] **Step 2:** `cargo test` → FAIL (functions missing).
- [ ] **Step 3:** Implement `sdl_pad.rs` with `libloading` (one `Symbol` per SDL function named in Interfaces, `extern "C"` signatures from SDL2 2.x). Change `gamepad::spawn`: on Windows try `SdlPads::load` first, and if it returns `None` use the current gilrs loop unchanged; on other systems always use gilrs. While `LaunchGuard::running()` call `suspend()`; when it stops, call `resume()`. Route `Pressed` through `PadCapture` (emit `pad_captured` with `sdl_button_name`) or through `sdl_nav` → `nav`. Route `Trigger` → `nav` tab_prev/tab_next and `Stick` → `StickRepeat`. Keep gilrs `map_button`/`sdl_pad_name`/`device_kind` for the fallback.
- [ ] **Step 4:** `cargo test` → green. `npm test` still green.
- [ ] **Step 5: Live check on this PC (no pad needed):** run `launcher.exe` from a copy of the portable folder under `build/plata/`. The log must show the SDL backend chosen (`[PAD] SDL2 loaded from ...`) and the XInput pad if one is connected. Start a game and return: no crash, and the `[PAD]` suspend/resume lines are present.
- [ ] **Step 6:** Commit (launcher) `fix(launcher): pads through the game's SDL2 (DualSense and DualShock 4 again)` with `Verified:`.

### Task 3: Game — `display.window_mode` (windowed / borderless / fullscreen)

**Files:**
- Create: `psprecomp/runtime/include/psp_window_mode.h` (pure rules), `psprecomp/runtime/tests/test_window_mode.cpp` (+ `runtime/CMakeLists.txt` test entry)
- Modify: `psprecomp/runtime/src/psp_settings.cpp` (`kCore`: `{"display","window_mode","PSPRECOMP_WINDOW_MODE",Kind::Enum,"",0,0,"windowed,borderless,fullscreen"}`; an absent or invalid key sets no variable), `psprecomp/runtime/src/psp_event_loop.cpp` (apply at start + F11/Alt+Enter), `psprecomp/docs/contracts/settings-v1.md` (+ `fixtures/` if they list the display section), `psprecomp/docs/ENV_FLAGS.md` (`PSPRECOMP_WINDOW_MODE`)

**Interfaces:**
- Produces (`psp_window_mode.h`):
  - `enum class PspWindowMode { Windowed, Borderless, Fullscreen };`
  - `PspWindowMode psp_window_mode(const char* mode_env, const char* fullscreen_env)`: `"windowed"|"borderless"|"fullscreen"` (case-insensitive) win. Otherwise `fullscreen_env == "1"` → Fullscreen, else Windowed.
  - `struct PspRect { int x, y, w, h; }; PspRect psp_borderless_rect(PspRect display_bounds)`: returns the bounds unchanged. It is pinned by a test so a later "inset for the taskbar" change is a conscious one.
  - `PspWindowMode psp_window_mode_toggle(PspWindowMode current, PspWindowMode configured)`: Windowed → configured when that is not Windowed, else Fullscreen. Any other mode → Windowed.
- Behaviour in `psp_event_loop.cpp`:
  - Windowed: as today.
  - Borderless: `SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0")`, `SDL_SetWindowBordered(w, SDL_FALSE)`, then position and size = `SDL_GetDisplayBounds(SDL_GetWindowDisplayIndex(w))`. Leaving it restores the border and the `480*scale × 272*scale` centred size.
  - Fullscreen: `SDL_WINDOW_FULLSCREEN_DESKTOP` (unchanged).
  - Log `[RT] window mode: <name>`.

- [ ] **Step 1: Write the failing test** `test_window_mode.cpp`:
  - `psp_window_mode("borderless", nullptr) == Borderless`; `("BORDERLESS","0") == Borderless`; `(nullptr,"1") == Fullscreen`; `(nullptr,nullptr) == Windowed`; `("bogus","1") == Fullscreen`; `("windowed","1") == Windowed`.
  - `borderless_rect_is_display_bounds`: `psp_borderless_rect({1920,0,2560,1440})` equals the input.
  - Toggle: `(Windowed, Borderless) → Borderless`, `(Windowed, Windowed) → Fullscreen`, `(Borderless, Borderless) → Windowed`, `(Fullscreen, Borderless) → Windowed`.
  - Settings: extend the existing settings test (`runtime/tests/test_settings.cpp`) so that `window_mode = borderless` sets `PSPRECOMP_WINDOW_MODE=borderless`, an absent key sets nothing, and `window_mode = foo` logs the invalid line and sets nothing.
- [ ] **Step 2:** `ninja && ctest -R "window_mode|settings"` in `build/rtw` → FAIL.
- [ ] **Step 3:** Implement the header, the settings key and the event-loop application. Point `toggle_fullscreen` at `psp_window_mode_toggle`.
- [ ] **Step 4:** `ninja` + `ctest` green in the three trees.
- [ ] **Step 5: Live check:** a test instance on port 9998 with `PSPRECOMP_WINDOW_MODE=borderless` → a window capture (`build/tools/winshot.py`) has the display's size and no title bar. `=windowed` → 960×544 with a border. Unset with `PSPRECOMP_FULLSCREEN=1` → desktop fullscreen. F11 from borderless returns to windowed.
- [ ] **Step 6:** Commit (psprecomp) `feat(runtime): borderless window mode (display.window_mode)` with `Verified:`.

### Task 4: Launcher — "Modo de pantalla" option and its own window

**Files:**
- Modify: `launcher/src-tauri/core_options.json`: replace the `display.fullscreen` entry with `{section:"display", key:"window_mode", type:"enum", values:["windowed","borderless","fullscreen"], default:"fullscreen", tab:"graphics", label:{es:"Modo de pantalla",en:"Display mode"}, display:{windowed:{es:"Ventana",en:"Windowed"}, borderless:{es:"Sin bordes",en:"Borderless"}, fullscreen:{es:"Pantalla completa",en:"Fullscreen"}}}`. Keep `display.fullscreen` known to `options.rs` as a hidden legacy key, not shown in the UI.
- Modify: `launcher/src-tauri/src/options.rs` (effective `display.window_mode` when absent: `fullscreen == "true"` → `"fullscreen"`, else `"windowed"`; an invalid value → the same derivation), `launcher/src-tauri/src/settings.rs` (`save`: when `display.window_mode` changes, also write `display.fullscreen`), `launcher/src-tauri/src/lib.rs` (setup applies the mode; new `fn apply_window_mode(win: &WebviewWindow, mode: &str)` also called after `save_settings` when the mode changed)
- Test: `options.rs` / `settings.rs` unit tests, fixtures `src-tauri/tests/fixtures/*.ini`, `src/screens/Options.test.tsx`

**Interfaces:**
- Consumes: Task 1 `stepValue` (wraps over the three modes).
- Produces: `pub fn window_mode_of(values: &BTreeMap<String,String>) -> &'static str` in `options.rs` (`"windowed"|"borderless"|"fullscreen"`).
- Launcher window behaviour:
  - windowed: `set_fullscreen(false)`, `set_decorations(true)`, 1280×720 centred.
  - borderless: `set_fullscreen(false)`, `set_decorations(false)`, position and size = `current_monitor()` (fallback `primary_monitor()`) position/size in physical pixels.
  - fullscreen: `set_fullscreen(true)`.

- [ ] **Step 1: Write the failing tests:**
  - `window_mode_derived`: an ini with only `fullscreen = false` → `"windowed"`; `fullscreen = true` → `"fullscreen"`; no display section → `"fullscreen"` (the default); `window_mode = borderless` + `fullscreen = true` → `"borderless"`; `window_mode = foo` + `fullscreen = false` → `"windowed"`.
  - `save_writes_legacy_fullscreen`: saving `{display.window_mode: "borderless"}` writes `window_mode = borderless` and `fullscreen = true`; `"windowed"` writes `fullscreen = false`.
  - The `invalid.ini` / `custom.ini` expectations updated for the new key.
  - `Options.test.tsx`: the Gráficos tab shows "Modo de pantalla" with "Pantalla completa". `{ArrowRight}` → "Ventana" (wrap from the last value), `{ArrowRight}` → "Sin bordes".
- [ ] **Step 2:** `cargo test` and `npm test` → FAIL.
- [ ] **Step 3:** Implement as in Files/Interfaces.
- [ ] **Step 4:** `cargo test` and `npm test` → green.
- [ ] **Step 5: Live check:** with a copy of the portable folder, switch the three modes from Options. The launcher window changes at once: title bar gone in Sin bordes and covering the monitor, real fullscreen in Pantalla completa. Start the game in each mode: the game window matches (`[RT] window mode:` in `logs/game.log`).
- [ ] **Step 6:** Commit (launcher) `feat(launcher): display mode option (windowed, borderless, fullscreen)` with `Verified:`.

### Task 5: Package, the user's build, the user's checks

**Files:**
- Modify: `build/mi_build/Patapon3Recomp` (refresh: launcher exe + root `SDL2.dll` + the three game exes; settings.ini and saves untouched)
- Create: `build/plata/RESUMEN_LAUNCHER.md` (Spanish checklist)

- [ ] **Step 1:** Rebuild the three game trees (`ninja` + `ctest`) and the launcher (`cargo test`, `npm test`, then `P3_NO_BETA_GATE=1 npm run tauri build -- --no-bundle` last). Refresh `mi_build`: copy `launcher.exe`, `SDL2.dll` to the root and the exes to `game/<id>/`. Keep the old files in `build/plata/mi_build_prev2/`.
- [ ] **Step 2:** Write `RESUMEN_LAUNCHER.md` with the user's checks, one line each with what "bien" looks like:
  1. ← → on every ‹ › option (Resolución interna, Tamaño de ventana, Idioma, Filtro, FPS, Volumen) give the wrap at both ends, with the keyboard and with the pad.
  2. Temas: → on the last goes to the first.
  3. DualSense by USB and by Bluetooth navigates the launcher (cross, circle, L1/R1, stick). The Xbox/clone pad still does. Remapping a button with each pad stores the right name.
  4. Start the game with the DualSense: it works in the game (lightbar normal). Return: it navigates the launcher again.
  5. Modo de pantalla: Ventana / Sin bordes / Pantalla completa change the launcher at once and the game on its next start. Sin bordes covers the monitor with no bar, and Alt+Tab does not minimise the game. F11 in the game toggles window ↔ the chosen mode.
- [ ] **Step 3:** Update memory (launcher input + borderless, local commits, not pushed).
