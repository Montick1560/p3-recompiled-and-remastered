# Environment Variables and Build Options

Complete reference for every `PSPRECOMP_*` environment variable and CMake
option in the project. Each entry lists where it is read, its type, default,
effect, and when you would set it.

Three kinds of knob appear here:

- **env** — an environment variable read at runtime or during the Rust
  pipeline (`std::env::var` in Rust, `std::getenv` in C++).
- **cmake** — a CMake cache variable/`option()` passed at configure time with
  `-D<NAME>=<VALUE>`.
- **both** — a name used as a CMake variable *and* exported into the
  environment of a sub-process (e.g. `PSPRECOMP_CROSS_MID`, which CMake only
  prints in its regenerate hint while the Rust pipeline reads it from the env).

Unless stated otherwise, a boolean env flag is "on" when set to `1` and
otherwise off; several flags activate on mere *presence* (any value) — that is
called out per entry.

## Most users only need these

| Flag | Type | What it does |
|------|------|--------------|
| `PSPRECOMP_CROSS_MID` | env | Recompile with cross-function mid-jump recovery. The documented build line sets `PSPRECOMP_CROSS_MID=1` before `cargo run -- recompile`. |
| `PSPRECOMP_GAME` | cmake | Selects which `games/<id>` module compiles into the runtime (`patapon` default, `none` for a generic build). |
| `PSPRECOMP_OUTPUT_DIR` | cmake | Points the runtime build at a recompiler output directory other than `../output`. |
| `PSPRECOMP_DISC0` | env | Path to the extracted disc image the runtime reads game data from (default `./disc0`). |
| `PSPRECOMP_STRICT` | env | Abort the runtime on the first dispatch lookup miss instead of returning a noop stub. |
| `PSPRECOMP_CLEANROOM` | env | Disable the Patapon game module's behavioral band-aids — the verification gate. |

Everything below "Debug and trace flags (advanced)" is for investigation only;
none of it is needed for a normal recompile-build-run cycle.

---

## Pipeline / recompile (Rust)

These are read by the `psprecomp` Rust binary during `analyze`/`recompile`/`dump`.

### `PSPRECOMP_CROSS_MID`
- **Read in:** `crates/psp-cli/src/recompile.rs`, `crates/psp-cli/src/config.rs`,
  `crates/psp-cli/src/dump.rs`, `crates/psp-cli/src/main.rs`,
  `crates/psp-cli/src/fingerprint.rs`
- **Type:** env (`=1` to enable) — also referenced in `runtime/CMakeLists.txt`
  and `games/patapon/game.toml` as the documented regenerate command.
- **Default:** off.
- **Effect:** When `=1`, the recompile pipeline coalesces Ghidra-over-split
  shared-frame siblings and runs systematic cross-function mid-jump recovery
  (D2+D3a), and activates the manifest's `force_entries_cross_mid` list. Gated
  off by default because landing it makes the broken decompressor at
  `0x089D7xxx` run and stall (D1 baseline note in `recompile.rs`).
- **Set it when:** generating output (the project's documented build line uses
  it). Pass the same value to `dump` so a single function's dumped C++ matches
  its batch file.

### `PSPRECOMP_NO_COALESCE`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable). Only has effect when `PSPRECOMP_CROSS_MID=1`.
- **Default:** off.
- **Effect:** Disables only the shared-frame sibling merge while leaving
  cross-function mid-jump injection on (an A/B diagnostic isolating the merge).
- **Set it when:** bisecting a regression introduced by `PSPRECOMP_CROSS_MID`.

### `PSPRECOMP_NO_RA_MODEL`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable).
- **Default:** off.
- **Effect:** Keeps the structural sibling merge but disables the Option-A
  LINK/RA lowering applied to coalesced owners (a diagnostic splitting the
  merge from the RA model).
- **Set it when:** isolating which half of the coalesce change moved behavior.

### `PSPRECOMP_COALESCE_DEBUG`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable). *Advanced / diagnostic.*
- **Default:** off.
- **Effect:** Logs each coalesce owner that absorbed >= 4 siblings (owner
  address, sibling count, member list) via `tracing::info!`.
- **Set it when:** inspecting which functions the coalescer merged.

### `PSPRECOMP_ALLOW_STALE`
- **Read in:** `crates/psp-cli/src/fingerprint.rs` and
  `runtime/CMakeLists.txt` (as a CMake `option()`).
- **Type:** both (env for the pipeline-side check; CMake option for the
  configure-time check — `-DPSPRECOMP_ALLOW_STALE=ON`).
- **Default:** off / `OFF`.
- **Effect:** Downgrades the output-staleness fingerprint mismatch from a fatal
  error to a warning. The fingerprint compares the emitter source hash against
  `output/fingerprint.json`; a mismatch means `output/` does not reflect the
  current emitter sources.
- **Set it when:** you knowingly want to build/recompile against an out-of-date
  `output/` (accepting that runtime behavior will not match current sources).

---

## Recompile manifest selection

### `PSPRECOMP_GAME` (pipeline side)
- **Read in:** `crates/psp-cli/src/config.rs`
- **Type:** env. (Distinct from the CMake `PSPRECOMP_GAME` below; same name,
  different layer.)
- **Default:** unset.
- **Effect:** On the pipeline side, used while resolving the per-game config so
  the recompile uses the matching `games/<id>` manifest context.
- **Set it when:** scripting a recompile for a non-default game id. For a normal
  build the `--config games/<id>/game.toml` argument already pins the game.

---

## Runtime — run-time configuration (C++)

Read by the runtime binary at start-up or during execution.

### Command line and `settings.ini`
- **Read in:** `runtime/src/psp_settings.cpp` (`psp_settings_apply`, the first thing
  `main()` does). Contract: [`docs/contracts/settings-v1.md`](contracts/settings-v1.md).
- **`--settings <path>`:** read this `settings.ini`. Without it the runtime reads
  `settings.ini` next to the executable when it exists; with neither nothing changes.
- **`--log <path>`:** stderr goes to this file (the launcher passes `../logs/game.log`).
- **Effect:** each known key becomes the `PSPRECOMP_*` variable below that it maps to
  (paths resolved against the ini's folder; `[game]` keys come from `game_options.json`
  next to the executable). **Precedence:** a variable already set in the environment >
  `settings.ini` > the runtime default. Invalid values keep the default. Every action
  is logged as a `[SETTINGS]` line.
- **Back to the launcher menu:** F1, Select+Start held 1 s on a gamepad, or the Guide
  button when SDL delivers it, opens a "Back to the menu?" Yes/No box (starts on No;
  arrows / d-pad choose, Enter / A confirm, Esc / B cancel; the game gets no input while
  it is open). Yes quits with exit code 10.
- **Exit codes:** 0 = the player quit, 10 = back to the launcher menu, anything else =
  failure.

### `PSPRECOMP_UI_LANG`
- **Read in:** the runtime's own dialogs (the "back to the menu" prompt).
- **Type:** env, `es` | `en`.
- **Default:** `es`.
- **Effect:** Language of in-game runtime dialogs. Set from `general.language` in
  `settings.ini`.

### `PSPRECOMP_UI_COLORS`
- **Read in:** `runtime/src/psp_menu_prompt.cpp` (`menu_prompt_colors`, once, the first time
  the "back to the menu" prompt opens).
- **Type:** env, `key=#rrggbb` pairs separated by `;` (the launcher theme keys `bg`, `text`,
  `accent`, `panel`, `panelText`, `muted`).
- **Default:** unset = the built-in dark prompt.
- **Effect:** The launcher sets it from the theme it shows (a locked theme never reaches the
  game) so the prompt looks like the launcher's own dialogs: `panel` background, `panelText`
  text, `muted` hint, the chosen button on `accent` with black text. Bad entries are ignored;
  not in `settings.ini`.

### `PSPRECOMP_ACHIEVEMENTS_FILE`
- **Read in:** `runtime/src/psp_achievements_run.cpp` (`psp_achievements_start`).
- **Type:** env (path). Set by the settings reader to `<ini folder>/achievements/unlocked.json`.
- **Default:** unset = achievements off (developer runs without `settings.ini` behave as before).
- **Effect:** where the game appends earned achievements (docs/contracts/achievements-v1.md).

### `PSPRECOMP_ACHIEVEMENT_TOASTS`
- **Read in:** `runtime/src/psp_achievements_run.cpp`.
- **Type:** env, `1` | `0`. From `[game] achievement_toasts` (game_options.json).
- **Default:** on.
- **Effect:** `0` hides the in-game "achievement unlocked" pop-ups; achievements are still recorded.

### `PSPRECOMP_ACHIEVEMENTS`
- **Read in:** `runtime/src/psp_achievements_run.cpp`.
- **Type:** env, `0` = off.
- **Default:** on (when `PSPRECOMP_ACHIEVEMENTS_FILE` is set).
- **Effect:** kill switch: no catalogue, no worker, no vblank hook.

### `PSPRECOMP_DISC0`
- **Read in:** `runtime/src/main.cpp`
- **Type:** env (path string).
- **Default:** `./disc0`.
- **Effect:** Overrides the disc0 directory the I/O layer maps
  (`disc0:/...` guest paths). If the path does not exist the runtime warns and
  continues with no game data.
- **Set it when:** running against an extracted ISO that is not in `./disc0`.

