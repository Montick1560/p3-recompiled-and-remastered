# PC button prompts, part 1.1 — sharp help-bar text — Design

Date: 2026-10-20. Agreed in chat with the user (brainstorming with a font mockup in the browser companion:
`DECO/.superpowers/brainstorm/1797-1792488067/content/tipografia.html`). The user picked **Zen Maru Gothic**
(option A, bold) and said the new text "must always show" (no launcher option).

Claude's calls (not asked, flagged to the user): bold weight as in the mockup; the separators `:` `/` `&` of the
glyph row are rewritten too; words are placed one by one at the original word's position (§4.3); a word whose
characters the font lacks keeps the game's pixels; the whole TTF is embedded (no subsetting tool on this PC);
`PSPRECOMP_BUTTON_ICONS=off` turns off the icons only, and a new emergency switch `PSPRECOMP_BAR_TEXT=off` (not
in the launcher) turns off the text.

## 1. Intent

Part 1 put PC icons into the help bars. Next to them the game's labels ("Seleccionar", "Confirmar", "Pantalla
de título"…) look blurry: the game ships them pre-rendered at 11 texels per row in the same 256×128 atlas, and
at PC resolutions they are magnified (`scratchpad/gray_bars.png` of the part 1 session; also with the HD pack,
`pack_bars.png`). The user: "se ven feos, solo requeriría que los escribieras de nuevo". Goal: the same words,
in the same place, sharp at any render scale.

## 2. Goals, non-goals, anti-goals

Goals
- Every label of the help-bar atlas is redrawn **sharp** with Zen Maru Gothic Bold at the composition scale
  (the render scale 1..4, or the HD pack image's scale), in the atlas gray, for **every language** the game's
  language menu offers and every release the port builds (DxD 1.1.4, DxD 1.1.5b, vanilla EU).
- **Same words** as the game, character for character (abbreviations like "Selecc." and "Hab. Héroe" stay).
- **Always on**, with or without the PC icons, with or without the HD pack. No launcher option.

Non-goals
- Rewording or translating anything; changing the bar layout, its colours or the icons.
- Text outside the help-bar atlas (dialogue, menus, mission UI): other textures, other projects.
- A font choice in the launcher.

Anti-goals (a failure even if it "works")
- A wrong, missing or misspelled word, or a word of another language.
- Text spilling outside the area the game crops for it: a bar showing a piece of a neighbouring word, or a word
  cut short.
- A visible change in brightness/weight that makes the bar look worse than the original; any frame-time cost.

## 3. What the game does (recon 2026-10-19, part 1; DxD 1.1.4 Spanish)

- One atlas, **256×128 CLUT4 swizzled** (DxD 1.1.4 at `0x092E5300`, CLUT `0x092E9340`; 1.1.5b `0x092E52C0`;
  vanilla `0x092E4DC0`), language-specific. Dump at ×4: part 1 scratchpad `atlas_x4.png`. It holds ~55 labels in
  11-texel rows (y 0..98, plus labels to the right of the pills at y ~101..113 and of the glyph row at
  y 117..127), the START/SELECT pills and the glyph row `: / & □ ○ ✕ △ …`.
- The game builds each bar from sprites that crop one label (or glyph) by UV and draws it at its texel size.
  Labels are gray `0x7F` with an alpha ramp from the palette; **the ramp differs per language** (part 1 found
  that), the coverage does not change within a language.
- Part 1 recognises the atlas by the coverage of the pills and the glyph row (one hash for the five DxD 1.1.4
  languages, `games/patapon3/runtime/p3_prompts.cpp`). The labels differ per language, so their coverage
  identifies the language.
- Words in a row are separated by ~2–4 texels; some touch ("Menú" + "Datos de los miembros" in Spanish).

## 4. Decisions

1. **Technique: redraw the label pixels inside the atlas** through the existing texture patch seam
   (`psp_ge_texpatch.h`), in the same composition as the icons: one cached texture per variant, no per-frame
   work. The variant gains a "text on" flag; the language is a per-texture **tag** that the seam carries from
   recognition to `apply` (see §5).
2. **Language recognition by label coverage**: a second signature over the label area picks the language's
   label table. Unknown signature (a language or release not transcribed) → the labels stay the game's (icons
   still drawn). Never draw a table on an atlas it was not measured on.
