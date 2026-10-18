# Patapon 3 community launcher — Part 4: remappable controls + Steam Deck style keyboard (design)

Date: 2026-10-18. Status: approved in conversation ("bien"), awaiting written-spec review.
Author: Claude Opus 5.5 with the user (brainstorming). Builder for the plan: DeepSeek v4.1 flash at max
effort (`deepseek/deepseek-flash#max` through OpenCode); Claude designs, reviews and verifies, and writes
anything under `psprecomp/crates/` (the DECO `opencode.json` still denies it). Follows
decomposition item 4 of `2026-10-17-launcher-part1-design.md`.

## Goal

The player sets their own keyboard keys and gamepad buttons in the launcher, and can type in the game
(hero name, etc.) with a gamepad only, through an on-screen keyboard modelled on the Steam Deck's, with
selectable skins.

### Decisions taken (conversation 2026-10-17/18)

- **Bindings per PSP button: up to 2 keyboard keys + 1 gamepad button.**
- **On-screen keyboard: drawn over the game, Steam Deck style, several skins.**
- **No extra "PC controls"** (no drum presets, no right stick) in this part.
- **Global controls:** one mapping in `settings.ini` for every release.
- The user verified on 2026-10-17 that launcher gamepad navigation and the in-game Select+Start
  back-to-menu combo work with a DualSense (part 1 fully verified).

### Facts verified before the design (2026-10-17)

- Keyboard mapping is a fixed `switch` in `runtime/src/psp_event_loop.cpp` (`key_to_psp_button`):
  arrows = D-pad, X / Return = CROSS, Z = CIRCLE, A = SQUARE, S = TRIANGLE, Q = L, W = R, Space = START,
  V / Tab = SELECT. Gamepad mapping is a fixed `switch` in `runtime/src/psp_pad.cpp`
  (`pad_button_to_psp`, SDL GameController button numbers): A = CROSS, B = CIRCLE, X = SQUARE,
  Y = TRIANGLE, BACK = SELECT, START, LEFTSHOULDER = L, RIGHTSHOULDER = R, D-pad; the triggers also
  press L / R (`PadSet::set_trigger`); the left stick is the analog stick; the mouse wheel is D-pad
  up/down (`psp_wheel.h`).
- `[controls]` is reserved in contract v1 (`docs/contracts/settings-v1.md`) and ignored by the reader
  (`runtime/src/psp_settings.cpp`); the launcher's Options › Controls tab is a read-only table
  (`launcher/src/screens/Options.tsx`, `CONTROLS`).
- The on-screen keyboard (`sceUtilityOsk`, `runtime/src/hle/psp_osk.cpp` → `OskSession`,
  `psp_osk_session.h`; editing with `TextEdit`, `psp_text_edit.h`; drawing with stb_truetype in
  `psp_osk_overlay.cpp`) only accepts physical-keyboard typing (`SDL_TEXTINPUT`); a gamepad can only
  accept (Start) or cancel (B). **There is no way to type with a gamepad today.**
- Reserved keys already used by the runtime: F1 (back to menu), F11 and Alt+Return (fullscreen); the
  gamepad Guide button also opens the back-to-menu prompt.

## 1. The contract: `[controls]` and `[display] osk_skin`

`[controls]` gets meaning (additive; `meta.version` stays 1, the section was reserved):

```ini
[controls]
; keyboard: up to two SDL key names, comma separated; gamepad: one SDL GameController button name
up_keys = Up            ; up_pad = dpup
down_keys = Down        ; down_pad = dpdown
left_keys = Left        ; left_pad = dpleft
right_keys = Right      ; right_pad = dpright
cross_keys = X, Return  ; cross_pad = a
circle_keys = Z         ; circle_pad = b
square_keys = A         ; square_pad = x
triangle_keys = S       ; triangle_pad = y
l_keys = Q              ; l_pad = leftshoulder
r_keys = W              ; r_pad = rightshoulder
start_keys = Space      ; start_pad = start
select_keys = V, Tab    ; select_pad = back
```

(one key per line in the real file; the defaults are exactly today's mapping).

- **Key names** are SDL's (`SDL_GetKeyFromName` / `SDL_GetKeyName`: `X`, `Return`, `Space`, `Tab`,
  `Left Shift`, `Up`, `1`, `F2`, `Keypad 4` …), case-insensitive. **Pad names** are SDL's
  GameController button names (`SDL_GameControllerGetButtonFromString`): `a b x y back start
  leftshoulder rightshoulder leftstick rightstick dpup dpdown dpleft dpright misc1 paddle1..4
  touchpad`. An empty value unbinds.
