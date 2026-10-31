# Composed borderless window + display-aware auto render scale — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the user works natively: no helper
> models, see memory native-no-helpers). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** (1) "Sin bordes" becomes a real composed window: clicking outside no longer flickers and Win+G shows the
Game Bar overlay. (2) The launcher's automatic resolution stops picking a scale below the screen on small screens
(beta report #59, Xray, 1366x768).

**Architecture:** (1) The OpenGL driver promotes a window whose client area covers a monitor exactly to exclusive
fullscreen (GLFW maintainers; Godot 3/4 work around it the same way). The borderless rectangle grows 1 px into a
side of the display that touches no other display, so it never matches the monitor; the rule stays pure in
`psp_window_mode.h`, the event loop only feeds it the display list. (2) `gpu::scale_for` gets the primary
monitor's height: an integrated GPU on a screen under 1024 px tall starts at 3 instead of 2.

**Tech Stack:** C++17 + SDL2 (psprecomp runtime), Rust + Tauri 2 (launcher).

**Spec:** this conversation, 30 Oct 2026: triage `build/beta/reportes_2026-10-30/TRIAGE.md` (report #59) and the
user's words: "cuando doy click fuera del juego, la pantalla parpadea, por que es como si estuviera en pantalla
completa, y esa es otra, si doy windows + G, no se muestra la superposicion de windows".

## Global Constraints

- Branches: psprecomp `borderless-composed` from `pad-sticks` (7d856af, = u8); launcher `auto-scale-display` from
  `pad-sticks` (4a8c743, = u8). Local only: never push (the user publishes).
- Commits: author "Patapon Community"; message ends with the `Claude-Session:` line only, no `Co-Authored-By`
  (memory commit-attribution).
- Fullscreen mode (`SDL_WINDOW_FULLSCREEN_DESKTOP`) and windowed mode do not change.
- The 1 px goes to a side that touches no other display; never into a neighbour monitor.
- Auto scale only applies while `[launcher] render_scale_chosen` is unset (existing rule, unchanged): Xray, who
  already picked a resolution, keeps the choice.
- Thresholds, copied exactly: integrated GPU and primary monitor height `< 1024` → 3; height `>= 1024` or unknown
  → 2; dedicated (>= 2 GiB) → 4 whatever the screen.
- Visible change → one line in `build/beta/CHANGELOG.md`, new section "Actualización 9 (en preparación)", Spanish,
  for players.

## Review Focus

- Two monitors side by side, game on the left one: the window must grow downward, not 1 px into the right monitor
  (`neighbour_on_the_right_grows_down`).
- Taskbar/monitor below (laptop + monitor stacked): grows right (`neighbour_below_grows_right`).
- Display with negative origin (monitor left of primary): rect math stays correct (`negative_origin`).
- F11 from borderless back to windowed and again to borderless: the window returns to the oversized rect, not the
  exact bounds (event loop reuses `psp_borderless_rect`; checked in Task 3 step 3).
- DPI scaling 125 % on a 1080 laptop: Tauri's `PhysicalSize` gives 1080, not 864, so it keeps 2
  (`physical_height_used`, Task 1 uses `primary_monitor()`'s physical size, never logical).

---

### Task 1: Launcher auto scale knows the screen height

**Files:**
- Modify: `launcher/src-tauri/src/gpu.rs` (`scale_for`, `auto_render_scale`, tests)
- Modify: `launcher/src-tauri/src/lib.rs:576` (call site in `setup`)

**Interfaces:**
- Produces: `pub fn scale_for(adapters: &[(u32, u64, bool)], display_height: Option<u32>) -> u32` and
  `pub fn auto_render_scale(display_height: Option<u32>) -> u32`.

- [x] **Step 1: Branch** — `git -C launcher switch -c auto-scale-display pad-sticks`.
- [x] **Step 2: Write the failing tests** in `gpu.rs` `mod tests` (update the five existing calls to pass
  `Some(1080)`, keep their expectations):

```rust
const IGPU: (u32, u64, bool) = (0x8086, 128 << 20, false);
#[test] fn integrated_on_768_gets_3() { assert_eq!(scale_for(&[IGPU], Some(768)), 3); }
#[test] fn integrated_on_900_gets_3() { assert_eq!(scale_for(&[IGPU], Some(900)), 3); }
#[test] fn integrated_on_1024_keeps_2() { assert_eq!(scale_for(&[IGPU], Some(1024)), 2); }
#[test] fn integrated_on_1080_keeps_2() { assert_eq!(scale_for(&[IGPU], Some(1080)), 2); }
#[test] fn unknown_screen_keeps_2() { assert_eq!(scale_for(&[IGPU], None), 2); }
#[test] fn dedicated_on_768_still_4() { assert_eq!(scale_for(&[IGPU, (0x10DE, 4 << 30, false)], Some(768)), 4); }
```

- [x] **Step 3: Run, expect compile FAIL** — `cargo test --manifest-path launcher/src-tauri/Cargo.toml gpu::`
  → "this function takes 1 argument but 2 arguments were supplied".
- [x] **Step 4: Implement.** `scale_for`: dedicated → 4; else `match display_height { Some(h) if h < 1024 => 3,
  _ => 2 }`. Doc comment: why 1024 — under it scale 2 (544 px) is stretched by a non-integer factor; from 1024 up,
  2 is ~2x already and 3 would cost an integrated GPU 2.25x the pixels. Module doc line 1: "GPU and screen height".
- [x] **Step 5: Call site** `lib.rs:576`: height = `app.primary_monitor().ok().flatten().map(|m| m.size().height)`
  (physical pixels; the game opens on the primary display, SDL display 0), passed to `gpu::auto_render_scale`.
  Update `this_pc` to print `auto_render_scale(None)`.
- [x] **Step 6: Verify** — `cargo test --manifest-path launcher/src-tauri/Cargo.toml` → all pass (was 167 + 6 new);
  `npm --prefix launcher test -- --run` → 112 pass (no TS change expected).
- [x] **Step 7: Commit** — `fix(launcher): automatic resolution follows the screen on integrated GPUs` (body: report
  #59, 1366x768 got 960x544 stretched 1.41x).

### Task 2: Borderless rectangle that the driver cannot take for fullscreen

**Files:**
- Modify: `psprecomp/runtime/include/psp_window_mode.h` (`psp_borderless_rect`)
- Test: `psprecomp/runtime/tests/test_window_mode.cpp` (replace `borderless_rect_is_display_bounds`)

**Interfaces:**
- Produces: `PspRect psp_borderless_rect(PspRect display, const PspRect* others, int count)` — `others` are the
  other displays' bounds (the display itself may be included; equal rects are skipped).

Rule (order matters): grow 1 px toward bottom, else right, else top, else left — the first side no other display
touches. A display "touches" side S when it shares that edge line and overlaps on the perpendicular span by > 0
px (corner-only contact does not count). If all four sides touch, return the display 1 px shorter (`h - 1`).

- [x] **Step 1: Branch** — `git -C psprecomp switch -c borderless-composed pad-sticks`.
- [x] **Step 2: Write the failing tests** (replace the old test, add each to `main`):

```cpp
static void alone_grows_down() {
    const PspRect r = psp_borderless_rect({0, 0, 1366, 768}, nullptr, 0);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1366 && r.h == 769, "1 px taller, off the bottom edge");
}
static void neighbour_on_the_right_grows_down() {
    const PspRect o[] = {{1366, 0, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1366, 768}, o, 1);
    CHECK(r.w == 1366 && r.h == 769, "side by side: down, never into the right monitor");
}
static void neighbour_below_grows_right() {
    const PspRect o[] = {{0, 1080, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1920, 1080}, o, 1);
    CHECK(r.x == 0 && r.w == 1921 && r.h == 1080, "stacked: right");
}
static void below_and_right_grows_up() {
    const PspRect o[] = {{0, 1080, 1920, 1080}, {1920, 0, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1920, 1080}, o, 2);
    CHECK(r.y == -1 && r.h == 1081 && r.w == 1920, "up");
}
static void surrounded_shrinks() {
    const PspRect o[] = {{0, 1080, 1920, 1080}, {1920, 0, 1920, 1080}, {0, -1080, 1920, 1080}, {-1920, 0, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1920, 1080}, o, 4);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1079, "no free side: 1 px shorter");
}
static void corner_contact_ignored() {
    const PspRect o[] = {{1920, 1080, 1920, 1080}};
    const PspRect r = psp_borderless_rect({0, 0, 1920, 1080}, o, 1);
    CHECK(r.h == 1081 && r.w == 1920, "diagonal monitor does not block the bottom");
}
static void negative_origin() {
    const PspRect o[] = {{0, 0, 1920, 1080}, {-1920, 0, 1920, 1080}};  // self included, skipped
    const PspRect r = psp_borderless_rect({-1920, 0, 1920, 1080}, o, 2);
    CHECK(r.x == -1920 && r.y == 0 && r.w == 1920 && r.h == 1081, "left monitor grows down");
}
```

- [x] **Step 3: Run, expect FAIL** — `cmake --build build/rtw --target test_window_mode` → compile error (old
  one-argument signature).
- [x] **Step 4: Implement** `psp_borderless_rect` in the header (a small `touches(PspRect d, PspRect o, int side)`
  in `psp_window_mode_detail`). Update the header's line-2 comment: "the borderless rectangle (1 px past a free
  edge so OpenGL drivers keep it composed)".
- [x] **Step 5: Verify** — `cmake --build build/rtw --target test_window_mode && ctest --test-dir build/rtw -R
  window_mode --output-on-failure` → PASS.
- [x] Commit together with Task 3 (the header change breaks the event loop's call until then).

### Task 3: Event loop feeds the displays; build; user check

**Files:**
- Modify: `psprecomp/runtime/src/psp_event_loop.cpp:87-97` (Borderless case of `apply_window_mode`)
- Modify: `build/beta/CHANGELOG.md`
- Create: `build/plata/probar_sin_bordes.bat`

**Interfaces:**
- Consumes: `psp_borderless_rect(PspRect, const PspRect*, int)` from Task 2.

- [x] **Step 1: Implement.** In the Borderless case: collect up to 8 `SDL_GetDisplayBounds` of
  `SDL_GetNumVideoDisplays()` into a `PspRect[8]`, pass them; after `SDL_SetWindowSize` log
  `[RT] borderless rect %dx%d at %d,%d` (the actual `SDL_GetWindowPosition/Size`, so a clamp by SDL shows up in
  player logs). Comment the why in one line (driver exclusive-fullscreen promotion).
- [x] **Step 2: Build + tests** — `ninja -C build/rtw` and `ctest --test-dir build/rtw` → 125/125 (+0: same test
  binary). Tour gate (window mode untouched by it, proves nothing else moved):
  `TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/borderless1`
  then `python -I build/tools/rt_regress.py build/tour/wait30 build/tour/borderless1` → `RT_REGRESS PASS`.
- [x] **Step 3: Check the real window on this PC.** Start `build/rtw/psprecomp_runtime.exe` with
  `PSPRECOMP_WINDOW_MODE=borderless` (same env as `build/plata/probar_barra.bat`, savedata a COPY), log to
  `build/logs/sin_bordes.log`. After the title shows: the log has `borderless rect <W>x<H+1> at 0,0` for the primary
  display; Win32 `GetWindowRect` of the game's window (python ctypes, `FindWindowW` by the process's PID) matches
  it. Send F11 twice (`build/plata/lnav/keys.py` with the Alt trick, memory p3-launcher-input-borderless) → log shows windowed then
  borderless with the same oversized rect. Stop only the PID you started.
- [x] **Step 4: User test .bat** `build/plata/probar_sin_bordes.bat` modelled on `probar_barra.bat`
  (`PSPRECOMP_WINDOW_MODE=borderless`, savedata copy `build\plata\save_sin_bordes`, log
  `build\logs\plata_sin_bordes.log`). First line `rem`: what to look at — 1) click on another window or the second
  monitor: the game stays visible, no black flash; 2) Win+G opens the Game Bar on top of the game; 3) no 1-px line
  of desktop at the bottom.
- [x] **Step 5: CHANGELOG** under "Actualización 9 (en preparación)" → Arreglos: "**Ventana sin bordes**: ya no
  parpadea al hacer clic fuera del juego y Win+G muestra la barra de juego de Windows." and "**Resolución
  automática**: en pantallas pequeñas (por ejemplo 1366x768) con gráficos integrados el juego arranca más nítido."
- [x] **Step 6: Commit** psprecomp — `fix(window): borderless window 1 px past a free display edge` (body: OpenGL
  drivers promote an exactly-covering window to exclusive fullscreen: flicker on focus loss, no Game Bar). Commit
  this plan's checkbox state with it.
- [ ] **Step 7: Hand over** — tell the user (Spanish): run `probar_sin_bordes.bat`, the three checks; if the flicker
  persists, report it with the log's `borderless rect` line and stop: the next lever (rendering through a DXGI
  swapchain) is a separate plan. Update memory
  `p3-launcher-input-borderless` and `user-changelog`; nothing is pushed.