3. **Word-by-word placement**: each table entry is one *word run* of the original: its ink box (x0..x1 inclusive,
   texels), its row (y0..y1, baseline) and its exact text. The new word is left-aligned at x0 on the row's
   baseline, at one font size per atlas (the size whose cap height matches the original cap height × scale);
   if its natural width exceeds the box it is condensed horizontally to fit (down to 75 %; never stretched).
   Placing every word at its own original x makes the result independent of how the game groups words into
   labels. Touching words ("MenúDatos") are separate entries with the boundary measured from the dump.
4. **Clearing**: before drawing, the redrawn boxes (plus the half-texel ring the bilinear enlargement spreads,
   as part 1 does for icon cells) are cleared to transparent gray; the ring never eats into a box that is kept
   (a pill or glyph cell left alone, a word kept because of a missing glyph). All clears happen before all draws.
5. **Colour and weight**: text colour = the atlas gray (`spec.gray`), alpha = glyph coverage × the language's
   peak label alpha (measured in the recon, stored with the table), so the brightness matches the original.
6. **Font**: Zen Maru Gothic **Bold** (SIL Open Font License 1.1, Yoshimichi Ohira), from the Google Fonts
   repository, vendored in `runtime/assets/fonts/` with its `OFL.txt` and embedded in the exe like the Kenney
   icons. Rasterised with the vendored `stb_truetype` (already compiled once in `psp_osk_overlay.cpp`; reuse
   that implementation, do not define `STB_TRUETYPE_IMPLEMENTATION` twice). A character the font lacks → that
   word keeps the game's pixels (logged once).
7. **Code placement** (`CLAUDE.md`: no game data in the runtime core): the generic text drawing (font loading,
   rasterising a word into an `RgbaImage` box with the fit rule, the table types) lives in `runtime/`; Patapon 3's
   label tables and their signatures live in `games/patapon3/runtime/`.
8. **Switches**: `PSPRECOMP_BAR_TEXT=off` keeps the game's labels (emergency switch, not in the launcher).
   `PSPRECOMP_BUTTON_ICONS=off` now keeps the PSP icons but still rewrites the text. Both off → nothing is
   registered (today's "off" behaviour).
9. **Credits**: README (next to Kenney) and the font's `OFL.txt` shipped in the repo; the launcher credits
   screen is part 6.

## 5. Seam change (decided)

`GeTexPatcher::recognise` returns a `uint32_t` **tag** (0 = not this texture, 1..255 = recognised) instead of
`bool`; the memo stores the tag with the patcher index; `ge_texpatch_variant` returns
`(patcher + 1) << 28 | tag << 20 | latched_variant` (latched variant now limited to 20 bits); `apply` receives
the low 28 bits and the patcher decodes the tag itself. For the prompts patcher: tag 1 = atlas recognised, labels
unknown; tag 2 + i = label table i.

## 6. Left to the builder

- The recon tooling (dump each language's atlas on port 9998, propose word runs from the coverage, a ×8
  contact sheet to check boxes) and the transcription itself, verified against the dumps.
- The exact baseline/cap measurement per atlas and the rounding of the font size.
- Whether vanilla EU's atlases equal DxD's (then they share tables) or need their own.
- Glyph caching inside a composition.

## 7. Verification

- Unit tests: the seam carries the tag; the fit rule (left-aligned, condensed only when too wide, never outside
  the box); clearing never touches kept boxes; a word with a missing character is kept; every table word renders
  with the embedded font (all characters present) and fits at ≥ 75 %; every table's signature is unique.
- Live on 9998, `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`: hideout bar and the barracks/forge/pause bars, in
  every language, at render scale 4, with and without the HD pack, icons Gris and off; a ×4 crop next to the
  original for each language.
- `ctest`; `rt_regress.py` with `PSPRECOMP_BAR_TEXT=off` against a base of the current build (proves nothing
  else changed) and a default run whose differences are only in the bars; `perf_hideout.py` (no frame-time
  change).
- The user plays and approves before any push.
