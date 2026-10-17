# Patapon 3 community launcher — Part 1: settings contract + launcher shell (design)

Date: 2026-10-17. Status: approved in conversation (sections 1-5), awaiting written-spec review.
Author: Claude Opus 5.5 with the user (brainstorming, "nativo").

## Goal and context

The user wants the Patapon 3 DxD port to feel like *Patapon 1+2 Replay* and to be something the
community can use: a launcher with a "pre" (main menu + options before playing), graphics /
controls / online settings, several savedata, PPSSPP save conversion, a native or on-screen
keyboard, PC-like controls, and community-made main-menu themes unlocked through in-game
achievements (for example beating Gnar in VS). "Bring your DxD ISO and your saves."

### Decisions taken (conversation 2026-10-17)

- **Distribution: prebuilt game exe + the player's ISO.** The project publishes the compiled
  runtime; the launcher asks for the player's ISO only for game data (part 2). Nothing is
  compiled on the player's PC (no bundled compiler, no "program that builds and runs a program"
  antivirus pattern, one binary that gains reputation). The legal grey zone is the one the
  community recomp projects accept; part 2 can move EBOOT data (not code) out of the exe.
- **Launcher technology: Tauri 2 + React (TSX), TypeScript 7.0.2**, a separate program from the
  game. It writes `settings.ini` and launches the game; themes become web assets.
- **Cross-platform design, Windows first.** Nothing in the contract or the launcher may be
  Windows-only; the Linux build of the game (and AppImage) is its own later part.
- **Art:** a plain theme now, in the final theme format; the real art arrives in part 5.
- **Languages:** Spanish and English, extendable with one JSON file per language.
- **Back to the menu from the game:** a key / button combo with confirmation that quits the game
  with a distinct exit code; the launcher reappears.

### Decomposition (each part gets its own spec -> plan -> implementation)

1. **This spec:** `settings.ini` contract, game-side reader, launcher shell (menu, options, launch,
   back to menu).
2. First run with the player's ISO: pick ISO, detect the DxD version, extract `disc0`, decrypt
   the EBOOT (data).