- **Fixed, not remappable:** left stick = analog; LT / RT also press L / R; F1, F11, Alt+Return and
  the pad `guide` button are reserved (refused as bindings); the back-to-menu combo holds the buttons
  bound to `select` and `start`.
- **Validation (game and launcher):** unknown action keys are ignored; an unknown name, a reserved key,
  or a third key → that entry falls back to its default and the game logs `[CONTROLS] <key>: …`; a
  key or pad button bound to two actions keeps the first action in the list order above and drops the
  other with a `[CONTROLS]` line. The launcher never writes such values (it moves a key instead).
- **Transport to the game:** the settings reader turns the section into one environment variable,
  `PSPRECOMP_CONTROLS`, canonical form `action=key1,key2|pad;…` (only actions present in the ini),
  following the contract rule "environment variable already set > settings.ini > default".
- **`[display] osk_skin = deck`** → `PSPRECOMP_OSK_SKIN=<ini folder>/skins/osk/<name>.json`. Missing
  or bad skin → the built-in Deck skin (`[OSK] skin …` log line).

## 2. Game side (`psprecomp`)

1. **`runtime/include/psp_controls.h`, `runtime/src/psp_controls.cpp` (pure, ctest
   `controls_tests`):** `ControlMap` with the defaults, `parse(spec, log)` for `PSPRECOMP_CONTROLS`
   (key and pad names resolved through injectable lookup functions so the tests need no SDL),
   `psp_button_for_key(keycode)`, `psp_button_for_pad(sdl_button)`, `pad_buttons_for(action)`.
   `psp_event_loop.cpp` and `psp_pad.cpp` use it instead of their `switch`es; the triggers and the
   wheel keep their fixed behaviour.
2. **Settings reader:** `[controls]` → `PSPRECOMP_CONTROLS`; `[display] osk_skin` →
   `PSPRECOMP_OSK_SKIN`. `test_settings` covers both (and the env-wins rule).
3. **On-screen keyboard model `runtime/include/psp_osk_keyboard.h`, `src/psp_osk_keyboard.cpp`
   (pure, ctest `osk_keyboard_tests`):** a Steam Deck style key grid with layouts `es` (QWERTY + `ñ`)
   and `en`, two layers (letters; numbers and symbols incl. `¿ ¡ @ # € % & ( ) - _ ' " : ; , . ? !`),
   shift (one letter) and caps (double shift), an accent key (`´` then a vowel → `á é í ó ú`; `¨` →
   `ü`), and grid keys Space, Backspace, ← →, Accept, Cancel. Actions: `Up Down Left Right` (wrap per
   row, column snapped by key centre), `Press`, `Backspace`, `Shift`, `Space`, `CursorLeft`,
   `CursorRight`, `Layer`, `Accept`, `Cancel`. It edits the `OskSession` through a small interface
   (insert, backspace, move cursor, accept, cancel); `TextEdit` gains a cursor
   (`move_cursor(int)`, insert / backspace at the cursor).
4. **Gamepad while the keyboard is open:** D-pad / left stick (with repeat) move, **A = Press,
   B = Backspace, X = Shift, Y = Space, LB / RB = cursor, Select (back) = Layer, Start = Accept**;
   Cancel is a grid key (and Esc on the physical keyboard). These are positional SDL buttons, fixed for
   the keyboard (the game mapping does not apply while it is open). Physical typing is unchanged.
