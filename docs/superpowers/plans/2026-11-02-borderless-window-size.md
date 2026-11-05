# Borderless window with a chosen size — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fourth display mode, "Ventana sin bordes": the game window without frame, at the size of "Tamaño de
ventana", centred on its display.

**Architecture:** One more `PspWindowMode` value (`borderless_window`) parsed from `PSPRECOMP_WINDOW_MODE` /
`settings.ini display.window_mode`; a pure rule computes its rectangle (centred `480·k × 272·k`, clamped to the
display, and — when it would cover the display — the existing 1-px rule against exclusive-fullscreen promotion);
`apply_window_mode` applies it. The launcher offers the value; its own window stays a normal window in that mode.

**Tech Stack:** C++17 + SDL2 runtime (psprecomp), Tauri launcher (Rust catalog + React).

**Spec (approved in chat, 2 Nov 2026):** new mode "Ventana sin bordes" next to "Ventana", "Sin bordes" (full
display) and "Pantalla completa", which keep working as now. Size = "Tamaño de ventana"; bigger than the screen →
capped to the screen, with the same anti-flicker rule as "Sin bordes". F11 / Alt+Enter: that window ↔ a normal
window. Not draggable (no title bar), centred. The launcher opens as a normal window in this mode.

## Global Constraints

- Value name `borderless_window` everywhere (env, settings.ini, catalog); labels es "Ventana sin bordes",
  en "Borderless window".
- Existing values `windowed | borderless | fullscreen` keep their exact behaviour.
- psprecomp work on a new local branch `borderless-window` from `ping-masks` (u16 content); launcher on
  `patapon-theme`. No push.
- Gates: ninja + ctest (runtime build `build/rtw`), vitest + cargo test + tsc (launcher).
- Commits authored "Patapon Community", ending `Verified: …` / `Agent: Claude Opus 5.5`.

## Review Focus

1. A size larger than the display (scale 8 on 1366×768): capped, never off-screen. Test in Task 1.
2. A size exactly the display (scale 4 at 1920×1088 on a 1920×1080 screen is larger → capped; scale 4 on 1920×1088
   display would match): must not equal the monitor rect (driver fullscreen promotion). Test in Task 1.
3. Second monitor on the left/right: the window centres on the display the window is on, not display 0. Test in
   Task 1 (display at negative x).
4. F11 from the borderless window and back: the window returns framed at the same size, then frameless again.
   Checked live in Task 1.
5. An older launcher/settings.ini with `borderless_window` but an older game exe: the game falls back to the legacy
   switch (no crash). Already covered by `psp_window_mode("bogus", …)`.

---

### Task 1: The game's fourth mode

**Files:**
- Modify: `runtime/include/psp_window_mode.h`, `runtime/src/psp_event_loop.cpp:70-125`,
  `runtime/src/psp_settings.cpp:37`, `docs/ENV_FLAGS.md` (PSPRECOMP_WINDOW_MODE), `docs/contracts/settings-v1.md:26`
- Test: `runtime/tests/test_window_mode.cpp`

**Interfaces:**
- Produces: `PspWindowMode::BorderlessWindow`; `psp_window_mode("borderless_window", …)`; name
  `"borderless_window"`; `PspRect psp_borderless_window_rect(PspRect display, int w, int h, const PspRect* others,
  int count)`.

- [ ] **Step 1: Failing tests** in test_window_mode.cpp:
  - `mode_from_env`: `psp_window_mode("borderless_window", nullptr) == M::BorderlessWindow` and
    `psp_window_mode("Borderless_Window", "1") == M::BorderlessWindow`; `psp_window_mode_name(M::BorderlessWindow)`
    is `"borderless_window"`.
  - `window_centred`: display {0,0,1920,1080}, 960×544 → {480, 268, 960, 544}.
  - `window_on_left_monitor`: display {-1366,0,1366,768}, 960×544 → {-1163, 112, 960, 544}.
  - `window_capped`: display {0,0,1366,768}, 3840×2176 alone → equals `psp_borderless_rect` of that display
    ({0,0,1366,769}).
  - `toggle_borderless_window`: `psp_window_mode_toggle(M::BorderlessWindow, M::BorderlessWindow) == M::Windowed`,
    `psp_window_mode_toggle(M::Windowed, M::BorderlessWindow) == M::BorderlessWindow`.
- [ ] **Step 2: Run** `ninja -C ../build/rtw test_window_mode && ../build/rtw/test_window_mode.exe` — Expected:
  compile FAIL (`BorderlessWindow` unknown).
- [ ] **Step 3: Implement** the header (enum value, parse, name, rect rule: if `w >= d.w || h >= d.h` return
  `psp_borderless_rect(d, others, count)`, else centred `{d.x + (d.w - w) / 2, d.y + (d.h - h) / 2, w, h}`), then
  `apply_window_mode` case `BorderlessWindow` (fullscreen off, restore if maximised, frame off, rect from
  `480 * g_window_scale`, `272 * g_window_scale` on the window's display, log `[RT] borderless window rect WxH at
  X,Y`), the `Windowed` case also re-sizes when coming from `BorderlessWindow`, `SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS`
  "0" for it too, settings enum list, docs.
- [ ] **Step 4: Run** the test — Expected: PASS; then `ninja -C ../build/rtw` and `ctest --test-dir ../build/rtw`
  all green.
- [ ] **Step 5: Live check:** run `build/rtw/psprecomp_runtime.exe` with `PSPRECOMP_WINDOW_MODE=borderless_window`,
  `PSPRECOMP_WINDOW_SCALE=2`, a lab port (9998) and the dev disc0 for ~20 s; Expected log line
  `[RT] borderless window rect 960x544 at …` centred on the display; a screenshot shows no frame. F11 then F11:
  framed, then frameless again (log lines `window mode: windowed` / `borderless_window`).
- [ ] **Step 6: Commit** `feat(window): borderless window at the chosen window size`.

### Task 2: The launcher offers it

**Files:**
- Modify: `launcher/src-tauri/core_options.json` (window_mode values + display; window_scale description),
  `launcher/src-tauri/src/options.rs` (`WINDOW_MODES`, test)
- Test: `launcher/src-tauri/src/options.rs` tests

- [ ] **Step 1: Failing test** in options.rs: `window_mode_of` of an ini with `window_mode = borderless_window`
  returns `"borderless_window"`; `normalize(window_mode, "borderless_window")` is Some.
- [ ] **Step 2: Run** `cargo test -q --manifest-path src-tauri/Cargo.toml --lib options` — Expected: FAIL.
- [ ] **Step 3: Implement**: value + labels es "Ventana sin bordes" / en "Borderless window"; window_scale
  description says it also sizes "Ventana sin bordes"; `WINDOW_MODES` gets the value (lib.rs `apply_window_mode`
  keeps it in its `_` arm: a normal launcher window).
- [ ] **Step 4: Run** cargo test, vitest, tsc, `scripts/pack_font_map.py --check` (new label text) — Expected:
  green.
- [ ] **Step 5: Commit** `feat(options): Ventana sin bordes display mode`; rebuild the release launcher and copy
  it (only if the test folder is not running) to `build/plata/tema_prueba/Patapon3Recomp`, plus the new game exe
  as that folder's `game/dxd-1.1.5b/Patapon 3 DxD Remastered.exe` (beta build via `build/beta/make_beta_exes.sh`
  is Task 3 of the u16 packaging, not here: copy the plain `build/v115b/rtw` exe).
