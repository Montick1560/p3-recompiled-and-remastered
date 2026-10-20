# PC button prompts, part 2 — the mission command list — Design

Date: 2026-10-20. Asked by the user ("sigue con la parte 2, crea un plan … para que lo sigas en la siguiente
sesión de forma nativa"). Not brainstormed with the user: the decisions below are Claude's, taken under the
standing "toma todas las decisiones" of parts 1 / 1.1 and flagged to the user. Parts 1 + 1.1 (help bars:
icons + sharp labels) are pushed (`p3/main` 5504555). Spec of part 1:
`2026-10-19-pc-button-prompts-part1-design.md`; part 1.1: `2026-10-20-pc-button-prompts-part1-1-bar-text-design.md`.

## 1. Intent

During a mission the game shows the **command list** at the bottom of the screen — "Avanzar: □·□·□·○
Atacar: ○·○·□·○ Defen.: △·△·□·○ Cargar: ○·○·△·△ / Atrás: ○·□·○·□ Saltar: ✕·✕·△·△ Purificar: □·○·✕·△" — with
PSP buttons. Part 2 makes it show the player's keys / pad buttons exactly as the help bars now do (part 1), with
the labels redrawn sharp (part 1.1). It is the most visible PSP leftover: it is on screen for the whole of every
mission.

## 2. What the game does (recon 2026-10-20, DxD 1.1.4, training mission of the tour save)

Tool: `DECO/build/tools/texture_recon/recon_mission.py` (drives `gfx_tour`'s checkpoints to `march` on port
9998, finds the strip's PRIM in a display-list log, dumps the atlas, reads its sprites). Outputs in
`DECO/build/prompts_ref/p2recon/` (`es/`, `en/`).

- The whole strip is **one through-mode sprite PRIM** (`prim=6`, `vtype 0x800102`: u16 UV + s16 xyz, 10 bytes a
  vertex, 63 sprites in Spanish) at screen y 244..265 (two rows of 11 px), the last textured PRIM before the
  frame ends. Texture **128×64 CLUT4, swizzled, bufw 128** at `0x092E4100`, CLUT `0x092E5140` (16 × 8888:
  colour `7F7F7F`, alpha ramp `00 11 22 … EE FF`; English `… 10 …`: the ramp differs per language like the
  help bar's). The same address in Spanish and EU English.
- The atlas (pre-rendered per language) holds the **command names** in rows of 11 texels — row k = y
  `11k .. 11k+10`, cap height 8 (y `11k+1 .. 11k+8`), baseline `11k + 9`, descenders to `11k+10` — and one
  **glyph row**, the same in both languages (identical coverage):
  | glyph | UV cell the game crops (x, y, w, h) | ink |
  |---|---|---|
  | `:` | 0, 53, 2, 10 | x 1, y 56..60 |
  | `·` | 3, 53, 10, 10 | x 7..9, y 57..59 |
  | □ (Pata) | 14, 53, 10, 10 | x 15..23, y 54..62 |
  | ○ (Pon) | 25, 53, 10, 10 | x 26..34 |
  | △ (Chaka) | 36, 53, 10, 10 | x 37..45 |
  | ✕ (Don) | 47, 53, 10, 10 | x 48..56 |
  Labels (Spanish): "Defen." "Cargar" (row 0, x 40..109), "Atacar" "Atrás" "Invocar" (row 1), "Saltar"
  "Avanzar" (row 2), "Purificar" (row 3, y 33..43). English: "Onward" "Defend" "Charge" / "Attack" "Retreat"
  "Summon" / "Party" "Jump" (rows 0..2). Each label is cropped by one sprite whose UV rect is its row band
  (e.g. `0,0..41,10`), so a label's box is its own sprite. "Invocar"/"Summon" is in the atlas even when the
  strip does not show it yet.
- **Drum hits**: no button glyph is drawn when a drum is hit (the game shows the PATA / PON words); nothing to do.
- **Hint panel / dialogue** (START in the training mission: "□□□○ = Marchar"): drawn from the dialogue
  **font texture** (512×512 CLUT4 at `0x092BA040`, a glyph cache with □ ○ △ as ordinary characters) — that is
  part 3 (icons inside text), not this part.
- HD pack: **the pack replaces this atlas** with a ×4 image per language (Spanish: key
  `092e410044477d7efca61e81` → `ui/language/spanish_us/0000000044477d7efca61e81.png`, 512×256, same layout);
  the seam already composes on a pack image at `img.w / spec.width` (part 1), so icons and text land on it.
  (The recon lands in the hideout with the pack only at render scale 1.)

## 3. Decisions (Claude's, flagged)

1. **Same technique, same options as parts 1 / 1.1.** The atlas is recognised by content (coverage signature of
   the glyph row), its four drum cells are redrawn with the icon of the button mapped to that PSP button for the
   last-used device (keyboard / Xbox-Steam Deck / DualShock 4 / DualSense), Gris / Color style; its labels and
   its `:` `·` separators are redrawn sharp with Zen Maru Gothic Bold from per-language word tables. No new
   launcher option: `PSPRECOMP_BUTTON_ICONS` (gray / color / off) and `PSPRECOMP_BAR_TEXT=off` cover both atlases.
2. **Icon cell = the UV cell the game crops** (10×10 texels, table above), not the ink box: the icon may use the
   whole area the game shows. The 1-texel gaps between cells stay empty.
3. **The prompts patcher holds several atlases** (help bar + command list now, part 3 may add more): at most 4,
   one patcher in the seam; the recognition tag packs the atlas index and its local tag. No second patcher
   (the seam allows 4, but the variant is the same for every prompt atlas).
4. **Languages / releases**: every language and release the help-bar text covers — DxD 1.1.4 es, en (EU), en (US),
   fr, it, de, ja; 1.1.5b; vanilla EU en, de, fr, it, es, pt, nl, ru — sharing tables wherever the label
   coverage is identical (as in part 1.1). Polish: icons only (the bar font lacks ą ę ł ś ż ź ć ń), unless its
   command names happen to use none of them (then it gets a table). An atlas whose glyph row differs gets an
   extra signature hash (as part 1.1 did for fr/ru/pl/nl/ja).
5. **Code placement** (CLAUDE.md: no game data in `runtime/`): the multi-atlas patcher is runtime; the command
   list spec, cells, signatures and word tables are `games/patapon3/runtime/` (new `p3_command_labels.cpp`).

## 4. Goals / anti-goals

Goals: every command-list cell shows the button that performs that drum, switching with the last input; labels
sharp; every language and release above; with and without the HD pack.
Anti-goals: any frame-time cost; any change to the help bars (part 1 / 1.1 output must stay byte-identical);
an icon that is not the mapped button; a word not character-for-character the game's.

## 5. Verification

- Unit: multi-atlas tag packing and recognition; the command-list tables (drawable, fit ≥ 75 %, inside the
  128×64 atlas, unique signatures); part 1 / 1.1 tests unchanged and green.
- Live on 9998 (scale 2 for the drum checkpoint; `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`): the strip per
  language with keyboard / xbox / ps4 / ps5, Gris and Color, icons off, text off, HD pack, 1.1.5b, vanilla.
- Gates: `ctest`; a both-off tour = `prompts_off` (rt_regress PASS); the default tour vs `bartext_on2`: only
  the strip rows (y ≥ 240) of the mission checkpoints differ; `perf_hideout.py` unchanged.
- Push only after the user's OK (next push branch `push-p3-24`).