5. **Overlay:** `psp_osk_overlay.cpp` draws the text box (as today, with the cursor) and, below it,
   the key grid with the focused key highlighted and a hint line ("Ⓐ escribir · Ⓑ borrar · Ⓧ mayús ·
   Ⓨ espacio · Start aceptar") in `PSPRECOMP_UI_LANG`. Layout follows the skin.
6. **Skins `runtime/include/psp_osk_skin.h`, `src/psp_osk_skin.cpp` (pure parse, ctest in
   `osk_keyboard_tests`):** JSON `{ "name", "background", "panel", "key", "key_text", "focus",
   "focus_text", "accent", "radius", "gap", "opacity", "font" }` (colors `#RRGGBB` or `#RRGGBBAA`,
   numbers in pixels at 1080p, `font` optional path relative to the skin). Shipped skins in
   `runtime/assets/osk_skins/`: `deck.json` (Steam Deck dark grey, blue focus), `light.json`,
   `patapon.json` (black / white with red and yellow accents). `p3.sh build` copies them to
   `<build>/skins/osk/`; make-portable copies them to the portable folder's `skins/osk/`.
7. **`PSPRECOMP_OSK_DEMO=1`:** opens a demo keyboard request ("Demo", 16 characters) two seconds after
   boot, so the skins can be captured over the title screen without playing (documented in
   `docs/ENV_FLAGS.md`; logs the text on accept).
8. **Docs:** `docs/contracts/settings-v1.md` (`[controls]`, `osk_skin`), `docs/ENV_FLAGS.md`
   (`PSPRECOMP_CONTROLS`, `PSPRECOMP_OSK_SKIN`, `PSPRECOMP_OSK_DEMO`), `ARCHITECTURE.md` (input
   mapping and keyboard rows), `README.md` (controls are configurable).

## 3. Launcher side (`DECO/launcher`)

1. **`src-tauri/src/controls.rs`:** the action list and defaults (same as the contract), name
   validation (the SDL key names the UI can produce, the pad names above), reserved keys, conflict
   resolution (`assign(bindings, action, slot, name) -> (bindings, moved_from: Option<(action, slot)>)`),
   read / write through the existing `settings.rs` save path (keeps comments and unknown keys).
2. **Gamepad capture:** `gamepad.rs` maps gilrs buttons to SDL names (South → `a`, East → `b`,
   West → `x`, North → `y`, LeftTrigger → `leftshoulder`, RightTrigger → `rightshoulder`,
   Select → `back`, Start → `start`, LeftThumb → `leftstick`, RightThumb → `rightstick`, DPad* →
   `dp*`; LeftTrigger2 / RightTrigger2 and Mode are refused); a `capture_pad` state makes the next
   button press emit `pad_captured { name }` instead of a navigation event (`start_pad_capture`,
   `cancel_pad_capture` commands).
3. **Keyboard capture** happens in the web UI: `KeyboardEvent.code` → SDL key name through a table
   (`KeyX` → `X`, `Enter` → `Return`, `ArrowUp` → `Up`, `Space` → `Space`, `ShiftLeft` →
   `Left Shift`, `Digit1` → `1`, `Numpad4` → `Keypad 4`, `F2` → `F2`, …); unknown codes are refused
   with a message.
4. **Options › Controls** becomes an editor: rows = the 12 actions (PSP glyph + name), columns
   Key 1 · Key 2 · Gamepad; Confirm on a cell → "Press a key…" / "Press a button…" (Esc / ○ cancels);
   Delete / Backspace or the reset key on a cell clears it; R restores all defaults; a moved key shows
   "Quitada de ✕" for a few seconds; the existing save / discard flow applies. Below, the keyboard skin
   picker (`deck`, `light`, `patapon`, plus any `skins/osk/*.json`) with a colour swatch from the skin
   file (`list_osk_skins` command).
5. Strings in `lang/es.json` / `lang/en.json`; the launcher README documents controls and skins.

## 4. Testing and success criteria

- **ctest:** `controls_tests` (defaults equal today's mapping; parse valid / invalid names / third key /
  reserved / duplicate; env round trip), `osk_keyboard_tests` (layout grid and navigation wrap, shift
  vs caps, accents, symbols layer, cursor editing, max length, accept / cancel through a fake session,
  skin parse with bad colours / missing fields → defaults), `settings_tests` additions.
- **cargo test (launcher):** controls defaults / validation / conflict move / ini round trip; gilrs →
  SDL names; capture state machine.
- **Vitest:** Controls editor (capture a key, move from another action, clear, reset, pad capture
  event), skin picker.
- **Regression:** ctest all green; 1.1.4 tour `RT_REGRESS PASS`; without `[controls]` the game behaves
  exactly as today (default map test + tour).
- **End to end (port 9998):** launcher (CDP driver `build/e2e3/cdp.mjs`) changes cross to `K` and
  `square_pad` to `y` → `settings.ini` holds them → the game log has `[CONTROLS] cross: K` lines and a
  debug-socket-free check that `PSPRECOMP_CONTROLS` reached the exe; `PSPRECOMP_OSK_DEMO=1` screenshots
  of the three skins (debug socket `S`).
- **Part 4 is done when** the user types a hero name with the DualSense only in each skin, remaps a
  key and plays with it, the regression gates pass, nothing is pushed without the user's OK, and the
  user approves.

## Out of scope

Control profiles; right-stick bindings; drum presets; in-game gamepad glyphs; per-release controls;
a visual skin editor and launcher-side keyboard previews beyond the colour swatch (part 5, themes);
remapping the analog stick, triggers or mouse wheel.
