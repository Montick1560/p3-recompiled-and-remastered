# PC button prompts, part 1.1 — sharp help-bar text — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every label of Patapon 3's help-bar atlas ("Seleccionar", "Confirmar", "Pantalla de título"…) is
redrawn sharp with Zen Maru Gothic Bold, same words in the same place, in every language and release, always on.

**Architecture:** Part 1's texture patch seam already recomposes the help-bar atlas (icons) on a cache miss. The
seam learns to carry a per-texture *tag* from recognition to `apply`; the prompts patcher uses it for the atlas's
language (a second coverage signature over the labels). A generic text rasteriser (embedded TTF + vendored
`stb_truetype`) draws each transcribed *word run* at its original position, condensed only if too wide. Patapon
3's module holds the per-language word tables.

**Tech Stack:** C++17 runtime, `stb_truetype` (vendored in `runtime/third_party/stb`), CMake/Ninja,
`runtime/cmake/embed_files.cmake`, test-exe-per-file pattern of `runtime/tests/`, Python + PIL for recon tools.

**Spec:** `docs/superpowers/specs/2026-10-20-pc-button-prompts-part1-1-bar-text-design.md`

## Global Constraints

- Words are transcribed **character for character** from the game's atlas (abbreviations kept). Never draw a
  table on an atlas whose label signature it was not measured on.
- Font: Zen Maru Gothic **Bold**, SIL OFL 1.1, vendored with its `OFL.txt` in `runtime/assets/fonts/`.
- No game data in `runtime/src` (CLAUDE.md §3): word tables, signatures, row metrics live in
  `games/patapon3/runtime/`.
- Always on; no launcher option. `PSPRECOMP_BAR_TEXT=off` = game labels; `PSPRECOMP_BUTTON_ICONS=off` = PSP icons
  but new text; both off = nothing registered.
- Condense down to 75 % at most for every table word (enforced by a test); never stretch; never draw outside the
  word's box.
