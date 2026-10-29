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
fullscreen = true           ; PSPRECOMP_FULLSCREEN=1 when true (legacy: window_mode decides when present)
window_mode = fullscreen    ; PSPRECOMP_WINDOW_MODE: windowed | borderless | fullscreen
window_scale = 2            ; PSPRECOMP_WINDOW_SCALE, 1..8
render_scale = 4            ; PSPRECOMP_RENDER_SCALE, 1..8
present_filter = linear     ; PSPRECOMP_PRESENT_FILTER: linear | nearest
texture_filter = auto       ; PSPRECOMP_TEX_FILTER: auto | nearest | linear
hd_textures = true          ; PSPRECOMP_TEXTURES=<texture_pack> only when true
texture_pack = textures/INFN00001
osk_skin = deck             ; PSPRECOMP_OSK_SKIN=<ini folder>/skins/osk/deck.json

[game]                      ; keys declared by game/game_options.json (Patapon 3 below)
fps = 30                    ; 30 | 60  -> 60 adds "60fps" to PSPRECOMP_PATCHES
smooth_animation = true     ; PSPRECOMP_P3_ANIM_SMOOTH=1|0
fast_loading = true         ; PSPRECOMP_P3_FAST_LOAD=1|0
achievement_toasts = true   ; PSPRECOMP_ACHIEVEMENT_TOASTS=1|0

[online]
server = 127.0.0.1          ; PSPRECOMP_ADHOC_SERVER
nickname = Patapon          ; PSPRECOMP_ADHOC_NICK
relay = false               ; PSPRECOMP_ADHOC_RELAY=1 when true
port_offset = 10000         ; PSPRECOMP_ADHOC_PORT_OFFSET
mac_file = adhoc_mac.txt    ; PSPRECOMP_ADHOC_MAC_FILE

