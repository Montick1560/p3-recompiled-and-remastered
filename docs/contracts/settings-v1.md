# settings.ini v1 — contract between the launcher and the game

Source: `docs/superpowers/specs/2026-10-17-launcher-part1-design.md` section 2 (copied verbatim
below), plus the parsing rules, key table, command line and exit codes both readers implement.
Both the game (C++, `runtime/src/psp_settings.cpp`) and the launcher (Rust) are tested against
the fixtures in `docs/contracts/fixtures/`.

## `settings.ini` v1 (the contract)

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


## Parsing

- UTF-8 with or without a BOM; CRLF or LF line endings.
- `;` and `#` start a comment only at the start of a line (after optional spaces). There are no
  inline comments: a `;` after a value is part of the value. The default file above shows
  trailing comments for documentation only; the real default file
  (`fixtures/defaults.ini`) puts each comment on its own line.
- `[section]` lines open a section. `key = value` splits at the first `=`.
- Keys and section names are case-insensitive (lowercased on read). Values are trimmed.
- A key repeated in a section keeps the last value.
- Lines that are neither a section, a key nor a comment are ignored with a log line.
- Booleans: `true|false|1|0|yes|no|on|off`, case-insensitive. Integers: decimal, optional sign.

## Core keys

| key | env | type | rule |
|---|---|---|---|
| general.language | PSPRECOMP_UI_LANG | enum es,en | copied |
| paths.disc0 | PSPRECOMP_DISC0 | path | resolved against the ini folder |
| paths.savedata | PSPRECOMP_SAVEDATA | path | resolved |
| display.fullscreen | PSPRECOMP_FULLSCREEN | bool | true -> `1`, false -> unset |
| display.window_scale | PSPRECOMP_WINDOW_SCALE | int 1..8 | copied |
| display.render_scale | PSPRECOMP_RENDER_SCALE | int 1..8 | copied |
| display.present_filter | PSPRECOMP_PRESENT_FILTER | enum linear,nearest | copied |
| display.texture_filter | PSPRECOMP_TEX_FILTER | enum auto,nearest,linear | copied |
| display.hd_textures + display.texture_pack | PSPRECOMP_TEXTURES | bool + path | resolved pack path when true, unset when false |
| online.server | PSPRECOMP_ADHOC_SERVER | string (non-empty) | copied |
| online.nickname | PSPRECOMP_ADHOC_NICK | string (non-empty) | copied |
| online.relay | PSPRECOMP_ADHOC_RELAY | bool | true -> `1`, false -> unset |
| online.port_offset | PSPRECOMP_ADHOC_PORT_OFFSET | int 0..65535 | copied |
| online.mac_file | PSPRECOMP_ADHOC_MAC_FILE | path | resolved |
| game.* | from `game_options.json` | per catalog | `map` or copied |

An environment variable that is already set is never overwritten. An invalid value keeps the
runtime default (the variable stays unset) and logs `[SETTINGS] <section>.<key>: ...`.

## Option catalogs

`core_options.json` (launcher) and `game/game_options.json` (one per game; Patapon 3:
`games/patapon3/game_options.json`) share one schema:

```
{ "version": 1, "options": [ { "section", "key",
    "type": "bool" | "enum" | "int" | "string" | "path",
    "values"?: [string], "min"?: int, "max"?: int, "default": string,
    "tab": "general" | "graphics" | "online",
    "label": {"es", "en"}, "description"?: {"es", "en"},
    "display"?: { value: {"es", "en"} },
    "env"?: string, "map"?: { value: string | null } } ] }
```

All values are strings (`"true"` / `"false"` for bools). `map` turns a value into the
environment value (`null` = leave unset); without `map` the value is copied as is. A `map` onto
`PSPRECOMP_PATCHES` appends to the patch list (comma-separated) instead of replacing it.

## Command line

- `--settings <path>`: read this file. Without it the game reads `settings.ini` next to the
  executable if it exists, else nothing (behaves exactly as before the contract).
- `--log <path>`: stderr goes to this file. Without it nothing changes.

Both are optional and independent.

## Exit codes

| code | meaning |
|---|---|
| 0 | the player quit |
| 10 | the player chose "back to the menu" (the launcher shows its main menu again) |
| anything else | crash / failure (the launcher shows the crash notice and the log) |
