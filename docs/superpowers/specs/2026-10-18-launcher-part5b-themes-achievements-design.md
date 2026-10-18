# Launcher part 5b — Themes + achievements — Design

Date: 2026-10-18. Approved in chat by the user, section by section (brainstorming); written spec approved by
the user the same day. Next: writing-plans, Plan 1 (launcher) first. This is the "part 5" of the
launcher decomposition (`2026-10-17-launcher-part1-design.md`: "Themes + achievements: community theme format,
achievements read from the game, unlocks"); the HD texture download that shipped as "part 5" was an extra.

## Goal

Make the launcher feel like *Patapon 1+2 Replay*: menu themes with music, sounds and an animated background,
a Steam-like **Achievements** screen, and themes that unlock with achievements. Achievements are detected **by
the game while the player plays** and announced in game with a toast.

**Performance comes before the feature** (user, 2026-10-18): nothing in this design may cost measurable frame
time during a game session (§5).

### Decisions (conversation 2026-10-18)

1. Achievements are a feature of their own (a screen with the list, what is earned, percentage), and some unlock
   themes. The in-game toast is in scope.
2. Two implementation plans, one spec: **Plan 1** launcher (themes, Themes gallery, Achievements screen, music,
   sounds, volume, sample themes); **Plan 2** game (find the progress struct, reader, evaluator, toast, final
   catalogue, end to end).
3. Achievements are **global on this PC**, Steam-like, and remember the save and date that earned each one. An
   imported save (the user's PPSSPP DATA04, 1878 h) unlocks everything it already meets at once; that is wanted.
4. The catalogue is **official and fixed**: the project defines it, every player has the same list, so earned
   achievements are comparable. Players cannot add achievements; community themes can only **require** an
   official achievement to unlock.
5. A theme can bring colours, font, background image **or looping video**, logo, CSS, **menu music** and **UI
   sounds**. Options gets a **music volume**.
6. Theme art: no game art in the package (no licence, same as the texture pack). Plan 1 ships 2–3 sample themes
   made for the project; themes built from assets of the player's own ISO are a later part.
7. Themes are chosen in a **Themes** gallery reached from the main menu; **Achievements** is its own screen.
   Both link to each other.
8. Detection: the game reads its own progress structure in RAM (approach "hybrid", §4).

## 1. Facts found during the design (2026-10-18)

- After the PSP mode-5 decrypt, `SECURE.BIN` is still encrypted by the game itself (entropy 7.8 bits/byte).
  owodzeg/libP3Hash (no licence: never vendored) removes that layer: 16-byte blocks, all-zero blocks kept.
- Decrypted, the user's DATA04 has the hero name "Edson" in UTF-16 at `+0xF1C8` (DATA08: "edd", same offset)
  and the current class at `+0x9520` (4 = Yumiyacha; DATA08: 2 = Yarida).
- idrgn/PataponRichPressence reads, in PPSSPP RAM, `*(0x08AABD94)` → `+0x50` = the save struct, whose `+0x9520`
  is the current class: **the save file layout is the RAM layout**. `+0x78` points to the multiplayer struct;
  `0x08ABB1A0` holds the loaded overlay's file name (`OL_Title.bin`, `OL_Azito.bin`, `OL_Mission.bin`).
  Those addresses are for the build that tool targets; the DxD releases must be verified (Plan 2).
- The game keeps its own statistics (heroes defeated, VS completed, …) in the save: the main source of
  achievements.
- Reference repos (user-provided): idrgn/PPEditor (param files: names of characters, classes, equipment),
  Nemoumbra/PataponAllocators (heap). Local clones in `DECO/build/ext_ref_p5/`, used read-only.

## 2. Architecture and files

```
Game (psprecomp, games/patapon3)                         Launcher
save struct pointer in RAM (per release)
every 60 vblanks: compiled checks over the struct ──► queue ──► worker thread
                                                                 ├─ <root>/achievements/unlocked.json ──► Achievements screen
                                                                 └─ toast texture ──► overlay quad       Themes gallery
achievements.json (official catalogue, next to the exe) ◄─────────────────────────────── read by the launcher too
```

- **Catalogue** `games/patapon3/achievements.json` in psprecomp, copied next to the exe of every release by the
  build (like `game_options.json`). The launcher reads it from the selected release's folder.
- **Earned** `<portable root>/achievements/unlocked.json`, shared by every release:
  `{"version": 1, "earned": {"<id>": {"date": "<ISO-8601 UTC>", "save": "DATA04", "hero": "Edson",
  "release": "dxd-1.1.4"}}}`. Append-only: written only by the game; the launcher never writes it.
- **Field map** per release in the game module (`games/patapon3/runtime/`): the save-struct pointer address
  and the offset/size of every fact. A release without a map has no achievements (logged once).
- **Themes** `themes/<id>/theme.json` (§3.1). Settings (contract `docs/contracts/settings-v1.md`, extended,
  still v1): `[launcher] theme = <id>` (default `default`), `[launcher] music_volume = 0..100` (default 70),
  `[game] achievement_toasts = true|false` (default true).
- **Game settings:** the settings reader sets `PSPRECOMP_ACHIEVEMENTS_FILE = <ini folder>/achievements/unlocked.json`
  and `PSPRECOMP_ACHIEVEMENT_TOASTS` from `[game] achievement_toasts`. Without a `settings.ini` (developer
  `jugar.bat`) nothing is set and achievements are off: the game behaves exactly as today.
  `PSPRECOMP_ACHIEVEMENTS=0` turns the whole system off.

## 3. Launcher (Plan 1)

### 3.1 Theme format (extends part 1's, nothing breaks)

`themes/<id>/theme.json`; every new key is optional:

```json
{
  "name": "Noche", "author": "Patapon Community",
  "colors": { "bg": "#0b0b0e", "text": "#ffffff", "accent": "#f5a400", "panel": "#e9e9e4",
              "panelText": "#111111", "muted": "#9a9a9a" },
  "font": null,
  "background_video": "bg.webm",
  "music": "music.ogg", "music_volume": 80,
  "sounds": { "move": "move.ogg", "confirm": "confirm.ogg", "back": "back.ogg" },
  "unlock": "hero_level_10"
}
```

Plus the existing `background.png`, `logo.png`, `theme.css` and a new optional `preview.png` (gallery card).
`<id>` is the folder name (`[a-z0-9_-]`). File names are relative, no `..`, no absolute paths, no `\`. A theme
whose `theme.json` cannot be read is skipped (one log line). `default` is always available and can never be
locked. `unlock` naming an id that is not in the catalogue keeps the theme locked forever (log line).

### 3.2 Screens

- **Main menu:** Iniciar · Partidas · **Logros** · **Temas** · Opciones · Salir.
- **Logros:** header "18 / 27 · 67 %"; list of icon, name, description; earned ones with "18 oct 2026 · Edson",
  pending ones dimmed, hidden ones as "???" until earned. Q/E filter: Todos / Conseguidos / Pendientes. An
  achievement that unlocks a theme shows "🎨 Abre el tema X"; Enter opens that theme in the gallery.
- **Temas:** grid of cards (preview, name, author). Focusing a card previews the theme (screen background and its
  music); Enter applies and saves `[launcher] theme`; Esc returns to the applied theme. Locked: 🔒 and "Consigue:
  <name>"; previewable, not applicable; Enter opens that achievement.
- **Music:** the applied theme's music loops in every launcher screen, stops when the game starts and resumes
  when the launcher comes back. Effective volume = `music_volume` (theme) × `[launcher] music_volume` / 100.
  **Options › General › Volumen de la música** (0–100, steps of 10).
- **Sounds:** move / confirm / back from the theme; none if the theme has none.
- **No data yet** (before Plan 2, never played): Logros shows everything pending; locked themes stay locked.
- Missing or corrupt `unlocked.json` = nothing earned.

### 3.3 Sample themes

`default` (today's) plus 2–3 made for the project: colours, CSS, synthesized UI sounds and music, one with a
generated looping `.webm` background (FFmpeg, as `p3.sh` already uses), one locked by an early official
achievement so the lock path is exercised. No art or audio from the game.

## 4. Game (Plan 2)

### 4.1 Finding the struct

Locate, for DxD 1.1.4 first (then vanilla EU and 1.1.5b), the global that leads to the save struct (the
RichPresence chain is the starting point) using the recompiled code, the debug socket and PPSSPP as oracle.
Verify by reading `+0xF1C8` (hero name) and `+0x9520` (class) with the user's save loaded.

### 4.2 Facts and catalogue

A fact = (offset, size, signedness) in the struct, named (`stats.heroes_defeated`, `hero.level`,
`dark_hero.ragewolf.defeated`, …). Achievement entry:

```json
{ "id": "boss_ragewolf", "icon": "🐺", "hidden": false,
  "name": { "es": "Lobo domado", "en": "Wolf Tamed" },
  "desc": { "es": "Derrota a Ragewolf.", "en": "Defeat Ragewolf." },
  "when": { "fact": "dark_hero.ragewolf.defeated", "op": ">=", "value": 1 } }
```

`op` ∈ `>=`, `==`, `!=`; `when` may be `{"all": [...]}` or `{"any": [...]}`. An `id` never changes once
published. Initial list (~25), by category: **game statistics** in steps (heroes defeated, VS completed,
missions cleared, … 1 / 10 / 100 …), **story** (prologue, each Archfiend, the ending), **Dark Heroes**,
**hero progress** (levels, classes unlocked), **DxD** (Gigante King), 2–3 **hidden**. Rule: an achievement
enters the catalogue only when its fact is **verified** — present as expected in the user's DATA04, absent or
lower in DATA08 — and, for statistics, equal to what the game's own statistics screen shows. Unverifiable ones
are dropped, never guessed. Final names and texts are fixed in Plan 2.

### 4.3 Reading safely

Before evaluating, a sanity check: the pointer lies in user RAM, the hero name at `+0xF1C8` is printable UTF-16,
the class at `+0x9520` is in 1..34, every fact used is within its plausible range. Any failure skips that check
(retried 60 vblanks later). An achievement is granted only when its condition holds in **two consecutive
checks** (no half-written state during loads).

### 4.4 Toast

Top right, ~4 s: icon, "Logro desbloqueado" / "Achievement unlocked", the name, in `general.language`. Queued;
more than 3 at once collapse into "🏆 N logros desbloqueados". Drawn with the overlay layer the on-screen
keyboard uses, never blocks input. `[game] achievement_toasts = false` hides toasts; achievements are still
recorded.

### 4.5 Out of scope

Cheat protection (an edited save unlocks achievements; comparisons are on trust), online / ad hoc achievements,
themes from the player's ISO, downloading themes, a toast sound.

## 5. Performance (above the feature)

1. **Per frame:** the vblank hook decrements a counter; the check runs once every 60 vblanks. 59 frames out of 60
   cost one decrement and one compare.
2. **Compiled checks:** at boot the catalogue becomes a flat array of `(offset, size, op, value, index)` with only
   the achievements **not yet earned**. No JSON, no strings, no allocation during play. An earned achievement is
   removed from the array; with everything earned the check does nothing.
3. **Cheap reads:** only the bytes the pending checks name, straight from guest RAM; the 178 KB struct is never
   copied. Budget **< 20 µs per check**, enforced by a micro-benchmark test.
4. **No I/O or text work on game / render threads:** a new achievement pushes its index into a lock-free queue;
   a low-priority worker writes `unlocked.json` (temp file + rename) and rasterizes the toast text into a
   texture.
5. **Toast cost only while visible:** no extra draw when idle; while visible one quad with a pre-built texture
   (one upload), released after ~4 s.
6. **Kill switch:** `PSPRECOMP_ACHIEVEMENTS=0` — no catalogue, no worker, no check.
7. **Gate:** `build/tools/perf_hideout.py` before/after, two samples each, `--diff`: fps, ge_ms/frame and CPU
   within noise or the change does not land; plus one sample taken while a toast is on screen, whose frames stay
   within the 60 fps budget.

## 6. Testing, errors, done

- **Plan 1 — Rust:** theme.json old and new forms, broken theme skipped, unknown `unlock` locked, unsafe paths
  refused, `unlocked.json` missing / empty / corrupt = nothing earned and never written by the launcher, catalogue
  read from the release folder. **Vitest:** gallery (navigate, preview, apply, locked → achievement), Logros
  (counter, filters, hidden, → theme), volume and sounds. **E2E (CDP):** sample `unlocked.json`, unlocks, applied
  theme survives a restart, screenshots at 1920×1080 and 1280×800 with nothing overflowing.
- **Plan 2 — ctest:** reader over small synthetic fixtures (fields at their offsets), sanity check rejects
  garbage, two-check rule, evaluator (ops, all/any, shrinking array), atomic file write, micro-benchmark < 20 µs.
  Optional test against the user's decrypted saves when present in `build/` (DATA04 meets the expected list,
  DATA08 almost none); the saves are never committed. `rt_regress.py` PASS and the §5 gate. **E2E (port 9998):**
  load DATA04 → grouped toast in the hideout → `unlocked.json` correct → the launcher shows them and unlocks the
  matching themes.
- **Errors:** nothing in achievements or themes may stop the game or the launcher; at worst achievements are
  missing and `logs/game.log` has an `[ACH]` line.
- **Done:** Plan 1 when themes with music, sounds and video can be chosen and both screens work with keyboard,
  mouse and gamepad, and the user approves; Plan 2 when the user's save unlocks its achievements live with the
  toast, the performance gate passes, and the user approves after playing.