### `PSPRECOMP_PATCHES`
- **Read in:** `runtime/src/psp_patch_sets.cpp` (`psp_patch_sets_apply`, called
  from `runtime/src/main.cpp` right after the dispatch table is built).
- **Type:** env (set names separated by commas, spaces or semicolons; exact,
  case-sensitive match).
- **Default:** unset (no patch set; the game runs unpatched).
- **Effect:** Registers the named `[[patch_sets]]` of the recompiled game
  (`games/<id>/game.toml`, emitted into the output's `patch_sets.cpp`) over the
  dispatch table: every function holding a patched word runs its patched
  variant, and the set's code caves become callable. Each enabled set logs
  `[RT] Patch set '<name>' enabled (<n> entries)`; an unknown name logs a
  warning with the available sets. A game module can pair a set with host-side
  help (`psp_patch_set_active`); Patapon 3's `60fps` set paces the input update
  to 30 Hz, turns on smooth animation (`PSPRECOMP_P3_ANIM_SMOOTH`) and paces the
  weather (`PSPRECOMP_P3_WEATHER_30HZ`, `PSPRECOMP_P3_WEATHER_SMOOTH`) and the
  script objects (`PSPRECOMP_P3_HIDEOUT_30HZ`), the scripts' frame waits
  (`PSPRECOMP_P3_SCRIPT_WAIT_30HZ`) and the item lists' hold repeat
  (`PSPRECOMP_P3_MENU_30HZ`).
- **Set it when:** playing with an opt-in game patch, e.g. Patapon 3
  `PSPRECOMP_PATCHES=60fps` (60 fps; see `games/patapon3/game.toml`).

### `PSPRECOMP_NO_AUDIO`
- **Read in:** `runtime/src/psp_audio_out.cpp`
- **Type:** env (presence).
- **Default:** unset (an SDL2 audio device is opened).
- **Effect:** Skips opening the audio device. The `sceAudio*Blocking` calls
  fall back to timing-only pacing on a per-channel virtual drain clock: each
  grain is scheduled one playback duration after the previous one (44.1 kHz),
  the way a real device drains while the caller works, so the game runs at the
  same speed, silently.
- **Set it when:** running headless or in automated test runs.

### `PSPRECOMP_RTC_FIXED`
- **Read in:** `runtime/src/hle/psp_hle_utility.cpp` (through
  `runtime/src/psp_rtc.cpp`, which owns the parsing and the fixed clock).
- **Type:** env (`YYYY-MM-DDTHH:MM:SS`, local civil time; a space instead of
  the `T` also parses). An unparsable value warns on stderr and is ignored.
- **Default:** unset — the host clock is reported, as on a real PSP.
- **Effect:** Pins the guest-visible wall clock to a fixed base that keeps
  advancing with real time. `sceRtcGetCurrentClockLocalTime`,
  `sceRtcGetAccumulativeTime`, `sceKernelLibcTime` and
  `sceKernelLibcGettimeofday` all read from it. Patapon reads the clock once
  at boot and the hideout sky/lighting follows that value, so identical
  scripted tours taken at different host hours differ far past the visual
  regression gate: 18:33 vs 20:41 local (11 Oct) differ up to 54.8 % struct
  on the mission-prep captures. Pinned to the reference capture's boot time,
  the same tour is reproducible at any host time (runtime-vs-runtime
  comparison: PASS, all 21 gated points <= 0.15 % struct against the
  reference; two runs with the same value, 0.00 % struct).
- **Set it when:** running the scripted graphics tour and captures must not
  depend on the host clock. Normal play should leave it unset.

### `PSPRECOMP_AUDIO_DUMP`
- **Read in:** `runtime/src/psp_audio_out.cpp`
- **Type:** env (path string).
- **Default:** unset.
- **Effect:** Also writes the mixed device output to a 44.1 kHz stereo s16
  WAV file at the given path (header sizes patched at exit). Requires an
  audio device.
- **Set it when:** verifying audio output without listening to it.

### `PSPRECOMP_TEX_WATCH`
- **Read in:** `runtime/src/psp_ge_texture.cpp`, `runtime/src/psp_ge_draw.cpp`
- **Type:** env (hex guest address).
- **Default:** unset.
- **Effect:** For draws that bind a texture at this address, logs `[TEX-WATCH]`
  lines (one per 60 binds): texture size/format, TEXFUNC (raw), blend and
  alpha-test state, vertex type, the first vertex (position, UV, color) and
  its raw bytes.
- **Set it when:** a texture (e.g. a movie frame from `sceMpegAvcCsc`) is
  written but never appears on screen.

### `PSPRECOMP_TEX_FILTER`
- **Read in:** `runtime/src/psp_ge_texture.cpp`
- **Type:** env (string: `auto`, `nearest`, `linear`).
- **Default:** `auto`.
- **Effect:** `auto` follows the game's `TEXFILTER` bits (bit 0 minify, bit 8
  magnify). `nearest` forces both filters off, `linear` forces both on; the
  `TEXWRAP` (U/V clamp vs repeat) choice is never overridden. An unrecognised
  value is treated as `auto`.
- **Set it when:** comparing against old nearest-only screenshots or PPSSPP's
  forced filter modes.

### `PSPRECOMP_TEX_HASH_BACKOFF`
- **Read in:** `runtime/src/psp_ge_texture.cpp` (`tex_hash_backoff_window`,
  applied in `ge_texture_init`); policy in `runtime/include/psp_ge_texmemo.h`.
- **Type:** env (integer, display lists).
- **Default:** `1` (verify every list, as PPSSPP rehashes once per sync domain).
- **Effect:** cap of the content-hash memo's reuse window. A texture whose
  hash verified unchanged is reused without re-reading guest memory for a
  window that doubles per stable check (1, 2, 4, ... up to this cap); any
  hash mismatch restarts it at one list (what changes every frame keeps
  being hashed every frame), as does a GE block transfer. `1` verifies every
  list (backoff off); `0` or a non-power-of-two value rounds down; values
  above 65536 clamp.
- **Set it when:** a slow host needs the hash cost back (with XXH3 it is
  ~0.02 ms/frame in the hideout). There is no guest write tracking, so a
  window > 1 can draw a stale texture for up to that many lists after the CPU
  rewrites it without a GE transfer.

### `PSPRECOMP_BUTTON_ICONS`
- **Read in:** `runtime/src/psp_prompt_patcher.cpp` (`psp_prompts_register_atlas`).
- **Type:** env, `gray` | `color` | `off`.
- **Default:** `gray`.
- **Effect:** PC button icons in the game's help bars (last input device), in the atlas gray or in
  colour; `off` keeps the PSP icons. Set from the launcher's "Iconos de botones" option.

### `PSPRECOMP_BAR_TEXT`
- **Read in:** `runtime/src/psp_prompt_patcher.cpp`.
- **Type:** env, `off` (anything else = on).
- **Default:** on.
- **Effect:** the help-bar labels are redrawn sharp with the embedded bar font (a word table per
  language, `games/patapon3/runtime/p3_prompt_labels.cpp`); `off` keeps the game's labels. Emergency
  switch, not in the launcher. With `PSPRECOMP_BUTTON_ICONS=off` too, nothing is patched.

### `PSPRECOMP_PROMPTS_SIG_LOG`
- **Read in:** `runtime/src/psp_prompt_patcher.cpp`.
- **Type:** env, `1`.
- **Effect:** logs the atlas and label signatures of every candidate help-bar texture (`[PROMPTS]
  candidate ...`), to add a new language or release.

### `PSPRECOMP_TEXTURES_BUDGET_MB`
- **Read in:** `runtime/src/psp_texrep.cpp` (`ge_texrep_init`).
- **Type:** env (integer, megabytes).
- **Default:** `256`.
- **Effect:** byte budget for replacement texture memory with
  `PSPRECOMP_TEXTURES`: the decoded-PNG cache and the uploaded HD GL textures
  are each LRU-capped to it (least recently used freed first). The GL trim
  only considers *cold* textures, so the budget is a target rather than a
  hard cap when the scene's working set is larger (see
  `PSPRECOMP_TEX_HOT_FRAMES`). An uploaded PNG buffer is dropped immediately
  (re-decoded from disk if its GL texture is evicted). Values <= 0 are
  ignored. The renderer's GL texture cache is budgeted by it as well, so even
  without a pack a very small value evicts original textures (re-decoded on
  demand).
- **Set it when:** running several packed instances on one machine (RAM) or
  testing eviction behaviour.

### `PSPRECOMP_TEX_HOT_FRAMES`
- **Read in:** `runtime/src/psp_ge_texture.cpp` (`ge_texture_init`).
- **Type:** env (integer, display lists — one per frame in practice).
- **Default:** `300`.
- **Effect:** "in use" window of the GL texture budget trim: a texture bound
  within this many lists is never evicted, even while the budget is exceeded.
  Needed because the HD pack's working set can dwarf the budget (missions
  decode ~1 GB of 4096x4096 atlases): a hard LRU below it evicted textures
  in use, and each eviction re-decoded the pack PNG while the original was
  drawn — sustained HD flicker (measured ~50 re-decodes/s). `0` = pure LRU
  (the old behaviour); values above 100000 clamp; a value with no digits
  leaves the default.
- **Set it when:** forcing the budget hard at the price of flicker (`0`), or
  tuning how long a texture is considered live.