[controls]                  ; PSPRECOMP_CONTROLS: remappable keyboard keys / gamepad buttons
up_keys = Up                ; up to two SDL key names, comma separated
up_pad = dpup               ; one SDL GameController button name
down_keys = Down
down_pad = dpdown
left_keys = Left
left_pad = dpleft
right_keys = Right
right_pad = dpright
cross_keys = X, Return
cross_pad = a
circle_keys = Z
circle_pad = b
square_keys = A
square_pad = x
triangle_keys = S
triangle_pad = y
l_keys = Q
l_pad = leftshoulder
r_keys = W
r_pad = rightshoulder
start_keys = Space
start_pad = start
select_keys = V, Tab
select_pad = back
```

`[launcher]` (e.g. `version = dxd-1.1.5b`, the last played release) belongs to the launcher; the
game ignores the section without logging.

Launcher keys: `version` (last played release), `save_<release>` (the chosen save), `ppsspp_savedata` (the PPSSPP
folder), `theme` (the applied launcher theme id, default `default`; a missing or locked one shows `default` and the
key is left alone) and `music_volume` (`0`, `10`, …, `100`, default `70`; the only `[launcher]` key in
`core_options.json`, so Options › General shows it) and `render_scale_chosen` (`true` once the player saves a changed resolution in Opciones; while it is absent the launcher writes the detected `[display] render_scale` at each start: `4` when a DXGI hardware adapter has >= 2 GiB of dedicated video memory, `2` otherwise; the defaults fixture keeps `4`). Themes and achievements: `docs/contracts/achievements-v1.md`.

`[controls]` remaps the PSP buttons. Each action has up to two keyboard keys, `<action>_keys`
(comma separated SDL key names), and one gamepad button, `<action>_pad` (an SDL GameController
button name). Actions, in this order: `up down left right cross circle square triangle l r start
select`. An empty value unbinds. The defaults above are exactly the fixed mapping from before
this section meant anything.

- **Key names** are SDL's (`X`, `Return`, `Space`, `Tab`, `Left Shift`, `Up`, `1`, `F2`, `Keypad 4`
  ...), case-insensitive. **Pad names** are SDL's GameController button names (`a b x y back start
  leftshoulder rightshoulder leftstick rightstick dpup dpdown dpleft dpright misc1 paddle1..4
  touchpad`).
- **Fixed, never remapped:** the left stick is the analog stick; LT / RT also press L / R; the
  mouse wheel presses D-pad up / down; F1, F11, Alt+Return and the pad `guide` button are reserved
  (refused as bindings). The back-to-menu combo holds the buttons bound to `select` and `start`.
- **Validation (game and launcher):** unknown action keys are ignored; an unknown name, a reserved
  key, or a third key makes that entry fall back to its default, and the game logs
  `[CONTROLS] <action>: ...`; a key or pad button bound to two actions keeps the first action in
  the list order above and drops the other with a `[CONTROLS]` line. The launcher never writes such
  values (it moves a key instead).
- **Transport to the game:** the reader turns the section into one variable, `PSPRECOMP_CONTROLS`,
  canonical form `action=key1,key2|pad;...` (only the actions present in the ini; an empty key list
  or pad is valid and means unbound). It follows the same precedence as every other key.

`[display] osk_skin` names the on-screen keyboard skin:
`PSPRECOMP_OSK_SKIN=<ini folder>/skins/osk/<name>.json`. Names match `[a-z0-9_-]+`; shipped skins
are `deck` (default), `light` and `patapon`. A missing or invalid skin falls back to the built-in
Deck skin.

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
  description. The launcher builds its Options rows from it (each option's `tab`); the game reader uses
  it for the `[game]` section, so the generic runtime has no Patapon 3 knowledge. Patapon 3 ships the
  four options above.
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
| display.fullscreen | PSPRECOMP_FULLSCREEN | bool | true -> `1`, false -> unset (legacy switch, see window_mode) |
| display.window_mode | PSPRECOMP_WINDOW_MODE | enum windowed,borderless,fullscreen | copied; absent or invalid -> unset, and then `display.fullscreen` decides (true = fullscreen, false = windowed). The launcher writes both keys (`fullscreen = mode != windowed`) so older game builds keep working |
| display.window_scale | PSPRECOMP_WINDOW_SCALE | int 1..8 | copied |
| display.render_scale | PSPRECOMP_RENDER_SCALE | int 1..8 | copied |
| display.present_filter | PSPRECOMP_PRESENT_FILTER | enum linear,nearest | copied |
| display.texture_filter | PSPRECOMP_TEX_FILTER | enum auto,nearest,linear | copied |
| display.hd_textures + display.texture_pack | PSPRECOMP_TEXTURES | bool + path | resolved pack path when true, unset when false |
| display.osk_skin | PSPRECOMP_OSK_SKIN | name `[a-z0-9_-]+` | resolved to `<ini folder>/skins/osk/<name>.json` when valid, unset otherwise |
| (always) | PSPRECOMP_ACHIEVEMENTS_FILE | path | <ini folder>/achievements/unlocked.json (docs/contracts/achievements-v1.md) |
| online.server | PSPRECOMP_ADHOC_SERVER | string (non-empty) | copied |
| online.nickname | PSPRECOMP_ADHOC_NICK | string (non-empty) | copied |
| online.relay | PSPRECOMP_ADHOC_RELAY | bool | true -> `1`, false -> unset |
| online.port_offset | PSPRECOMP_ADHOC_PORT_OFFSET | int 0..65535 | copied |
| online.mac_file | PSPRECOMP_ADHOC_MAC_FILE | path | resolved |
| controls.*_keys + controls.*_pad | PSPRECOMP_CONTROLS | SDL key / button names | canonical spec of the actions present (see above) |
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
environment value (`null` = leave unset); without `map` the value is copied as is. Precedence
applies to catalog keys too: `fps = 60` sets `PSPRECOMP_PATCHES=60fps` only when
`PSPRECOMP_PATCHES` is not already set.

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
| 11 | the game files in disc0 do not match this exe (the launcher asks to add the ISO again) |
| anything else | crash / failure (the launcher shows the crash notice and the log) |