- No steady-state frame-time cost: text is drawn only when a variant is composed (texture-cache miss).
- Tests and live runs on port **9998** only (the user's game is on 9999), with
  `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`. Nothing destructive, nothing installed (no pip).
- Work on the local branch `patapon3-windows` of `psprecomp/` (on top of part 1, `4dcc9ae`); commits authored
  "Patapon Community", **no Co-Authored-By line**; nothing is pushed.
- Talk to the user in Spanish; code, comments and commits in English.

## Recon facts the tasks rely on (part 1 dumps, DxD 1.1.4 Spanish; copies in `DECO/build/prompts_ref/`)

- `atlas_es.png` (256×128 RGBA, decoded), `atlas_x4.png` (×4 view), `gray_bars.png` / `pack_bars.png` (how the
  bars look today, without / with the HD pack).
- Label texels are RGB `7F7F7F`; alpha levels `0,17,34,51,73,102,124,157,196,221,238,255` (peak **255** in
  Spanish; other languages' ramps differ — measure each).
- Label rows have a pitch of **11 texels**: row k = y `11k .. 11k+10` for k = 0..8 (y 0..98). In a row, accents
  and capitals start at `+1`, the x-height spans `+3..+8`, descenders `+9..+10`: baseline = `11k + 9` (first
  texel row below the letters), cap height ≈ 8 texels. Further labels sit right of the pills (y ~99..113,
  x ≥ 80: "Repetir búsqueda", "Introducción manual") and right of the glyph row (y 116..127, x ≥ 125: "Cambiar
  página", "Hab. Héroe"); measure their bands.
- Glyph-row separators (language independent, inside part 1's signature): `:` x 0..1, `/` 4..7, `&` 10..16,
  y 117..127.
- Tools saved from part 1 (2026-10-20): `DECO/build/tools/p3_lang_boot.py <downs> <tag> <outdir> [ENV=V…]`
  boots a 9998 instance, picks the language `downs` rows down in the boot language menu (it saves
  `<tag>_langmenu.tga`: read it to map rows → languages; part 1 used `2` for EU English), reaches the hideout,
  and with `DUMP=<atlas hex>` dumps the atlas via `DECO/build/tools/texdump.py` (`TEXDUMP_PORT=9998`).
  Atlas addresses: DxD 1.1.4 `092E5300`, 1.1.5b `092E52C0`, vanilla `092E4DC0` (CLUT = atlas + 0x4040 in
  1.1.4; check the others, `DUMP_CLUT=` overrides). `PSPRECOMP_PROMPTS_SIG_LOG=1` logs every candidate's
  signature. Exes: `DECO/build/rtw` (1.1.4), `DECO/build/v115b/rtw`, `DECO/build/vanilla/rtw`.

## File Structure

| File | Responsibility |
|---|---|
| `runtime/include/psp_ge_texpatch.h`, `src/psp_ge_texpatch.cpp` | `recognise` returns a tag; memo keeps it; variant packing `patcher<<28 \| tag<<20 \| variant` |
| `runtime/src/stb_truetype_impl.cpp` (new), `src/psp_osk_overlay.cpp` | the single `STB_TRUETYPE_IMPLEMENTATION` TU, shared by the OSK and the bar text |
| `runtime/assets/fonts/ZenMaruGothic-Bold.ttf`, `OFL.txt` | vendored font (embedded as `g_bar_fonts`) |
| `runtime/include/psp_bar_text.h`, `src/psp_bar_text.cpp` (new) | table types, font access, cap→px, fit rule, `rgba_draw_text`, `bar_text_compose` |
| `runtime/include/psp_prompt_atlas.h`, `src/psp_prompt_atlas.cpp`, `src/psp_prompt_patcher.cpp` | spec gains label tables; recognition returns the tag; variant gains icons/text flags; switches |
| `games/patapon3/runtime/p3_prompt_labels.cpp` (+ `.h`) (new) | per-language word tables + label signatures + separators |
| `games/patapon3/runtime/p3_prompts.cpp` | attaches the tables to the spec |
| `DECO/build/tools/atlas_words.py` (new) | recon: proposes word runs from an atlas dump, renders a ×8 contact sheet with numbered boxes |
| `runtime/tests/test_ge_texpatch.cpp`, `test_prompt_atlas.cpp`, `test_bar_text.cpp` (new), `test_p3_prompt_labels.cpp` (new) | unit tests |
| `ARCHITECTURE.md`, `README.md`, `DEBUGGING.md` | seam tag, bar text, font credit, `PSPRECOMP_BAR_TEXT` |

## Review Focus

- A language whose label ramp differs (it, de): the label signature must use coverage only (like part 1) so a
  table matches, and alpha must come from that table's measured peak. Test in Task 3 (same coverage, other ramp).
- Touching words ("MenúDatos"): clearing one word's ring must not erase the neighbour before it is redrawn —
  all clears before all draws. Test in Task 3.
- Icons off + text on, and icons on + text off: the variant is never 0 when either is on, and `apply` draws only
  what is on. Test in Task 3.
- HD pack image (scale ≠ render scale): boxes, baseline and font size follow `img.w / spec.width`. Test in Task 3
  (compose at scale 2 and 3 into the same boxes).
- A character outside the font (e.g. `œ`, `’`): the word keeps the game's pixels, its box is not cleared.
  Test in Task 2.

---

### Task 1: The seam carries a recognition tag

**Files:** Modify `runtime/include/psp_ge_texpatch.h`, `runtime/src/psp_ge_texpatch.cpp`,
`runtime/src/psp_prompt_patcher.cpp` (adapt `recognise`), `runtime/tests/test_ge_texpatch.cpp`.

**Interfaces — Produces:** `uint32_t (*recognise)(const GeTexDecodeParams& p, const uint8_t* rgba)` returns
0 (not this texture) or a tag 1..255. `ge_texpatch_variant` returns
`(idx + 1) << 28 | (tag & 0xFF) << 20 | (latched & 0xFFFFF)` (0 when not recognised or latched 0); `apply`
still receives `variant & 0x0FFFFFFF`, so the tag is `(v >> 20) & 0xFF`, the patcher variant `v & 0xFFFFF`.
`ge_texpatch_begin_list` masks the latched variant to 20 bits. Header comment updated ("a patcher variant must
fit in 20 bits; the tag in 8").

- [ ] **Step 1: Failing tests** in `test_ge_texpatch.cpp`: the fake's `recognise` returns tag 7 for its marker →
  `CHECK(((v >> 20) & 0xFF) == 7)` and `CHECK((v & 0xFFFFF) == 1)`; the tag is memoised (a second bind with the
  same content hash does not call `recognise`, still tag 7); two content hashes with tags 7 and 9 give two
  different variants; `apply` sees the tag (the fake records `(v >> 20) & 0xFF`). Existing tests adapted to the
  new return type.
- [ ] **Step 2:** `cd DECO/build/rtw && ninja test_ge_texpatch && ./test_ge_texpatch` → FAIL (compile error or
  tag 0).
- [ ] **Step 3: Implement.** Memo value = `{int idx; uint8_t tag;}`. Prompts patcher's `recognise` returns 1 when
  part 1's signature matches, else 0 (labels come in Task 3).
- [ ] **Step 4:** test → PASS; `ninja` (whole runtime) OK; `ctest` all green.
- [ ] **Step 5: Commit** `feat(texpatch): recognise returns a per-texture tag carried to apply`.

### Task 2: Embedded font and the word rasteriser

**Files:** Create `runtime/src/stb_truetype_impl.cpp`, `runtime/include/psp_bar_text.h`,
`runtime/src/psp_bar_text.cpp`, `runtime/assets/fonts/{ZenMaruGothic-Bold.ttf,OFL.txt}`,
`runtime/tests/test_bar_text.cpp`. Modify `runtime/src/psp_osk_overlay.cpp` (drop its
`#define STB_TRUETYPE_IMPLEMENTATION`), `runtime/CMakeLists.txt` (embed `assets/fonts/*.ttf` as `g_bar_fonts`
with `embed_files.cmake`, add the impl TU to the runtime, add `test_bar_text` + `add_test(bar_text_tests)`).

**Interfaces — Produces** (`psp_bar_text.h`):
```cpp
struct BarWord { int x0, x1; std::string text; };            // ink columns, inclusive texels; UTF-8
struct BarRow { int y0, y1, baseline; std::vector<BarWord> words; };  // band (inclusive), baseline row
struct BarLabels { const char* name; uint64_t sig; int cap; uint8_t alpha; std::vector<BarRow> rows; };
bool bar_font_ok();                                   // embedded font parsed
bool bar_font_has(const std::string& utf8);           // every code point has a glyph
float bar_font_px_for_cap(float cap_px);              // pixel size whose 'H' is cap_px tall
float bar_text_width(const std::string& utf8, float px);   // advance width incl. kerning, unscaled
// Draws utf8 left-aligned at x, baseline at y (pixels of img), condensed by min(1, box_w / width);
// colour gray, alpha = coverage * alpha / 255, blended "over" what is there. False (nothing drawn) when
// a code point is missing.
bool rgba_draw_text(RgbaImage& img, int x, int baseline, int box_w, float px, const std::string& utf8,
                    uint8_t gray, uint8_t alpha);
```

- [ ] **Step 1: Vendor the font.** Download into a new empty dir under the scratchpad:
  `https://github.com/google/fonts/raw/main/ofl/zenmarugothic/ZenMaruGothic-Bold.ttf` and `…/OFL.txt`; check the
  TTF starts with `00 01 00 00` (or `OTTO`) and note its size; copy both to `runtime/assets/fonts/`. If the
  embed step makes CMake configure take > 30 s, still embed it whole (no subsetting tool is installed) and note
  the time in the ledger.
- [ ] **Step 2: Failing tests** (`test_bar_text.cpp`): `bar_font_ok()`; `bar_font_has("Pantalla de título")`,
  `bar_font_has("Introducción ÀÉÎÕÜßçñ")`, and a deliberately missing code point (U+E000 private use) → false;
  `bar_font_px_for_cap(32)` gives an 'H' whose ink height is 31..33 px; `rgba_draw_text` into a 200×60 clear
  image: ink only inside `[x, x + box_w)`, ink bottom of "H" on `baseline - 1`, colour 0x7F; a word wider than
  the box is condensed (ink width ≤ box_w, ≥ 0.9 × box_w); a narrow word is not stretched (ink width =
  natural ±1 px); alpha 128 halves the peak alpha; missing code point → returns false, image unchanged.
- [ ] **Step 3:** `ninja test_bar_text && ./test_bar_text` → FAIL (missing symbols).
- [ ] **Step 4: Implement.** One `stbtt_fontinfo` from `g_bar_fonts` (parsed once, mutex like
  `prepared_icon`); render glyph by glyph with `stbtt_MakeCodepointBitmapSubpixel` at scale × condense on x,
  advancing by `advance + kern`; per-glyph bitmap cache keyed by (code point, px, sx) inside a call is enough.
  UTF-8 decoding local to the file.
- [ ] **Step 5:** test PASS; `ninja` OK (OSK still links: the OSK overlay keeps working with the shared impl);
  `ctest` green.
- [ ] **Step 6: Commit** `feat(prompts): embedded Zen Maru Gothic Bold (OFL) and a word rasteriser`.

### Task 3: Bar text in the atlas composition

**Files:** Modify `runtime/include/psp_prompt_atlas.h`, `runtime/src/psp_prompt_atlas.cpp`,
`runtime/src/psp_prompt_patcher.cpp`, `runtime/src/psp_bar_text.cpp`, `runtime/tests/test_prompt_atlas.cpp`.

**Interfaces — Consumes:** Task 1 tag, Task 2 types. **Produces:**
- `PromptAtlasSpec` gains `std::vector<PromptRect> label_sig_rects; std::vector<BarLabels> labels; BarRow separators;`.
- `void bar_text_compose(const BarLabels* labels, const BarRow& separators, int cap, uint8_t gray, uint8_t alpha,
  const std::vector<PromptRect>& keep, double sx, double sy, RgbaImage& img)` (in `psp_bar_text.cpp`): words whose
  text the font cannot draw are added to `keep`; clears every other word box + half-texel ring (clipped so it
  never touches a `keep` rect) to alpha 0 / gray, then draws every word at `x0 * sx`, baseline `baseline * sy`,
  box `(x1 - x0 + 1) * sx`, px = `bar_font_px_for_cap(cap * sy)`. `labels == nullptr` → separators only.
- `prompt_atlas_variant(InputDevice d, PromptStyle s, bool icons, bool text)` → `(icons ? 1 + d * 2 + s : 0) |
  (text ? 0x10 : 0)`; `prompt_atlas_variant_decode(v, &d, &s, &icons, &text)` false when `v == 0` or `v > 0x18`.
- Prompts `recognise`: 0 if part 1's signature fails; else `2 + i` for the label table i whose `sig` equals
  `prompt_atlas_signature(rgba, w, spec.label_sig_rects)`, else 1. `apply`: icons (part 1 compose) when the
  icons flag is on, then `bar_text_compose` when the text flag is on, with `keep` = every icon cell (always)
  and `labels = tag >= 2 ? &spec.labels[tag - 2] : nullptr`; alpha = the table's (separators: 255).
- Registration: `PSPRECOMP_BAR_TEXT=off` clears the text flag; `PSPRECOMP_BUTTON_ICONS=off` clears the icons
  flag; both off → nothing registered (log line says which are on).

- [ ] **Step 1: Failing tests** (`test_prompt_atlas.cpp`, synthetic atlas + a synthetic `BarLabels` with two
  touching words in one row and one word containing U+E000): variant encode/decode round-trip for all 4 flag
  combinations, never 0 when a flag is on; compose with text on: old ink inside the redrawn boxes is gone and new
  ink is only inside the boxes (scale 1, 2, 3); the touching neighbour is fully redrawn (its ink exists after
  compose); the U+E000 word's pixels are byte-identical to the original; icon cells byte-identical when icons
  off; with a label ramp scaled ×0.5 (same coverage) the label signature is unchanged; recognition returns 1
  for an unknown label signature and 2 for the synthetic table's.
- [ ] **Step 2:** `ninja test_prompt_atlas && ./test_prompt_atlas` → FAIL.
- [ ] **Step 3: Implement** as in Interfaces.
- [ ] **Step 4:** test PASS; `ninja`; `ctest` green.
- [ ] **Step 5: Commit** `feat(prompts): redraw the help-bar labels with the bar font (text flag, language tag)`.

### Task 4: Recon and transcription — every language, every release

**Files:** Create `DECO/build/tools/atlas_words.py`, `games/patapon3/runtime/p3_prompt_labels.{h,cpp}`,
`runtime/tests/test_p3_prompt_labels.cpp` (registered in `runtime/CMakeLists.txt` inside the
`if(PSPRECOMP_GAME STREQUAL "patapon3")` block, like `test_p3_achievements`). Modify
`games/patapon3/runtime/p3_prompts.cpp`.

**Interfaces — Produces:** `const std::vector<BarLabels>& p3_bar_labels();` `BarRow p3_bar_separators();`
`std::vector<PromptRect> p3_label_sig_rects();` (rects covering every label area and none of part 1's signature
rects, e.g. `{0,0,256,99}`, `{80,99,176,17}`, `{125,116,131,12}` — confirm against the dumps).

- [ ] **Step 1: Dumps.** For each language of the boot menu (read `<tag>_langmenu.tga` first to map rows):
  `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 DUMP=092E5300 SHOTS=keyboard python -I build/tools/p3_lang_boot.py
  <downs> <lang> build/prompts_ref/dumps PSPRECOMP_PROMPTS_SIG_LOG=1` (DxD 1.1.4). Then Spanish + English with
  1.1.5b (`092E52C0`; `P3DRIVE_EXE=../v115b/rtw/psprecomp_runtime.exe P3DRIVE_DISC0=C:/Users/torso/Documents/deco/build/v115b/disc0`)
  and vanilla (`092E4DC0`; same with `build/vanilla/…`) and
  compare their label coverage with 1.1.4's; transcribe only atlases that differ. Expected: one PNG per
  (release, language) that differs, all 256×128.
- [ ] **Step 2: `atlas_words.py <atlas.png> <out_x8.png> [--json out.json]`**: finds row bands (runs of texel
  rows with ink), per band splits ink columns into runs at gaps ≥ 2 empty columns, prints
  `row y0 y1 baseline | run x0 x1` and draws a ×8 sheet with each run's box and index. Baseline = first row
  below the x-height block (Spanish rows: `11k + 9`).
- [ ] **Step 3: Transcribe** each language into `p3_prompt_labels.cpp`: one `BarRow` per band, one `BarWord` per
  word run with its text read off the ×8 sheet; split touching words by the visible boundary; keep multi-word
  labels as one run only when the gap between their words is < 2 columns (then the run's text includes the
  space). Store `cap` (8 unless measured otherwise), `alpha` (peak of that dump), `name` ("dxd114-es" …) and
  `sig` (from the `[PROMPTS]` log or computed by the tool with the same FNV-1a coverage hash over
  `p3_label_sig_rects()`; add a `--sig` option to the tool that prints it). Separators `:` `/` `&` go in
  `p3_bar_separators()`.
- [ ] **Step 4: Failing table test** first, with an empty table list: for every table, every word
  `bar_font_has(text)`, `x0 <= x1`, inside 256×128 and inside its row; the word's natural width at
  `bar_font_px_for_cap(cap * 4)` ≤ `(x1 - x0 + 1) * 4 / 0.75`; signatures unique; no two words of a row overlap;
  every table has ≥ 40 words; there is one table per distinct (release, language) dump of Step 1 (the test lists
  the expected names, so the empty list fails). Then fill the tables until it passes.
- [ ] **Step 5: First look.** Build, boot the 1.1.4 Spanish hideout at render scale 4 on 9998 and compare a ×4
  crop of the bar with `build/prompts_ref/gray_bars.png` (Task 5 Step 2 does every language).
- [ ] **Step 6:** `ninja`; `ctest` green. **Commit** `feat(p3): help-bar word tables for every language and release`.

### Task 5: Docs, live verification, deploy

**Files:** `ARCHITECTURE.md` (seam tag; bar text in the prompts patcher), `README.md` (Zen Maru Gothic credit next
to Kenney: "Zen Maru Gothic by Yoshimichi Ohira, SIL Open Font License 1.1"), `DEBUGGING.md`
(`PSPRECOMP_BAR_TEXT=off`, `atlas_words.py`), `runtime/include/psp_prompt_atlas.h` comments (switches).

- [ ] **Step 1: Docs** as above; commit `docs: sharp help-bar text, bar font credit`.
- [ ] **Step 2: Live, 9998, scale 4, RTC fixed.** For every transcribed language (1.1.4) a hideout shot with
  keyboard and xbox; Spanish also with the HD pack (`PSPRECOMP_TEXTURES=C:/Users/torso/Documents/deco/build/e2e_portable3/textures/INFN00001`,
  as in part 1), with `PSPRECOMP_BUTTON_ICONS=off`, and with `PSPRECOMP_BAR_TEXT=off` (must look
  like `gray_bars.png`); 1.1.5b and vanilla Spanish. Crop the bars ×4 into one contact sheet per language
  (`build/prompts_ref/after_<lang>.png`) and **read every word** against the original: right text, nothing cut,
  no piece of a neighbour, brightness like the original. Also the barracks / forge / pause bars: run
  `TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/bartext_on`
  and look at `08_cuartel_menu`, `05_armeria_shop`, `02_totem_menu`.
- [ ] **Step 3: Gates.** `ctest` all green; `rt_regress.py` of a `PSPRECOMP_BAR_TEXT=off` tour against
  `build/tour/prompts_off` → PASS (nothing else changed); the default tour (`bartext_on`) against the same base:
  report the struct % per checkpoint and confirm by eye that differences are only in the bars; `perf_hideout.py`
  off vs on: same GE ms and binds (part 1: 4.1 ms, 27 binds).
- [ ] **Step 4: Deploy for the user's play check** exactly like part 1: copy the new exes (1.1.4, 1.1.5b, vanilla
  rebuilt in `build/v115b/rtw`, `build/vanilla/rtw`) to `build/e2e_portable3` and `build/e2e_portable`, backing up
  the previous ones to `build/backup_pre_bartext_<date>/`. Never touch an instance on 9999.
- [ ] **Step 5: Hand-off.** Write `build/autopilot/EN_QUE_NOS_QUEDAMOS_<date>.md` (Spanish: what changed, how to
  try it, decisions, contact sheets), update the memory `p3-pc-button-prompts.md`, and report to the user in
  Spanish. Push only after the user's OK, on a new branch from `p3/main` with parts 1 + 1.1 cherry-picked and
  **without** `067ac6e` (debug menu, LOCAL ONLY) or `f639180` (icon).