### `PSPRECOMP_WINDOW_SCALE`
- **Read in:** `runtime/src/psp_event_loop.cpp` (`psp_runtime_init_sdl`).
- **Type:** env (integer 1..8).
- **Default:** `2` (960x544 window).
- **Effect:** initial window size = 480x272 times this value. The window is
  resizable (minimum 480x272); out-of-range values fall back to 2.
- **Set it when:** you want a different starting window size.

### `PSPRECOMP_FULLSCREEN`
- **Read in:** `runtime/src/psp_event_loop.cpp` (`psp_runtime_init_sdl`).
- **Type:** env (`1` = on).
- **Default:** off.
- **Effect:** starts in borderless desktop fullscreen. F11 or Alt+Enter toggles
  fullscreen at any time.
- **Set it when:** you always play fullscreen.

### `PSPRECOMP_CONTROLS`
- **Read in:** `runtime/src/psp_event_loop.cpp` (`load_controls`, once SDL is up); the
  reader `runtime/src/psp_settings.cpp` builds it from `settings.ini [controls]`, and
  `runtime/src/psp_controls.cpp` parses and validates it (contract:
  [`docs/contracts/settings-v1.md`](contracts/settings-v1.md)).
- **Type:** env, `action=key1,key2|pad;...` with SDL key and GameController button
  names (`cross=K,Return|y`). Empty keys / pad = unbound.
- **Default:** unset: the fixed mapping from before the `[controls]` section existed
  (arrows D-pad, X / Return cross, Z circle, A square, S triangle, Q L, W R, Space
  start, V / Tab select; pad A / B / X / Y, shoulders, D-pad, start and back).
- **Effect:** maps keyboard keys and gamepad buttons to PSP buttons. Unknown names,
  reserved keys (F1, F11, Alt+Return, pad `guide`) and a third key fall back to that
  action's default with a `[CONTROLS] <action>: ...` line; a key or button bound to two
  actions keeps the first action (contract order) and logs the drop. The effective map
  is logged once as `[CONTROLS] defaults: action=keys|pad;...` (or `[CONTROLS] ...`
  when the variable is set). Fixed: left stick analog, LT / RT = L / R, wheel = D-pad.
- **Set it when:** remapping keys or pad buttons; normally the launcher writes
  `settings.ini [controls]` instead.

### `PSPRECOMP_OSK_SKIN`
- **Read in:** `runtime/src/psp_settings.cpp` resolves `[display] osk_skin` to this path;
  the on-screen keyboard overlay (`runtime/src/psp_osk_overlay.cpp`) consumes it.
- **Type:** env (path to a skin JSON, `<ini folder>/skins/osk/<name>.json`).
- **Default:** unset: the built-in Deck skin.
- **Effect:** selects the on-screen keyboard skin (`deck`, `light`, `patapon`, or any
  `skins/osk/*.json`). A missing or unreadable file uses the built-in Deck skin and logs
  an `[OSK]` line.
- **Set it when:** trying a skin without editing `settings.ini`.

### `PSPRECOMP_PRESENT_FILTER`
- **Read in:** `runtime/src/psp_ge_draw.cpp` (`present_filter`).
- **Type:** env (`nearest`; anything else = linear).
- **Default:** linear.
- **Effect:** filter of the window blit that scales the 480x272 image
  (letterboxed to the PSP aspect ratio). `nearest` keeps hard pixel edges.
  Screenshots (`S`, `PSPRECOMP_SCREENSHOT`) are always the unscaled 480x272 FBO.
- **Set it when:** you prefer sharp pixels or compare against nearest captures.

### `PSPRECOMP_SAVEDATA`
- **Read in:** `runtime/src/hle/psp_savedata.cpp` (`default_root()`; used by the
  `sceUtilitySavedata*` handlers in `runtime/src/hle/psp_hle_utility.cpp`)
- **Type:** env (path string).
- **Default:** `SAVEDATA` under the current working directory.
- **Effect:** Host directory the savedata utility reads and writes. Each save
  is a `<gameName><saveName>/` subdirectory holding the game's data file
  (plaintext, not PSP-encrypted), a real `PARAM.SFO`, and `ICON0.PNG` /
  `ICON1.PMF` / `PIC1.PNG` / `SND0.AT3` when the game supplies them. Created
  on first save. An empty value is treated as unset.
- **Set it when:** keeping saves outside the working directory, or giving a
  test run its own throw-away save set.

### `PSPRECOMP_SAVE_SLOT`

Save name (the part after the game id, e.g. `_DATA04`) that `LISTLOAD` loads when it is in the
game's list and exists, instead of the newest save. Patapon 3's Continue uses `LISTLOAD`; the
launcher's Saves screen sets this variable. Unset or missing save: the newest, as before. Logs
`[SAVEDATA] LISTLOAD picks _DATA04 (PSPRECOMP_SAVE_SLOT)`.

### `PSPRECOMP_STRICT`
- **Read in:** `runtime/src/psp_dispatch.cpp`, `runtime/src/main.cpp`,
  `runtime/src/hle/psp_hle_dispatch.cpp`
- **Type:** env (`=1` to enable).
- **Default:** off.
- **Effect:** On any `RECOMP_LOOKUP` miss, prints the address and calls
  `std::abort()` instead of returning the noop stub. Boot also probes a known
  invalid address to exercise the path. The HLE dispatch path honors it too.
- **Set it when:** you want a missing function to crash loudly (debugging) rather
  than silently no-op.

### `PSPRECOMP_GE_TEST`
- **Read in:** `runtime/src/main.cpp`
- **Type:** env (`=1` / first char `'1'`).
- **Default:** off.
- **Effect:** Runs the GE renderer self-test after GL init and reports PASS/FAIL,
  then continues booting.
- **Set it when:** verifying the renderer brings up independently of game assets.

### `PSPRECOMP_GE_TEST_ONLY`
- **Read in:** `runtime/src/main.cpp`; also referenced by Patapon hooks and the
  per-game gatekeeper scripts.
- **Type:** env (`=1` / first char `'1'`). Implies `PSPRECOMP_GE_TEST`.
- **Default:** off.
- **Effect:** Runs the GE self-test, captures a frame (`frame.tga`), and exits
  `0` before the game thread starts.
- **Set it when:** capturing a renderer-only reference frame in CI/headless runs.

### `PSPRECOMP_SCREENSHOT`
- **Read in:** `runtime/src/psp_ge_draw.cpp`
- **Type:** env (path string; any non-empty value).
- **Default:** unset.
- **Effect:** Sets the file path the draw layer writes its captured screenshot
  to.
- **Set it when:** you want the captured frame written somewhere specific.

### `PSPRECOMP_PRESENT_STALE_MS`
- **Read in:** `runtime/src/psp_ge_draw.cpp` (declared in
  `runtime/include/psp_ge_draw.h`)
- **Type:** env (integer milliseconds).
- **Default:** `100`.
- **Effect:** Liveness-present budget. If content is dirty and no real
  page-flip happened within this many milliseconds, the front buffer is
  presented once (a no-flip safety net).
- **Set it when:** tuning the no-flip present heartbeat for a title that flips
  rarely.

### `PSPRECOMP_GEOM_SELFTEST`
- **Read in:** `runtime/src/psp_ge_draw.cpp`
- **Type:** env (any non-empty value).
- **Default:** off.
- **Effect:** Synthesizes exactly one real type-4 (TRIANGLE_STRIP, non-clear,
  non-sprite) draw through the real counter + sentinel path, for the geometry
  verification harness (`scripts/verify_geometry.sh`).
- **Set it when:** running the geometry verification harness.

### `PSPRECOMP_RUN_CTORS`
- **Read in:** `games/patapon/runtime/hooks_main.cpp` (the Patapon
  `on_boot_context` hook; referenced as opt-in in `runtime/src/main.cpp`).
- **Type:** env (presence — any value).
- **Default:** off.
- **Effect:** Runs the emitted `psp_call_constructors()` walk
  (`init_array.cpp`) at boot. PSP OS does not normally process `.init_array`
  (the game CRT does); this is an opt-in experiment in the Patapon module.
- **Set it when:** investigating constructor-walk-dependent initialization in
  Patapon.

### `PSPRECOMP_NO_CPU_LOCK`
- **Read in:** `runtime/src/psp_cpu.cpp` (`enabled()`, first call).
- **Type:** env (`=1` to disable the lock; first character `'1'`).
- **Default:** lock on.
- **Effect:** Disables the guest CPU lock. Guest threads then run recompiled
  code free-threaded (the pre-lock races). For A/B comparison only.
- **Set it when:** checking whether a failure depends on the single-core token.

### `PSPRECOMP_OSK_TEXT`
- **Read in:** `runtime/src/hle/psp_hle_utility.cpp` (`osk_use_ui`, and
  `psp_osk::resolve_text` in `runtime/src/hle/psp_osk.h`).
- **Type:** env (UTF-8 string). Empty falls back to `Hero`.
- **Default:** unset: the on-screen keyboard text box is shown.
- **Effect:** When set (scripted/automated runs), no text box is shown: the OSK
  dialog's RUNNING poll completes field 0 at once with this text (UTF-8 converted
  to UTF-16LE, truncated to the field limit, field result `CHANGED`). In the text
  box, Esc and pad B on an empty text answer CANCEL instead (`PSPRECOMP_OSK_CANCEL`). Without a
  window the same fallback is used with `Hero`. Logged as `[HLE] sceUtilityOsk: ...`.