3. Savedata: import / convert PPSSPP saves (sceChnnlsv mode 5 with the game key, as done for the
   user's main save on 2026-10-16), a "CARGAR"-style save picker.
4. Controls and keyboard: remap keyboard and gamepad, on-screen keyboard with a gamepad, PC-like
   controls.
5. Themes + achievements: community theme format, achievements read from the game, unlocks.
6. Community repo and release: public repo, packaged launcher + game exe, guide, legal notes.
7. (after 6) Linux build of the game, AppImage, Steam Deck testing.

## Part 1 scope

In: the `settings.ini` contract (v1), its reader in the game, the game's "back to menu" combo and
exit codes, the game log path argument, and a launcher with main menu, Options (General,
Graphics, Controls read-only, Online), launch and return handling, es/en strings and a plain
default theme.

Out: ISO handling, saves, editing controls, on-screen keyboard, real art, achievements, release
packaging, Linux. New graphics features (MSAA/SMAA, exclusive fullscreen, arbitrary resolutions)
are not part 1; the options are the ones the runtime already supports.

## 1. Architecture and flow

```
Launcher (Tauri: Rust + React/TSX)                     Game (psprecomp_runtime[.exe], C++)
Open launcher
 ├─ read settings.ini (create with defaults if missing)
 ├─ Main menu: Start · Options · Exit
 ├─ Options: General · Graphics · Controls · Online
 │     └─ saves settings.ini on confirm
 └─ Start ──► hide window, launch the game, wait ───►  read settings.ini -> runtime options
                                                        boot the game (unchanged)
                                                        F1 / Select+Start 1 s: "Back to menu?"
 ◄── game exits (code 0 / 10 / other) ◄────────────    fast exit
 └─ 10: show the main menu again · 0: exit too · other: crash notice + log
```

- The game boot order does not change; the game never shows the launcher UI.
- The launcher reads the gamepad in Rust (`gilrs`) and forwards navigation events to the web UI
  (the Web Gamepad API is unreliable in WebKitGTK on Linux).
- Portable folder, nothing in AppData:
  ```
  Patapon3Recomp/
    launcher(.exe)  game/psprecomp_runtime(.exe) + DLLs  game/game_options.json
    settings.ini  savedata/  disc0/  textures/  themes/  lang/  logs/
  ```
  Paths in the contract are relative to the folder holding `settings.ini`; the game executable
  name depends on the OS.

## 2. `settings.ini` v1 (the contract)

UTF-8 text, `[section]` headers, `key = value` lines, `;` comments. Defaults:

```ini
[meta]
version = 1

[general]
language = es               ; launcher (and in-game dialog) language: es | en

[paths]
disc0 = disc0               ; PSPRECOMP_DISC0
savedata = savedata         ; PSPRECOMP_SAVEDATA

[display]
fullscreen = true           ; PSPRECOMP_FULLSCREEN=1 when true (borderless desktop)
window_scale = 2            ; PSPRECOMP_WINDOW_SCALE, 1..8
render_scale = 4            ; PSPRECOMP_RENDER_SCALE, 1..8
present_filter = linear     ; PSPRECOMP_PRESENT_FILTER: linear | nearest
texture_filter = auto       ; PSPRECOMP_TEX_FILTER: auto | nearest | linear
hd_textures = true          ; PSPRECOMP_TEXTURES=<texture_pack> only when true
texture_pack = textures/INFN00001

[game]                      ; keys declared by game/game_options.json (Patapon 3 below)
fps = 30                    ; 30 | 60  -> 60 adds "60fps" to PSPRECOMP_PATCHES
smooth_animation = true     ; PSPRECOMP_P3_ANIM_SMOOTH=1|0
fast_loading = true         ; PSPRECOMP_P3_FAST_LOAD=1|0

[online]
server = 127.0.0.1          ; PSPRECOMP_ADHOC_SERVER
nickname = Patapon          ; PSPRECOMP_ADHOC_NICK
relay = false               ; PSPRECOMP_ADHOC_RELAY=1 when true
port_offset = 10000         ; PSPRECOMP_ADHOC_PORT_OFFSET
mac_file = adhoc_mac.txt    ; PSPRECOMP_ADHOC_MAC_FILE

[controls]                  ; reserved for part 4; ignored in v1
```

Rules:

- **Precedence:** an environment variable already set > `settings.ini` > the runtime default.
  `jugar.bat`, tours and `rt_regress` keep working unchanged.
- **Application:** the game converts each known key into today's environment variable before
  anything reads one; no existing option code changes.
- **Booleans:** `true|false|1|0|yes|no|on|off` (case-insensitive). Integers in decimal.
- **Invalid values / unknown keys:** never fatal. The game keeps the default and logs
  `[SETTINGS] <section>.<key>: ...`; the launcher validates before saving and keeps unknown keys
  and comments when it rewrites the file.
- **Future version** (`meta.version` > 1): the game applies the keys it knows and logs a warning.
- **`[game]` catalog:** `game/game_options.json` lists each game option: key, type (bool / enum /
  int), allowed values, default, environment variable and value mapping, and es/en label and
  description. The launcher builds the Graphics rows from it; the game reader uses it for the
  `[game]` section, so the generic runtime has no Patapon 3 knowledge. Patapon 3 ships the three
  options above.
- Launcher-only state (chosen theme, achievements, last save) lives in its own file, not here.
- Defaults are the user's current `jugar.bat` (30 fps, smooth animation, scale 4, HD pack), with
  fullscreen on.

The contract is written down in `docs/contracts/settings-v1.md` with fixture files (valid,
invalid values, future version) that both the C++ and the Rust readers are tested against.

## 3. Launcher (part 1)

- **Main menu:** theme background + logo, entries Start · Options · Exit, bottom prompt bar with
  keyboard or gamepad glyphs for the device in use (Xbox / PlayStation from `gilrs`).
- **Options**, tabs switched with Q/E or L1/R1:
  - General: launcher language.
  - Graphics: fullscreen, window size, internal resolution (x1..x8, shown as "1920x1088"),
    scaling filter, texture filter, HD textures, then the `game_options.json` rows (30/60 fps,
    smooth animation, fast loading).
  - Controls: read-only list of the current keyboard and gamepad mapping, "editable soon".
  - Online: server, nickname, relay, port offset.
  - Bottom bar: R = restore defaults, Confirm, Back; leaving with unsaved changes asks first.
- **Start:** checks that the game executable and `disc0` exist and explains what is missing
  (part 2 automates the ISO), then hides the window, launches
  `game/psprecomp_runtime(.exe) --settings ../settings.ini --log ../logs/game.log` with the game
  folder as working directory, and waits.
- **After the game:** exit 10 -> back to the main menu; exit 0 -> the launcher exits too; any other
  code -> "The game closed unexpectedly" with a button that opens the log. Before each launch
  `logs/game.log` is rotated to `logs/game.prev.log`.
- **Navigation:** keyboard (arrows, Enter, Esc), mouse and gamepad (d-pad / stick, A/Cross,
  B/Circle) behave the same. Rust turns gamepad input into `up/down/left/right/confirm/back/
  tab_prev/tab_next` events; a React focus manager owns the selection.
- **Strings:** `lang/es.json`, `lang/en.json`. **Theme:** `themes/default/theme.json` (colors, a
  free font, `background.png`, `logo.png`, optional CSS); part 5 extends this format without
  breaking it.
- **Window:** fullscreen when `display.fullscreen`, otherwise a resizable 1280x720 window, so the
  launcher and the game share the mode.

## 4. Game-side changes (`psprecomp`)

1. **Settings reader** `runtime/include/psp_settings.h`, `runtime/src/psp_settings.cpp` (pure,
   unit-tested): first thing in `main()`. `--settings <path>`, else `settings.ini` next to the
   executable, else nothing. Sets each unset environment variable from its key (paths resolved
   against the ini folder; `fps = 60` adds `60fps` to `PSPRECOMP_PATCHES`; `hd_textures = false`
   leaves `PSPRECOMP_TEXTURES` unset). `[game]` keys come from `game_options.json` next to the
   executable. Every action logs a `[SETTINGS]` line.
2. **Back to menu:** F1, or Select+Start held 1 s on a gamepad (the Guide/Home button is often
   taken by the Xbox Game Bar or Steam; it also works when SDL delivers it). A "Back to the
   menu? Unsaved progress is lost — Yes / No" box is drawn with the existing overlay layer
   (`psp_osk_overlay`), in `general.language` (es/en strings built in); while it is open the game
   gets no input. Yes -> fast exit with **exit code 10**. A normal quit -> 0; failures -> other.
3. **Log:** `--log <path>` redirects stderr to that file; without it nothing changes.
4. Nothing else changes: boot order, the game, existing options. Without a `settings.ini` next to
   the executable everything behaves exactly as today.

## 5. Repository, testing, success criteria

- **Launcher repo:** new project `DECO/launcher` with its own local git; not pushed until part 6
  or the user says so (the public name is decided then). `src-tauri/` (Rust: settings, launch,
  gamepad), `src/` (React + TSX + Vite), `lang/`, `themes/default/`. Tauri 2, React 19,
  TypeScript 7.0.2; the plan checks the installed Node / npm first.
- **The contract lives in `psprecomp`:** `docs/contracts/settings-v1.md` + fixtures; both readers
  are tested against the same fixtures.
- **Tests:**
  - Game (ctest): ini parsing, environment precedence, relative paths, `[game]` catalog, invalid
    values, no-ini no-op; back-to-menu combo timing and Yes/No dialog state as pure logic.
  - Launcher (`cargo test`): ini read/write keeping comments and unknown keys, exit-code
    handling, executable lookup per OS, gamepad -> navigation events.
  - Launcher UI (Vitest): menu and tab focus, editing and restoring options, unsaved-changes
    prompt.
  - End to end on the user's PC with a test instance: options saved -> the game applies them
    (`[SETTINGS]` lines, e.g. the 60 fps patch active) -> F1 -> Yes returns to the launcher (10) ->
    quitting closes both -> a forced crash shows the notice.
  - Regression: without `settings.ini`, the graphics tour and `rt_regress` PASS as always.
- **Part 1 is done when:** from the portable folder the launcher opens fullscreen, works with
  keyboard, mouse and gamepad, Options save and the game honours them, Start / back to menu /
  exit work, `jugar.bat` and the existing tests are untouched, and the user has tried it and
  approved it.