- **Set it when:** driving the game with scripts (tools/to_square.py, flow.py).

### `PSPRECOMP_OSK_CANCEL`
- **Read in:** `runtime/src/hle/psp_hle_utility.cpp` (`osk_scripted_cancel`).
- **Type:** env (`1` on; anything else or unset = off).
- **Default:** off.
- **Effect:** Scripted back-out: no text box is shown and every on-screen keyboard
  is cancelled at once, the same answer the text box gives for Esc or pad B on an
  empty text: `pspUtilityDialogCommon.result` = 1 (CANCEL), field result
  `CANCELLED`, text buffer untouched (`psp_osk::cancel`). Takes precedence over
  `PSPRECOMP_OSK_TEXT`. Logged as `[HLE] sceUtilityOsk: field0 desc="..." cancelled`.
- **Set it when:** checking with a script that a game leaves its keyboard screen
  when the player backs out (Patapon 3 search by online id, beta report #17).

### `PSPRECOMP_OSK_FONT`
- **Read in:** `runtime/src/psp_osk_overlay.cpp` (first time the text box is drawn).
- **Type:** env (path to a .ttf file).
- **Default:** the skin's `font` when set, then `C:/Windows/Fonts/segoeui.ttf`, `arial.ttf` (and common macOS/Linux paths).
- **Effect:** Font of the on-screen keyboard text box. Logged as `[OSK] font <path>`;
  `[OSK] no font found` means the box draws without text. Glyphs the main font lacks (⌫, ⇧,
  Ⓐ Ⓑ Ⓧ Ⓨ) are drawn with a symbol fallback font (`seguisym.ttf`, else DejaVu Sans) when one exists.

### `PSPRECOMP_OSK_DEMO`
- **Read in:** `runtime/src/psp_event_loop.cpp` (main loop).
- **Type:** env (`=1` to enable).
- **Default:** off.
- **Effect:** Two seconds after boot, opens a demo 16-character keyboard request ("Demo") over
  whatever is on screen, so the skins can be captured without playing. Logs
  `[OSK] demo keyboard open` and, when accepted, `[OSK] demo text: <text>`.
- **Set it when:** taking screenshots of the on-screen keyboard skins.

### `PSPRECOMP_ADHOC_SERVER`, `PSPRECOMP_ADHOC_NICK`, `PSPRECOMP_ADHOC_MAC`, `PSPRECOMP_ADHOC_PORT_OFFSET`, `PSPRECOMP_ADHOC_BIND`, `PSPRECOMP_ADHOC_MAC_FILE`, `PSPRECOMP_ADHOC_MACHINE_ID`
- **Read in:** `runtime/src/net/adhoc_config.cpp` (`adhoc_config_load`), once, from
  `psp_net_config()` in `runtime/src/hle/psp_hle_net.cpp` (first sceWlan/sceNetAdhocctl call).
- **Type:** env. `SERVER` = `host` or `host:port`; `MAC` = `aa:bb:cc:dd:ee:ff`; `PORT_OFFSET`
  = 0..65535; `BIND` = local IPv4 address; `MAC_FILE` = path; `MACHINE_ID` = any string.
- **Default:** server `127.0.0.1:27312` (a local PRO ad hoc server, e.g. PPSSPP's built-in one or
  `tools/adhoc_server.py`), nickname `Patapon`, port offset 10000 (PPSSPP's), no bind address,
  MAC read from `psprecomp_adhoc_mac.txt` in the working directory or generated once and written
  there (first byte's two low bits clear, like PPSSPP). The file is one line
  `aa:bb:cc:dd:ee:ff <hash16>`, the hash being FNV-1a 64 of the host's machine id (Windows
  `MachineGuid`, else the computer name; POSIX `/etc/machine-id`, else the host name,
  `runtime/src/psp_machine_id.cpp`). A file without the hash (older runtimes) or with another
  machine's hash is replaced once by a new MAC (`[ADHOC] new MAC for this machine ...`), so a game
  folder copied to another PC never shares the console identity. `MACHINE_ID` replaces the host's
  id (tests: two "machines" on one PC). `MAC` still wins over the file.
- **Effect:** Ad hoc multiplayer identity and server. The game sees the MAC through
  `sceWlanGetEtherAddr`; `sceNetAdhocctlInit` logs in to the server with MAC, nickname and the
  game's product code. Patapon 3 tells players apart by the OpenPSID built from this MAC, in ad hoc
  and in infrastructure mode, so two consoles with one MAC are both "player 1"; the infrastructure
  server refuses a second member with the same MAC in one room. Logged as `[ADHOC] config: ...`
  (with the machine hash, never the raw id), `[ADHOC] logged in ...`.
- **Set it when:** running two instances on one PC (each needs its own `MAC`, `BIND` such as
  127.0.0.1 / 127.0.0.2, and `PSPRECOMP_DEBUG_PORT`), or playing through another server.

### `PSPRECOMP_ADHOC_RELAY`, `PSPRECOMP_ADHOC_RELAY_PORT`
- **Read in:** `runtime/src/net/adhoc_config.cpp` (`adhoc_config_load`); used by
  `psp_net_wire_pdp()` (`runtime/src/hle/psp_hle_net.cpp`) for `pdp::Sockets` / `ptp::Sockets`.
- **Type:** env. `RELAY` = `1`/`on`/`true`/`yes`; `RELAY_PORT` = 1..65535.
- **Default:** off (peer to peer UDP/TCP, PPSSPP's default data mode); relay port 27313.
- **Effect:** PDP and PTP traffic goes through the aemu_postoffice relay on the ad hoc server host
  (PPSSPP's "server-provided packet relay") instead of peer to peer: one TCP session per PSP socket
  (`net/relay_protocol`, `net/relay_conn`). Needed to play with PPSSPP players on servers whose data
  mode is the relay (Paragate/patapon.net) and behind NAT. `tools/postoffice_server.py` is a local
  relay for tests. Logged as `[ADHOC] relay: ...` on connection problems.
- **Set it when:** the server uses the relay; every player of the session must use the same mode.

### `PSPRECOMP_INFRA_SERVER`
- **Read in:** `runtime/src/net/infra_client.cpp` (`infra::client()`, `parse_address`), once, the first time the
  game reaches infrastructure mode (netconf dialog with an infrastructure action, or a Matching2 call).
- **Type:** env. `host[:port]` or `tcp://host[:port]` (raw TCP, port 27320 by default),
  `ws://host[:port][/path]` (80) or `wss://host[:port][/path]` (443) (WebSocket through WinHTTP, Windows only;
  path `/infra` by default), or `off`.
- **Default:** `wss://online.montick.app/infra` (the public server behind a Cloudflare Tunnel).
- **Effect:** The server of Patapon 3's "Modo Infraestructura" (`tools/infra_server.py`): rooms, search,
  join/leave, signaling and the relay of the game's P2P sockets. Nothing connects until the player picks
  infrastructure mode; with `off` the netconf dialog aborts infrastructure actions as before. A malformed value
  is logged and means `off`. Logged as `[INFRA] connected to ... virtual IP 10.80.0.n`, `[INFRA] connection lost: ...`,
  `[NP2] 0x0106 req N ok`, `[NP2] room event 0x1101 ...`. At the end of a session (and on a lost connection) one
  `[INFRA] link: N pings, p50 X ms, p95 Y ms, max Z ms, S slow, longest gap G ms` line gives the relay link's
  numbers; the same numbers go to the server as `link_stats` every minute and before a disconnect (no address).
- **Set it when:** testing with a local server (`python -I tools/infra_server.py`, then `tcp://127.0.0.1` or
  `ws://127.0.0.1:27321/infra`), or playing on another server.

### `PSPRECOMP_INFRA_INVITE`
- **Read in:** `runtime/src/hle/psp_hle_np_matching2.cpp` (`ensure_connected`, through `infra::login_fields`), at
  each infrastructure login.
- **Type:** env (invite code, e.g. `P3-K7QF-2MXD`; case, spaces and dashes do not matter).
- **Default:** unset — the login carries no invite.
- **Effect:** The beta invite code sent in the login (`"invite"`), next to `"proto": 2`. A server started with
  `--beta` refuses a login without a valid code; the refusal is logged by name, e.g.
  `[INFRA] login to ... failed: INVITE_UNKNOWN {...}` (also `INVITE_DISABLED`, `BETA_CLOSED`, `OUTDATED`,
  `INVITE_IN_USE`). A server without `--beta` ignores it.
- **Set it when:** the launcher sets it from `settings.ini` `[online] invite`; by hand for a test instance
  against a gated server.

### `PSPRECOMP_INFRA_STATS`
- **Read in:** `runtime/src/net/infra_stats.cpp` (`infra::stats::enabled()`), once, at the first infrastructure
  datagram or stream segment.
- **Type:** env (file path).
- **Default:** off.
- **Effect:** Writes a CSV row `t_us,ev,kind,vip,src,dst,len,hash,extra_us` for every game datagram (kind 2) and
  stream segment (kind 3) the client sends (`tx_call` before the send, `tx_done` after it), receives (`rx`),
  hands to the game (`pop`, `extra_us` = time it waited in the queue) or drops (`drop`). `t_us` is steady_clock
  microseconds, one clock for every process of the PC, and `hash` is FNV-1a 32 of the payload, so
  `tools/infra_lagjoin.py` can match the logs of two instances. With `PSPRECOMP_FRAMELOG` the instance also logs
  `[FRAMELOG] start steady_us=N`, which places the frame log on the same clock. A path that cannot be created is
  logged (`[INFRA] stats: cannot open ...`) and leaves it off.
- **Set it when:** measuring online lag (`build/infra/two.py` with `LAG_DIR`).

### `PSPRECOMP_INFRA_TRACE`
- **Read in:** `runtime/src/hle/psp_hle_np.cpp`, `psp_hle_np_matching2.cpp`, `psp_hle_inet.cpp`.
- **Type:** env (`=1` to enable).
- **Default:** off.
- **Effect:** Logs every sceNp*/sceNpMatching2 call with its arguments, the Matching2 requests as JSON, every
  sceNetInet send/receive (sizes and peers), socket options, and on `sceNetInetClose` the guest callers found on
  the stack. Noisy (several lines per frame in a mission).
- **Set it when:** debugging infrastructure mode.

### `PSPRECOMP_DEBUG_PORT`
- **Read in:** `runtime/src/main.cpp` (debug socket start).
- **Type:** env (TCP port).
- **Default:** 9999.
- **Effect:** Port of the debug socket (DEBUGGING.md §6). A second instance needs another port.

---

## Scheduler (#66)

### `PSPRECOMP_PREEMPT`
- **Read in:** `runtime/src/psp_scheduler.cpp` (declared in
  `runtime/include/psp_scheduler.h`); the emitter
  (`crates/psp-emitter/src/cpp_generator.rs`,
  `crates/psp-emitter/src/function.rs`) emits the back-edge preemption point
  unconditionally and documents the flag; the harness
  `scripts/verify_determinism.sh` passes it through.
- **Type:** env (exactly `1`).
- **Default:** off.
- **Effect:** Default-off, the emitted loop back-edge `sched_preempt(ctx)` call
  only reloads `ctx->preempt_budget` and returns — guest behavior is identical
  to the pre-#66 scheduler. With `=1` it additionally takes a fair cooperative
  yield so a syscall-free busy-poll reaches a reschedule point. Read once and
  cached.
- **Set it when:** evaluating the preemptive-yield path (e.g. the `.hack`
  freeze-rate harness). Leave unset for the Patapon-identical baseline.

---

## Build options (CMake)

Passed at configure time: `cmake -B <dir> -S runtime -D<NAME>=<VALUE>`.

### `PSPRECOMP_OUTPUT_DIR`
- **Defined in:** `runtime/CMakeLists.txt`
- **Type:** cmake (`PATH`, cache variable).
- **Default:** `${CMAKE_CURRENT_SOURCE_DIR}/../output` (the in-repo `output/`).
- **Effect:** The recompiler output directory the runtime builds against. Must
  contain a generated `CMakeLists.txt` and define the `psp::recomp` alias target,
  or configure fails with the regenerate hint.
- **Set it when:** building the runtime against a different game's output, e.g.
  `-DPSPRECOMP_OUTPUT_DIR=/abs/path/hack_output`.

### `PSPRECOMP_GAME`
- **Defined in:** `runtime/CMakeLists.txt` (also referenced by
  `runtime/tools/purity_gate.sh`, `runtime/src/psp_game_default.cpp`,
  `runtime/include/psp_game_module.h`, and every `games/<id>/runtime/*.cpp`
  via the build).
- **Type:** cmake (`STRING`, cache variable).
- **Default:** `patapon`.
- **Effect:** Selects the `games/<id>/runtime/*.cpp` module compiled into the
  runtime (exactly one strong `psp_game_module()` definition per build).
  `none` (or empty) compiles `src/psp_game_default.cpp` — a pure generic build
  with zero game hooks. An unknown id with no sources is a fatal error.
- **Set it when:** building the generic runtime
  (`-DPSPRECOMP_GAME=none`) or a non-default game module.

### `PSPRECOMP_ALLOW_STALE`
- **Defined in:** `runtime/CMakeLists.txt` (`option()`).
- **Type:** cmake (`option`, `ON`/`OFF`) — see also the pipeline-side env use
  above.
- **Default:** `OFF`.
- **Effect:** Turns the configure-time output-staleness fingerprint mismatch
  into a warning instead of a fatal error.
- **Set it when:** intentionally configuring against a stale `output/`.

### `PSPRECOMP_BETA_UNTIL`
- **Defined in:** `runtime/CMakeLists.txt` (compile definition of `runtime/src/psp_beta.cpp`).
- **Type:** cmake (`STRING`, cache variable), ISO UTC `YYYY-MM-DDTHH:MM:SSZ`; any other form fails configure.
- **Default:** empty — not a beta build, never expires.
- **Effect:** A beta build of the online mode: after that instant (UTC) the exe shows "La beta de Patapon 3
  terminó..." and exits with code 12 before opening a window (the launcher shows its "the beta ended" screen).
  While it plays, the log says `[RT] beta build: plays until <date>`.
- **Set it when:** building the beta package (`-DPSPRECOMP_BETA_UNTIL=2026-10-26T05:59:59Z` for beta 1).

> Internal CMake locals (`PSPRECOMP_REGEN_CMD`, `PSPRECOMP_FP_RESULT`,
> `PSPRECOMP_FP_OUTPUT`, `PSPRECOMP_FP_ERROR`) are not user-settable; they hold
> the regenerate-command string and the fingerprint-check sub-process results.

---

## Debug and trace flags (advanced)

Everything below is for investigation only. None is needed for a normal
recompile-build-run cycle. Trace flags exist to gate logging that can produce
gigabytes per minute; probe flags add read-only diagnostics; the remaining
flags toggle individual experimental fixes/band-aids in the Patapon game module.

### Generic runtime trace / diagnostics

| Flag | Read in | Activation | Effect |
|------|---------|------------|--------|
| `PSPRECOMP_PC_TRACE` | `runtime/src/psp_dispatch.cpp` (declared `crates/psp-emitter/src/cpp_generator.rs`) | `=1` | Logs every generated-function entry with per-address frequency counting; dumps a summary at exit. No-op otherwise. |
| `PSPRECOMP_GE_TRACE` | `runtime/src/psp_ge.cpp` | first char `'1'` | Enables GE command tracing. |
| `PSPRECOMP_HLE_TRACE` | `runtime/src/hle/psp_hle_dispatch.cpp` (declared `runtime/include/hle/psp_hle.h`); used by `scripts/diff_traces.sh` | `=1` | Every HLE call logs thread name + function name to stderr. Used to produce the runtime trace for the PPSSPP differential. |
| `PSPRECOMP_SEMA_TRACE` | `runtime/src/hle/psp_hle_kernel_sema.cpp` | `=1` | Gates the per-call WaitSema/Signal logs (≈1 GB/min during stalls) behind a flag. |
| `PSPRECOMP_OVERLAY_TRACE` | `runtime/src/psp_dispatch.cpp` | `=1` | Logs the first 64 overlay-window lookups and their bank-resolution status. |
| `PSPRECOMP_LOG_VERBOSE` | `runtime/include/psp_log.h` (read once) | `=1` | Restores the per-frame and per-call lines that are off by default: `sceGeListEnQueue`, `sceDisplaySetFrameBuf`, `sceKernelSetEventFlag`, every 500th `[ATRAC] decode`, the per-call async IO lines (`sceIoReadAsync`, `PollAsync`, `WaitAsync`, `SetAsyncCallback`, `Lseek`, `Close`...). Without it an idle hideout logs under 100 lines a minute. |

> `PSPRECOMP_SPLEAK` no longer exists — the shadow-stack sp-leak detector it
> gated was a Patapon-tuned probe deleted in issue #47 Phase 5
> (`runtime/src/psp_dispatch.cpp` retains only the explanatory comment). It is
> listed here only so older notes referencing it are not mistaken for a live
> flag.

### `PSPRECOMP_FUNC_WATCH`
- **Read in:** `runtime/src/psp_dispatch.cpp` (`psp_trace_checkpoint`).
- **Type:** env (comma-separated hex addresses, up to 16).
- **Default:** unset (no watches).
- **Effect:** The first 200 entries of each listed guest function log
  `[FUNC-WATCH] 0xADDR from 0xCALLER; this thread's path:` plus up to 16
  addresses from that thread's entry ring.
- **Set it when:** you need the caller chain into a known guest function
  without a full `PSPRECOMP_PC_TRACE`.

### `PSPRECOMP_FUNC_CENSUS`
- **Read in:** `runtime/src/psp_dispatch.cpp` (`psp_trace_checkpoint`, `psp_dispatch_census_dump`).
- **Type:** env (`=1`).
- **Default:** off (no counters allocated).
- **Effect:** Counts the entries of every guest function in [0x08800000, 0x09000000) (main image and
  overlay window, 8 MB of counters). The debug socket `C <path>` writes `<addr> <entries>` for every
  function entered since the last `C` and resets the counts.
- **Set it when:** looking for the code behind something on screen: take one window with it and one
  without and diff them (`build/tools/wcensus.py`), or compare call rates at 30 and 60 fps.

### `PSPRECOMP_FUNC_ARGS`
- **Read in:** `runtime/src/psp_dispatch.cpp` (`psp_func_args_install`, called
  from `runtime/src/main.cpp`).
- **Type:** env (comma-separated hex addresses, up to 8).
- **Default:** unset.
- **Effect:** Wraps those dispatch-table entries at boot. The first 400 calls
  through `RECOMP_LOOKUP` log `[FUNC-ARGS-IN]` (a0–a3, ra) on entry and
  `[FUNC-ARGS]` with `v0` on return. Direct C++ calls bypass the wrapper. An
  address that resolves to the miss stub at boot (overlay window) is skipped
  with `[FUNC-ARGS] 0xADDR not resolvable at boot (overlay?), skipped`.
- **Set it when:** logging arguments of a main-module function. Overlay code
  uses `PSPRECOMP_OVL_ARGS`.

### `PSPRECOMP_FUNC_ARGS_DUMP`
- **Read in:** `runtime/src/psp_dispatch.cpp` (inside the `FUNC_ARGS` return
  log).
- **Type:** env (byte count; `strtoul` base 0, then masked with `~3`).
- **Default:** unset (0, no dump).
- **Effect:** After each logged `[FUNC-ARGS]` return, if `v0 != 0`, hex-dumps
  that many bytes of guest memory at `v0` as `[FUNC-ARGS]   ADDR: WORD ...`
  (16 bytes per line). Requires `PSPRECOMP_FUNC_ARGS`.
- **Set it when:** the watched function returns a pointer to a block you need
  to see before the game reuses it.

### `PSPRECOMP_OVL_ARGS`
- **Read in:** `runtime/src/psp_dispatch.cpp` (`ovl_args_intercept`).
- **Type:** env (comma-separated hex addresses, up to 8).
- **Default:** unset.
- **Effect:** On each overlay-bank resolution of a listed address, the first
  200 calls log `[OVL-ARGS-IN]` (a0–a3 and this thread's entry ring), a
  `[BT] ovl-args:` host backtrace, and `[OVL-ARGS-OUT]` with `v0`.
- **Set it when:** the function lives in an overlay window and cannot be
  wrapped at boot.

### `PSPRECOMP_STORE_WATCH`
- **Read in:** `runtime/src/psp_backtrace.cpp` (`g_psp_store_watch`); the hook
  is `psp_mem_write_w` / `MEM_W_WRITE` in the generated `recomp.h`
  (`crates/psp-emitter/src/cpp_generator.rs`).
- **Type:** env (hex word value). `0` or unset is off.
- **Default:** off.
- **Effect:** Every 32-bit guest store of that exact value calls
  `psp_store_watch_hit`. Up to 5000 hits print
  `[STORE-WATCH] store of 0xVALUE to 0xADDR`. A `[BT] store-watch:` line is
  added only for the word at `+4` of a 16-byte-aligned block, and only for
  the first 400 hits. Filtered further by `PSPRECOMP_STORE_WATCH_RANGE`.
- **Set it when:** hunting who stores a known word (a vtable pointer, a
  sentinel) without a debugger watchpoint.

### `PSPRECOMP_STORE_WATCH_RANGE`
- **Read in:** `runtime/src/psp_backtrace.cpp` (`psp_store_watch_hit`).
- **Type:** env (`<lo>-<hi>`, both hex).
- **Default:** unset, which is `[0, 0xFFFFFFFF)` — no address filter.
- **Effect:** `psp_store_watch_hit` returns immediately unless the store
  address is in `[lo, hi)`. Requires `PSPRECOMP_STORE_WATCH`.
- **Set it when:** the watched value is stored often and only one object
  range matters.

### `PSPRECOMP_PRIM_LOG`
- **Read in:** `runtime/src/psp_ge_draw.cpp`.
- **Type:** env (presence — any value, including empty).
- **Default:** unset.
- **Effect:** The draw path always logs the first 50 PRIMs and the first 5
  non-clear PRIMs as `[DRAW_PRIM]`. With this set it also logs every 25th
  PRIM. The comment in the draw path: unrestricted periodic logging grew a
  log to 1 GB in a 3 h run.
- **Set it when:** you need a PRIM heartbeat and can afford the log size.

### `PSPRECOMP_VTX_WATCH`
- **Read in:** `runtime/src/psp_ge_draw.cpp`.
- **Type:** env (hex vertex type, compared to `state.vertex_type`).
- **Default:** unset.
- **Effect:** The first 12 draws whose vertex type equals this value log
  `[VTX-WATCH] prim= count= stride= vaddr=` and, for up to 6 vertices, the
  raw bytes plus decoded position and RGBA.
- **Set it when:** a vertex format is decoding to the wrong positions.

### Patapon 3 module — enhancements

Compiled only under `-DPSPRECOMP_GAME=patapon3`. **Game-specific: patapon3.**

### `PSPRECOMP_P3_ANIM_SMOOTH`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`.
- **Type:** env (`1` on, `0` off; unset or empty = follow the `60fps` patch set).
  **Game-specific: patapon3.**
- **Default:** on when `PSPRECOMP_PATCHES` enables `60fps`, off otherwise.
- **Effect:** Character poses are blended from the two whole frames around the
  motion player's clock (+0x68). While the motion tick `0x08878A18` or draw
  `0x08878B18` runs, each GMO evaluation `0x089DBD2C` runs twice on the
  original whole-frame path — the playheads the player set (`0x089DAF74`)
  moved to the next frame (loop start past the end; a one-shot at its end
  holds), then at the game's own frame — and the bone blocks (0x100 bytes per
  bone: translation +0x50, rotation quaternion +0x60, scale +0x70) are blended
  by the clock's fraction (`runtime/include/psp_anim_blend.h`). The model's own
  interpolate flag (model +0x28 bit 4) is not used: `0x089DB1E0` honours it
  for some channels and rounds others, so the Hero lifted off the ground. Bone
  reads `0x08878D38` / `0x08878C8C` of a baked skeleton (model vtable
  `0x08A82BB0`, one bone matrix per whole frame; the Patapons in the barracks
  hang body, eye and weapon on it) blend the bone at the whole frame with the
  next one by the clock's fraction. The clock and frame-keyed events are
  untouched. Install prints
  `[P3] smooth animation: poses blended from two whole frames`.
- **Set it when:** `0` to compare against the stepped animation at 60 fps; `1`
  to try it at 30 fps.

### `PSPRECOMP_P3_ANIM_SMOOTH_FADE`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on with `PSPRECOMP_P3_ANIM_SMOOTH`.
- **Effect:** The smooth-animation blend also covers a keyed bone channel that is not a
  transform: the bone alpha (channel 0x9C, packed by `0x089DDD88` as
  `trunc(alpha * 255) << 24 | 0xFFFFFF` at bone +0x4C; the top byte is blended). The
  texture offset (channel 0x9B, +0xC0/+0xC4) stays on whole frames: models flip through
  atlas cells with it (the Patapons' eyes, u 0.125 -> 0.25), and blending it drew half of
  one cell and half of the next. Without it the hideout's place halo and the Hero Gate's
  sparkles (motions keyed at 15 frames/s) faded in steps every fourth vblank at 60 fps
  while their scale moved every vblank. Install prints
  `[P3] smooth animation: alpha blended too (halo, Hero Gate sparkles)`.
- **Set it when:** `0` to compare against the stepped fades (A/B).

### `PSPRECOMP_P3_ANIM_SMOOTH_BAKED`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on with `PSPRECOMP_P3_ANIM_SMOOTH`.
- **Effect:** Baked models (vtable `0x08A82BB0`) draw their own parts (the Patapons'
  body and eye) from one GE list recorded per animation frame (`0x0887A814` ->
  `0x08858CC8` -> called through `0x0883DC8C`). Inside a player's draw, when the clock
  lies between the drawn frame and the next, the two frames' lists are blended
  (`runtime/include/psp_ge_list_blend.h`: world/bone matrix data and material alpha;
  texture offsets stay, they flip atlas cells) into a 1 MB ring at guest `0x0F000000`
  and that copy is called instead. Without it the Patapons' bodies and eyes stepped at
  the key rate (~every second vblank at 60 fps) while their equipment moved every vblank.
  Install prints `[P3] smooth animation: baked frame lists blended too (Patapon bodies and eyes)`.
- **Set it when:** `0` to compare against the stepped bodies (A/B).

### `PSPRECOMP_P3_ANIM_SMOOTH_WORM`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on with `PSPRECOMP_P3_ANIM_SMOOTH`.
- **Effect:** The FEVER worm is drawn by the mission overlay's motion player draw
  `0x08B72338` (overlay id 3, all three variants), outside the main player's scope, so its
  baked parts showed whole frames: at 60 fps each body key for 2 vblanks and each head key
  for 4. The hook opens the smooth scope there (the overlay function is resolved per call
  and the scope opens only when the loaded code is that function); the baked list blend
  then takes the clock of the model it draws (body `+0x68`, head pair `+0xA0`, the part on
  a bone `+0xA8`), and the baked bone getter `0x0887A988` blends the bone that part hangs
  on. The clocks are only read, so the speed does not change. Install prints
  `[P3] smooth animation: FEVER worm motion blended too`.
- **Set it when:** `0` to compare against the stepped worm (A/B).

### `PSPRECOMP_P3_ANIM_SMOOTH_REWARD`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on with `PSPRECOMP_P3_ANIM_SMOOTH`.
- **Effect:** The reward that comes out of a chest in the chest room and its glowing halo
  are drawn by the mission overlay's `0x08B4CB28` (overlay id 3, all three variants),
  outside any smooth scope, so the halo (a 30-frame baked loop whose clock moves 0.25
  frame per vblank at 60 fps) showed each frame for 4 vblanks. The hook opens a scope
  there (same per-call overlay check as the worm) whose own clock is a whole frame, and
  gives the object's two motion-player blocks their clocks (item `+0x78`, halo `+0xF8`);
  the baked list blend then mixes the drawn frame and the next one of the same motion.
  The clocks are only read, so the speed does not change. Install prints `[P3] smooth animation: reward halo blended too`.
- **Set it when:** `0` to compare against the stepped halo (A/B).

### `PSPRECOMP_P3_WEATHER_30HZ`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on when `PSPRECOMP_PATCHES` enables `60fps`; never installed otherwise.
- **Effect:** The weather objects' updates (rain drops `0x089010DC`, wind ribbons
  `0x08902EE0`, `0x08903F40`; a0 = object, a1 = frame time in ticks, 4800/s) move
  every particle by a fixed step per call, so at 60 fps the rain fell and the wind
  blew twice as fast. Each object's update runs at most once per two vblanks and a
  skipped call's frame time goes to its next run (`runtime/include/psp_paced_step.h`).
  Install prints `[P3] 60fps: weather paced to 30 Hz`.
- **Set it when:** `0` to see the old double-speed weather at 60 fps (A/B).

### `PSPRECOMP_P3_WEATHER_SMOOTH`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on with `PSPRECOMP_P3_WEATHER_30HZ`.
- **Effect:** The weather moves every frame at the 30 fps speed instead of every
  second frame. Rain: on a frame whose update was skipped, the draw `0x089016D8`
  shows every drop half a step ahead (pos += velocity / 2,
  `runtime/include/psp_particle_nudge.h`) and the positions go back after it. Wind
  ribbons: `0x08902EE0` is not paced; its ribbon step `0x088FE738` runs every frame
  with the per-call advance along the curve (+40) halved for the call. Install prints
  `[P3] 60fps: smooth rain and wind (drawn every frame at the 30 fps speed)`; with
  `PSPRECOMP_P3_WEATHER_TRACE=1` it also logs rain draws, draws half a step ahead and
  ribbon steps per second.
- **Set it when:** `0` to compare against the plain 30 Hz weather.

### `PSPRECOMP_P3_HIDEOUT_30HZ`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on when `PSPRECOMP_PATCHES` enables `60fps`; never installed otherwise.
- **Effect:** The script objects' update `0x08909C7C` (a0 = script object, a1 = frame
  time in ticks) runs the game's script VM. Scripts count frames (`cmd_waitFrame`
  `0x08838434` takes one off per run) and move props by a fixed step per run (the
  hideout's Silver Hoshipon: read the actor position, add a step, set it), so at 60 fps
  they ran twice as fast. Each script object runs at most once per two vblanks and a
  skipped call's frame time goes to its next run (`runtime/include/psp_paced_step.h`),
  so `cmd_waitTime` still counts real time; a skipped call returns 1 ("keep running").
  Only while the hideout overlay (id 2 at `0x08ABB180`) is loaded: paced mission scripts
  lost the stage, the army and the drum HUD (Arena del Valor froze). Install prints
  `[P3] 60fps: script objects paced to 30 Hz in the hideout only`.
- **Set it when:** `0` to see the old double-speed scripts at 60 fps (A/B).

### `PSPRECOMP_P3_MENU_30HZ`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on when `PSPRECOMP_PATCHES` enables `60fps`; never installed otherwise.
- **Effect:** The DxD releases put `0x08A62E8C` in front of the item grid's list update
  `0x08966754` (Lady Meden's shop, the forge): while Triangle alone is held it reads
  Triangle's held-frame counter (input object +136) and, on every `20 - accel` counts,
  copies held into pressed, so holding Triangle buys or skips repeatedly. The counter
  moves only when the 30 Hz pad update runs, but the check ran every frame and saw each
  count twice: a 120 ms tap bought two items, a 1.5 s hold twelve instead of five. The
  check now runs once per pad update (`runtime/include/psp_once_per_count.h`); on the
  frame in between only the list update runs. The code is checked on every call (the
  vanilla EU disc has none there). Install prints
  `[P3] 60fps: item-list hold repeat (DxD) paced to 30 Hz`.
- **Set it when:** `0` to see the old double buys at 60 fps (A/B).

### `PSPRECOMP_P3_SCRIPT_WAIT_30HZ`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on when `PSPRECOMP_PATCHES` enables `60fps`; never installed otherwise.
- **Effect:** Outside the hideout the script objects run every vblank at 60 fps, and
  `cmd_waitFrame` (`0x08838434`, a1 = the script VM) counts one frame per run, so frame
  waits ended in half the time (mission-start countdown bubbles one per 0.245 s instead
  of 0.50 s, march hints every 2 s instead of 4 s). When a wait has just started (command
  block `vm + [vm+0x360]*0x118`: +0x118 started, +0x120 still running, +0x124 frames
  left), its count is set so it lasts twice as many runs (`runtime/include/psp_frame_wait.h`);
  scripts are never skipped. Not in the hideout while `PSPRECOMP_P3_HIDEOUT_30HZ` paces
  its scripts. `cmd_waitTime` already counts real time and is left alone. Install prints
  `[P3] 60fps: script frame waits last their 30 fps time`.
- **Set it when:** `0` to see the old double-speed mission waits at 60 fps (A/B).

### `PSPRECOMP_P3_HIDEOUT_SMOOTH`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on with `PSPRECOMP_P3_HIDEOUT_30HZ`.
- **Effect:** Props that scripts move are drawn moving every frame at the 30 fps speed
  instead of every second frame. The node matrix builder `0x0888E7FC` (a0 = 3D node,
  position +192/+196/+200) records the position of every node it builds during a script
  run; on the frame after that run, the scene update builds the node's matrix with the
  position half a step ahead (`runtime/include/psp_half_ahead.h`; never across a jump of
  more than 6 units) and the position goes back right after. Install prints
  `[P3] 60fps: smooth hideout animations (scripted props drawn every frame)`.
- **Set it when:** `0` to compare against the plain 30 Hz script motion.

### `PSPRECOMP_P3_HIDEOUT_TRACE`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`=1`).
- **Default:** off.
- **Effect:** With the 60 fps script pacing installed: logs the first 60 events (script
  run / skip, matrix builds of script-moved nodes) with their vblank as `[HIDEOUT]` lines,
  then every ~2 s the matrix builds per second of script-moved nodes, how many were half
  a step ahead, and the largest per-run step seen.
- **Set it when:** checking that scripted props move every frame at 60 fps (a screenshot
  loop is too slow to see 60 Hz motion).

### `PSPRECOMP_P3_FAST_LOAD`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`0` off; anything else or unset = on).
- **Default:** on.
- **Effect:** Tip-screen loads ("CARGANDO": Continue, entering a mission) run at host speed.
  The game's loader takes one file per two or three frames, so instant host reads still took
  ~2.3 s at 59.94 Hz. While the "CARGANDO" icon draw `0x08A18834` runs (it runs on every
  frame of those loads and nowhere else), it requests the runtime display fast-forward
  (`runtime/include/psp_fast_forward.h`): vblank waits return at once with the display clock
  moved to that vblank, presents are limited to one per real 16.7 ms, and thread delays are
  cut to 1 ms (the FileThread polls its queue every 10 ms). It lapses 8 vblanks after the
  icon stops. Never while an ad hoc session is up (`sceNetAdhocctlInit` until
  `sceNetAdhocctlTerm`): peers and network timeouts run in real time, so loads there keep the
  original pace (logs `[ADHOC] display fast-forward off for the session`). Continue: ~2.3 s ->
  0.5-0.8 s. Install prints
  `[P3] fast loading: tip-screen loads skip the vblank waits`; each load ends with
  `[DISPLAY] fast-forward: <n> vblanks in <ms> ms`.
- **Set it when:** `0` to see the original load pace, or to debug anything timed during a
  tip-screen load.

### Patapon 3 module — diagnostics

Compiled only under `-DPSPRECOMP_GAME=patapon3`. **Game-specific: patapon3.**

### `PSPRECOMP_P3_ANIM_TRACE`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`.
- **Type:** env (`=1`; first character `'1'`). **Game-specific: patapon3.**
- **Default:** off.
- **Effect:** Diagnostics only. Wraps the pose cache `0x088785A8` and logs
  each distinct model vtable +0x38 (pose) method once (first 16) as
  `[ANIM] cache=... model=... vtbl=... +0x38=... motion=... frame=...`.
- **Set it when:** looking for the pose path of a model that does not animate
  smoothly.

### `PSPRECOMP_P3_WEATHER_TRACE`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`.
- **Type:** env (`=1`; first character `'1'`). **Game-specific: patapon3.**
- **Default:** off.
- **Effect:** Diagnostics only. Wraps the weather object setters `0x088FDD50`..`0x088FDDDC` and the
  blend start `0x08900678` that the mission script's weather commands use, and logs the first 400
  calls as `[WEATHER] vb=<vblank> set 0x... obj=... f12=... a1=... ra=...`. With
  `PSPRECOMP_P3_WEATHER_SMOOTH` installed it also prints, every ~2 s,
  `[WEATHER] per s: rain draws ... (half a step ahead ...), ribbon steps ...`.
- **Set it when:** checking when and to what the mission script changes the rain and wind.

### `PSPRECOMP_P3_ANIM_SMOOTH_DIAG`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (`interp`; unset = normal).
- **Effect:** With smooth animation on, `interp` uses the earlier smooth mode instead of the
  two-frame blend: the player's fractional clock as the playhead and the model's interpolate
  flag (+0x28 bit 4) set around the evaluation. Prints
  `[P3] smooth animation diag: interpolate-flag path`.
- **Set it when:** comparing the blend against the earlier mode (the Hero rose slightly off the
  ground with it).

### `PSPRECOMP_P3_ANIM_PROBE`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`. **Game-specific: patapon3.**
- **Type:** env (a directory; unset = off). Only with smooth animation on.
- **Effect:** Diagnostics only. When `<dir>/probe.trigger` appears (optional content
  `"<min bones> <skip>"`), the next GMO model evaluated in a motion tick/draw with a fractional
  clock (at least `<min bones>` bones, after skipping `<skip>` such models) is evaluated three
  times: playheads at the whole frame (`a`), whole + 1 (`b`) and the current smooth mode
  (`mid`: the blend, or the interpolate-flag path with `PSPRECOMP_P3_ANIM_SMOOTH_DIAG=interp`). Its bone blocks (0x100 bytes per bone) go to
  `<dir>/probe_<n>_{a,b,mid}.bin`, a header to `<dir>/probe_<n>.txt`; the model keeps `mid`.
  The trigger file is removed when read. `build/tools/anim_probe.py <dir> <n>` compares them.
- **Set it when:** checking whether interpolated poses stay between the two whole-frame poses.

### `PSPRECOMP_P3_GMO_GUARD`
- **Read in:** `games/patapon3/runtime/hooks_main.cpp`.
- **Type:** env (`=1`; first character `'1'`). **Game-specific: patapon3.**
- **Default:** off.
- **Effect:** Diagnostics only (the original functions still run). Wraps the
  GMO display-list relocators `0x08859AC0` (jump/call), `0x08859D2C`
  (vertex) and `0x08859E78` (texture). `a0` is the BASE+address command pair
  being patched; the pair must lie in the loaded model's GE region
  `[a1+0x28, a1+0x28 + a2+0x2C)`. A pair outside it logs `[GMO-GUARD]` (first
  20 reports) and a `[BT] gmo-guard:` host backtrace. Install prints
  `[GMO-GUARD] watching the GMO display-list relocators`.
- **Set it when:** a model load is suspected of walking off its display list
  and rewriting unrelated heap memory.

### Patapon module — band-aid / fix toggles

Compiled only under `-DPSPRECOMP_GAME=patapon`. Behavioral hooks carry REMOVAL
CRITERION comments; the whole band-aid set is disabled by `PSPRECOMP_CLEANROOM`.

| Flag | Read in | Activation | Effect |
|------|---------|------------|--------|
| `PSPRECOMP_CLEANROOM` | `hooks_memory.cpp`, `hooks_main.cpp` | presence | Disables the game module's behavioral band-aids wholesale (the verification gate); also enables some `[CR_*]` diagnostic logging on the cleaned path. |
| `PSPRECOMP_BND_DISABLE` | `hooks_main.cpp` (and per-game gatekeeper scripts) | presence | Bypasses the BND asset layer entirely — routes through the shared stub (A/B aid). |
| `PSPRECOMP_ASSET_NO_STUB` | `hooks_main.cpp`, `asset_bnd.cpp`, `asset_bnd.h` | presence / `=1` | Calls the recompiled original asset-load path instead of the synthetic stub descriptor; also gates the region-buffer / loose-group-absent fixes on. |
| `PSPRECOMP_ASSET_FORCE_MISS` | `hooks_main.cpp`, `asset_bnd.cpp` | `=1` (or presence in hooks) | Forces every asset lookup to report "miss" (synthetic-stub-for-all diagnostic). |
| `PSPRECOMP_ASSET_SIZE_HINT` | `asset_bnd.cpp` | integer | Size returned by the synthetic stub descriptor (default 0). |
| `PSPRECOMP_ASSETPROC_ZERO` | `hooks_main.cpp` | presence | Installs zeroing wrappers around asset-proc functions to test the uninitialized-frame class. |
| `PSPRECOMP_REGION_BUF_FIX` | `hooks_main.cpp` | presence | Enables the region-buffer fix at `0x08863B6C`. |
| `PSPRECOMP_REGION_BUF_FIX_OFF` | `hooks_main.cpp` | presence | Force-disables the region-buffer fix even if `ASSET_NO_STUB`/`REGION_BUF_FIX` would enable it. |
| `PSPRECOMP_LOOSE_REGION_FIX` | `hooks_main.cpp` | presence | Enables the loose-region fix at `0x08861E28`. |
| `PSPRECOMP_LOOSE_REGION_FIX_OFF` | `hooks_main.cpp` | presence | Force-disables the loose-region fix. |
| `PSPRECOMP_LOOSE_REGION_SEED` | `hooks_memory.cpp` | presence | Seeds the loose-region object state path independently of the fix flag. |
| `PSPRECOMP_LOOSE_GROUP_ABSENT_OFF` | `hooks_main.cpp` | presence | Force-disables the loose-group-absent fix at `0x08863980`. |
| `PSPRECOMP_OBJ4_FIX_OFF` | `hooks_main.cpp` | presence | Force-disables the obj+4 region-object fix. |
| `PSPRECOMP_NO_DESC_XLATE` | `hooks_main.cpp` | presence | Skips the BND descriptor address translation. |
| `PSPRECOMP_BND_SHORT_OFF` | `hooks_main.cpp` | presence | Disables the BND short-name slot+0x308 fix. |
| `PSPRECOMP_LISTLOOP_OFF` | `hooks_memory.cpp` | presence | Takes the list-loop diagnostic path (also taken under CLEANROOM). |
| `PSPRECOMP_LK_FIX` | `hooks_main.cpp` | presence | Applies the long-name-key stack fix at `0x0896A6A4`. |
| `PSPRECOMP_LK_ZERO` | `hooks_main.cpp` | presence | Zeros a stack window before the long-name key is built (bisectable with the two flags below). |
| `PSPRECOMP_LK_ZLO` | `hooks_main.cpp` | integer offset | Low offset of the `LK_ZERO` window (default 0). |
| `PSPRECOMP_LK_ZHI` | `hooks_main.cpp` | integer offset | High offset of the `LK_ZERO` window. |
| `PSPRECOMP_LK_KEY` | `hooks_main.cpp` | presence | Logs the first 8 stack-built key strings. |
| `PSPRECOMP_GK_ZERO` | `hooks_main.cpp` | presence | Installs a zeroing wrapper at `0x088623E0`. |
| `PSPRECOMP_FFC_ZERO` | `hooks_main.cpp` | presence | Installs a zeroing wrapper at `0x08861FFC`. |

### Patapon module — read-only probes / traces

All of these are read-only logging hooks (default off) used to instrument a
specific address or object during an investigation.

| Flag | Read in | Activation | Effect |
|------|---------|------------|--------|
| `PSPRECOMP_BND_TRACE` | `asset_bnd.cpp` | `=1` (presence in some sites) | One-shot BND layout/inflate dumps. |
| `PSPRECOMP_BND_CONTENT_CHECK` | `asset_bnd.cpp` | `=1` | Dumps a BND content-integrity check. |
| `PSPRECOMP_PUSH_TRACE` | `hooks_memory.cpp` | presence | Traces list-ctor / push sentinel nodes. |
| `PSPRECOMP_D1_TRACE` | `hooks_memory.cpp` | presence | Logs object state on entry/exit (read-only). |
| `PSPRECOMP_O980_TRACE` | `hooks_main.cpp` | presence | Trace hook around `0x...0980`. |
| `PSPRECOMP_E28_TRACE` | `hooks_main.cpp` | presence | Traces lookups routed through `0x08861E28`. |
| `PSPRECOMP_AEC_TRACE` | `hooks_main.cpp` | presence | Traces `0x08861AEC` to see why the region buffer is NULL. |
| `PSPRECOMP_B6C_TRACE` | `hooks_main.cpp` | presence | Traces size/offset args into `0x08863B6C`. |
| `PSPRECOMP_LK_TRACE` | `hooks_main.cpp` | presence | Traces the long-name-key path at `0x0896A6A4`. |
| `PSPRECOMP_GATE_PROBE` | `hooks_main.cpp` | presence | Read-only probe around the dequeue at `0x088620E8`. |
| `PSPRECOMP_GK_BUF` | `hooks_main.cpp` | presence | Logs argument registers for the GK buffer call. |
| `PSPRECOMP_DIAG07` | `hooks_main.cpp` | presence | a0-relative read diagnostic ("wrong object" vs "zeroed"). |
| `PSPRECOMP_CTX_PROBE` | `hooks_memory.cpp` | presence | Logs context/SP around `0x0885FE90`. |
| `PSPRECOMP_END_PROBE` | `hooks_memory.cpp` | presence | Logs the end-sentinel copy args. |
| `PSPRECOMP_DF0_PROBE` | `hooks_memory.cpp` | presence | Logs args at the DF0 hook. |
| `PSPRECOMP_ITER_PROBE` | `hooks_memory.cpp` | presence | Logs the iterator node pointer each list comparison (taken under CLEANROOM/LISTLOOP_OFF). |
| `PSPRECOMP_FE90_OBJ` | `hooks_memory.cpp` | presence | Object-state probe at `0x0885FE90`. |
