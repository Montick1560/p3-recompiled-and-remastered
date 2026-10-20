# PC button prompts, part 2 — the mission command list — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **The user asked for this plan to be executed natively** ("de forma nativa" = superpowers:executing-plans,
> inline, no helper models / subagents; the final review is a self-review, say so in the report).

**Goal:** The command list at the bottom of every mission ("Avanzar: □·□·□·○ …") shows the player's keys / pad
buttons and sharp labels, like the help bars of parts 1 / 1.1, in every language and release.

**Architecture:** The prompts patcher (one texture patch of the seam, `psp_ge_texpatch.h`) learns to hold several
atlas specs; the recognition tag packs the atlas index with the atlas's own tag. Patapon 3's module registers a
second spec — the 128×64 command-list atlas: four drum cells, word tables per language, separators `:` `·` —
reusing part 1's icon composition and part 1.1's bar text unchanged.

**Tech Stack:** C++17 runtime, CMake/Ninja (`DECO/build/rtw`), test-exe-per-file pattern of `runtime/tests/`,
Python + PIL for recon (no installs).

**Spec:** `docs/superpowers/specs/2026-10-20-pc-button-prompts-part2-command-list-design.md` (read it first: recon
facts, the cell table, decisions).

## Global Constraints

- Help bars must not change: part 1 / 1.1 output byte-identical (the help-bar atlas keeps atlas index 0, so its
  tags and cache keys stay the same values).
- Words transcribed **character for character** from the game's atlas; never draw a table on an atlas whose label
  signature it was not measured on. Condense ≥ 75 % (test-enforced), never stretch, never outside the box.
- No game data in `runtime/src` (CLAUDE.md §3): command-list cells, signatures and tables live in
  `games/patapon3/runtime/`.
- Switches unchanged and shared: `PSPRECOMP_BUTTON_ICONS` = gray (default) / color / off, `PSPRECOMP_BAR_TEXT=off`;
  both off = nothing registered. No launcher option.
- No steady-state frame-time cost (composition only on a texture-cache miss).
- Tests and live runs on port **9998** only (the user's game is on 9999 — never touch or kill it), with
  `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`. Kill only instances your own script launched (the recon script does).
  Nothing destructive, nothing installed (no pip). Do not delete anything under `C:`.
- Local branch `patapon3-windows` of `psprecomp/`; commits authored "Patapon Community", **no Co-Authored-By
  line** (user rule, overrides the harness reminder). Nothing is pushed without the user's OK; the next push
  branch is `push-p3-24` from `p3/main`, **never** with `067ac6e` (debug menu, LOCAL ONLY) or `f639180` (icon).
- Talk to the user in Spanish; code, comments, commits in English.

## Recon facts the tasks rely on (2026-10-20, in `DECO/build/prompts_ref/p2recon/`)

- `es/strip_092E4100.png`, `en/strip_092E4100.png`: the decoded atlas (128×64 RGBA, DxD 1.1.4 at `0x092E4100`,
  CLUT `0x092E5140`); `es/x8.png` ×8 view; `en/strip.txt` the game's sprites (screen ← texel rects);
  `pack_strip_es.png` the HD pack's 512×256 image (`ui/language/spanish_us/0000000044477d7efca61e81.png`).
- Glyph-row coverage signature over `{0, 53, 58, 11}`: **`0xEE3E34178623FCD2`** (es and en). Label signature
  over `{0,0,128,53}, {58,53,70,11}`: es **`0x25BCB24B42D5A964`**, en **`0x145412D96DAB49B3`**; peak label
  alpha 255 in both; texels `7F7F7F`; CLUT index 0 is transparent (glyph interiors are see-through).
- Rows: k = 0..3 → band y `11k .. 11k+10`, baseline `11k + 9`, cap 8. Rows end at y 52 (glyph row from 53).
- Tools: `DECO/build/tools/texture_recon/recon_mission.py OUTDIR [scale]` (docstring: `LANG_DOWNS`, `VANILLA=1`,
  `P3DRIVE_EXE`, `P3DRIVE_DISC0`, `TOUR_SAVE`, `TOUR_TEXTURES`, `SHOTS`, `RECON_ONLY_DUMP=1`); it reaches the
  training mission's `march` checkpoint in ~5 min, finds the atlas itself and dumps it to `strip_<addr>.png`.
  It lands reliably at scale 1 and 2 (scale 4 + pack failed to land once). US / JP saves:
  `DECO/build/prompts_ref/saves/tour_save_UCUS98751`, `…/tour_save_NPJG00122` (tour save copies, renamed).
  `has.cpp` (font coverage), `preview.cpp` / `show.py` (offline compose), `atlas_words.py` — see the kit README.
  Build: `cd DECO/build/rtw && ninja` (1.1.4); `DECO/build/v115b/rtw`, `DECO/build/vanilla/rtw` for the others;
  `ctest` from the same dir (89 tests green before this plan).

## File Structure

| File | Responsibility |
|---|---|
| `runtime/include/psp_prompt_atlas.h`, `src/psp_prompt_atlas.cpp` | spec `name`; pure multi-atlas recognise / apply with the packed tag |
| `runtime/src/psp_prompt_patcher.cpp` | holds up to 4 specs (register adds / replaces by name); logs per atlas |
| `games/patapon3/runtime/p3_command_labels.{h,cpp}` (new) | command-list word tables, separators, label rects |
| `games/patapon3/runtime/p3_prompts.cpp` | registers the help-bar spec (unchanged) + the command-list spec |
| `runtime/tests/test_prompt_atlas.cpp` | multi-atlas tests |
| `runtime/tests/test_p3_command_labels.cpp` (new) | table tests (registered next to `test_p3_prompt_labels`) |
| `DECO/build/tools/atlas_words.py`, `texture_recon/preview.cpp` | `--layout command`; preview of any registered atlas |
| `ARCHITECTURE.md`, `DEBUGGING.md`, `README.md` | multi-atlas patcher; command list; recon tool |

## Review Focus

- A texture that matches *both* specs' size/format filter (e.g. another 128×64 CLUT4): only its signature
  decides; the first spec that recognises it wins, others are not consulted. Test in Task 1.
- A spec with more than 30 label tables would overflow its 5-bit local tag into the next atlas index: the local
  tag must saturate to 1 (icons + separators only) instead. Test in Task 1.
- The help-bar values must not move: atlas 0's packed tag equals its part 1.1 tag for every input. Test in Task 1.
- The HD pack image (512×256, scale 4 of the spec): cells, boxes and baselines follow `img.w / spec.width`
  (part 1 / 1.1 tested it for the help bar; Task 2 runs the preview on `pack_strip_es.png`).
- A language whose command names use a character the font lacks (Polish ł ś …): its words keep the game's
  pixels; the drum icons still change. Task 3 checks with `has.cpp` and leaves such a table out.

---

### Task 1: The prompts patcher holds several atlases

**Files:** Modify `runtime/include/psp_prompt_atlas.h`, `runtime/src/psp_prompt_atlas.cpp`,
`runtime/src/psp_prompt_patcher.cpp`, `runtime/tests/test_prompt_atlas.cpp`.

**Interfaces — Produces** (`psp_prompt_atlas.h`):
```cpp
// PromptAtlasSpec gains:
const char* name = "help-bar";   // log name of the atlas ("[PROMPTS] <name> atlas recognised ...")
constexpr int kPromptAtlasMax = 4;        // atlases one patcher holds
constexpr uint32_t kPromptAtlasTagStride = 32;  // packed tag = index * 32 + local tag (1..31)
// 0 = none; else index * kPromptAtlasTagStride + local, local = prompt_atlas_recognise() of the first spec
// (in order) whose width / height / format match and whose recognise is non-zero, saturated to 1 when it
// exceeds 31 (more than 30 label tables).
uint32_t prompt_atlases_recognise(const std::vector<PromptAtlasSpec>& specs, int width, int height, int format,
                                  const uint8_t* rgba);
// v = packed tag << 20 | prompt_atlas_variant(): prompt_atlas_apply() of spec index (tag / 32) with the local
// tag; nothing when the index is out of range.
void prompt_atlases_apply(const std::vector<PromptAtlasSpec>& specs, uint32_t v, const ControlMap& map,
                          RgbaImage& img);
```
`psp_prompts_register_atlas(PromptAtlasSpec spec)` now **adds** the atlas (a spec with the same `name` replaces
the earlier one; beyond `kPromptAtlasMax` it logs and ignores); the seam patcher is registered once. `candidate`
= any spec's size/format; `recognise` = `prompt_atlases_recognise`; `apply` = `prompt_atlases_apply`. The
recognised log line keeps its text for the help bar: `"[PROMPTS] %s atlas recognised (%dx%d) at 0x%08X, labels %s"`
with `name` (live scripts grep "help-bar atlas recognised"). `PSPRECOMP_PROMPTS_SIG_LOG=1` logs each candidate's
signatures with the name of the spec whose size it matched. The registration log line lists the atlas names.

- [ ] **Step 1: Failing tests** (`test_prompt_atlas.cpp`, reuse `synthetic_atlas()` / `label_atlas()` and their
  specs): `multi_atlas_tags()` — specs `{A (size of synthetic_atlas, name "a"), B (a second spec of another
  size, name "b", with one label table)}`: A's image → `prompt_atlases_recognise == prompt_atlas_recognise(A)`
  (index 0 unchanged, Review Focus 3); B's image with its table's labels → `1 * 32 + 2`; an image of B's size
  but wrong coverage → 0; a spec C with B's size placed *after* B that would also accept B's image → still
  `32 + 2` (first wins, Review Focus 1). `local_tag_saturates()` — a spec with 31 label tables whose last
  table matches → local tag 1 (Review Focus 2). `multi_atlas_apply_matches_single()` — for B at scales 1 and 3,
  `prompt_atlases_apply(specs, (32 + 2) << 20 | variant)` gives a byte-identical image to
  `prompt_atlas_apply(B, 2 << 20 | variant)`; an index ≥ specs.size() leaves the image unchanged.
- [ ] **Step 2:** `cd DECO/build/rtw && ninja test_prompt_atlas && ./test_prompt_atlas` → FAIL (undefined
  `prompt_atlases_recognise`).
- [ ] **Step 3: Implement** as in Interfaces (pure functions in `psp_prompt_atlas.cpp`; the patcher keeps a
  `std::vector<PromptAtlasSpec>`).
- [ ] **Step 4:** test PASS; `ninja` (whole runtime) OK; `ctest` all green (89/89).
- [ ] **Step 5: Live no-change check:** `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I
  build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/p2_t1` then
  `python -I build/tools/rt_regress.py build/tour/bartext_on2 build/tour/p2_t1` → only `14_hideout_sukopon` /
  `15_hideout_obelisco` may fail (animation phase, known); every other checkpoint 0.00 %.
- [ ] **Step 6: Commit** `feat(prompts): the prompts patcher holds several atlases`.

### Task 2: The command-list atlas — DxD 1.1.4 Spanish and EU English

**Files:** Create `games/patapon3/runtime/p3_command_labels.{h,cpp}`, `runtime/tests/test_p3_command_labels.cpp`
(in `runtime/CMakeLists.txt` inside the `patapon3` block, built like `test_p3_prompt_labels` with
`p3_command_labels.cpp`; the game module itself globs `games/patapon3/runtime/*.cpp`, so re-run CMake once
— `ninja` does it — for the new file to join the exe). Modify `games/patapon3/runtime/p3_prompts.cpp`, `DECO/build/tools/atlas_words.py`,
`DECO/build/tools/texture_recon/preview.cpp` (+ its build line in the kit README).

**Interfaces — Consumes:** Task 1. **Produces** (`p3_command_labels.h`):
```cpp
const std::vector<BarLabels>& p3_command_labels();   // one table per distinct (release, language) atlas
BarRow p3_command_separators();                       // ':' and '·' of the glyph row
std::vector<PromptRect> p3_command_label_sig_rects(); // {{0,0,128,53}, {58,53,70,11}}
```
Command-list spec in `p3_prompts_register()` (after the help-bar one, so the help bar stays index 0):
`name "command-list"`, 128×64, `GE_TFMT_CLUT4`, `sig_rects {{0,53,58,11}}`, `sig_hashes {0xEE3E34178623FCD2}`,
`gray 0x7F`, `hole 0x17`, cells (the UV cells the game crops, spec §2):
`{Square,14,53,10,10} {Circle,25,53,10,10} {Triangle,36,53,10,10} {Cross,47,53,10,10}`, labels / separators /
`label_sig_rects` from `p3_command_labels`, `separator_cap` chosen in Step 5. File header comment describes the
atlas like `p3_prompts.cpp` does for the help bar (address, CLUT, rows, cells, recon date).

Tables (ink columns measured 2026-10-20; cap 8, alpha 255):
```
dxd114-es  sig 0x25BCB24B42D5A964
  {0,10,9}   {40,70,"Defen."} {76,109,"Cargar"}
  {11,21,20} {1,32,"Atacar"} {38,63,"Atrás"} {66,101,"Invocar"}
  {22,32,31} {35,62,"Saltar"} {66,105,"Avanzar"}
  {33,43,42} {1,40,"Purificar"}
dxd114-en  sig 0x145412D96DAB49B3
  {0,10,9}   {1,40,"Onward"} {43,77,"Defend"} {80,116,"Charge"}
  {11,21,20} {1,32,"Attack"} {35,70,"Retreat"} {72,113,"Summon"}
  {22,32,31} {1,25,"Party"} {28,54,"Jump"}
separators {53, 62, 61, {{0,1,":"}, {7,9,"·"}}}
```

- [ ] **Step 1: Failing table test** `test_p3_command_labels.cpp` (model: `test_p3_prompt_labels.cpp`, with the
  bounds of a 128×64 atlas and rows ending at y ≤ 52): expected names `{"dxd114-es", "dxd114-en"}` (Task 3
  extends the list); every word `bar_font_has`, inside its row, in order, fits at ≥ 75 % and ≥ 0.6 of its box
  at cap 8 × 4; signatures unique; ≥ 7 words per table; separators = 2 words inside y 53..62;
  `p3_command_label_sig_rects()` never overlaps `{0,53,58,11}`. With empty function bodies → FAIL.
- [ ] **Step 2:** `ninja test_p3_command_labels && ./test_p3_command_labels` → FAIL (names / counts).
- [ ] **Step 3: Implement** the tables above and register the spec.
- [ ] **Step 4:** test PASS; `ninja`; `ctest` all green.
- [ ] **Step 5: Offline look.** `atlas_words.py --layout command` (bands k = 0..4, y1 = min(11k+10, 52);
  `PART1_RECTS = [(0,53,58,11)]`, `LABEL_RECTS` = the two rects above; `--sig` prints both signatures — must
  print the values above for `es/` and `en/`). `preview.cpp`: keep every registered spec, pick the one whose
  width/height match the input PNG ×k (k = PNG width / spec width), apply via `prompt_atlases_*`. Run
  `show.py` on `es/strip_092E4100.png` at ×4, on `en/…` at ×4 and on `pack_strip_es.png` (×1 of the pack image)
  for keyboard: icons fill their cells, words in place, nothing outside the boxes. Pick `separator_cap` so the
  new `:` is as tall as the original (5 texels → 20 px ±4 at ×4) and the `·` sits at the same height as the old
  dot (y 57..59); record the value as a ruling. Rebuild, ctest.
- [ ] **Step 6: First live look**, scale 2: `SHOTS=keyboard,xbox,ps4,ps5 TOUR_RT_PORT=9998
  PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/texture_recon/recon_mission.py
  C:/Users/torso/Documents/deco/build/prompts_ref/p2live/es 2` — the log line `command-list atlas recognised`
  appears in `build/logs/tour_rt.log`; crop y ≥ 236 (×2) of each `march_<dev>.tga` and read every cell
  against the mapping (default keyboard map: ✕ = first key of CROSS, etc.; Xbox A/B/X/Y as mapped). Same with
  `LANG_DOWNS=2` (EU English).
- [ ] **Step 7: Commit** `feat(p3): mission command list shows PC buttons and sharp labels (es, en)`.

### Task 3: Every other language and release

**Files:** Modify `games/patapon3/runtime/p3_command_labels.cpp`, `games/patapon3/runtime/p3_prompts.cpp`
(extra glyph-row hashes if any), `runtime/tests/test_p3_command_labels.cpp` (expected names).

- [ ] **Step 1: Dumps** with `RECON_ONLY_DUMP=1 recon_mission.py <out> 1` (scale 1: fast, lands reliably), out =
  `C:/Users/torso/Documents/deco/build/prompts_ref/p2recon/<name>`:
  DxD 1.1.4 `LANG_DOWNS` 0 (ja, `TOUR_SAVE=…/saves/tour_save_NPJG00122`), 1 (us, `…/tour_save_UCUS98751`), 3 (it),
  4 (de), 6 (fr); DxD 1.1.5b es + en (`P3DRIVE_EXE=../v115b/rtw/psprecomp_runtime.exe
  P3DRIVE_DISC0=C:/Users/torso/Documents/deco/build/v115b/disc0`); vanilla EU (`VANILLA=1
  P3DRIVE_EXE=../vanilla/rtw/psprecomp_runtime.exe P3DRIVE_DISC0=C:/Users/torso/Documents/deco/build/vanilla/disc0`)
  rows 0..8. Run them one after another (one 9998 instance at a time). Read each `langmenu.tga` once to confirm
  the row → language mapping. If a boot does not land (log `hideout landing FAILED`), rerun once, then note it
  in the ledger and go on.
  Expected: one `strip_<addr>.png` per run (address may differ per release/language — the script finds it).
- [ ] **Step 2: Classify.** `atlas_words.py --layout command --sig` on every dump. Same glyph sig and same label
  sig as an existing table → shares it (no new table). Same label sig, other glyph sig → add the glyph hash to
  `sig_hashes` with a comment naming the languages. New label sig → a new table named like part 1.1
  (`dxd114-us`, `dxd114-fr`, `dxd114-it`, `dxd114-de`, `dxd114-jp`, `van-pt`, `van-nl`, `van-ru`, `van-pl`…).
- [ ] **Step 3: Font coverage.** Put every new language's words in a UTF-8 file and run `has.exe` (kit README
  build line): a language with a missing character gets **no table** (its atlas stays the game's text; icons
  still change) — ruling + a line in the spec's decisions.
- [ ] **Step 4: Failing test first:** add the new names to the test's expected list → FAIL.
- [ ] **Step 5: Transcribe** (ink columns from `atlas_words.py`, words read off its ×8 sheet / `--strips`;
  Japanese words as the game draws them, cap measured — part 1.1's Japanese help bar used cap 6) until the test
  passes; `show.py` preview of each new table at ×4 (read every word).
- [ ] **Step 6:** `ninja` (1.1.4, 1.1.5b, vanilla build dirs), `ctest` green. **Commit**
  `feat(p3): command-list word tables for every language and release`.

### Task 4: Docs, live verification, gates, deploy, hand-off

**Files:** `ARCHITECTURE.md` (prompts patcher holds several atlases; the command list), `DEBUGGING.md`
(`[PROMPTS] command-list atlas recognised`, `recon_mission.py`), `README.md` (the command list joins the help
bars in the PC-prompts feature line), header comments of `psp_prompt_atlas.h`.

- [ ] **Step 1: Docs**; commit `docs: PC button prompts in the mission command list`.
- [ ] **Step 2: Live sweep**, 9998, scale 2, `SHOTS=keyboard,xbox,ps4,ps5`: every language of Task 3 (1.1.4),
  Spanish also with `PSPRECOMP_BUTTON_ICONS=color`, `=off`, `PSPRECOMP_BAR_TEXT=off`, and the HD pack
  (`TOUR_TEXTURES=C:/Users/torso/Documents/deco/build/e2e_portable3/textures/INFN00001`, scale 1 — lands
  there), 1.1.5b es and vanilla es. Contact sheet `build/prompts_ref/p2_after_<lang>.png` (strip rows, game
  vs keyboard vs xbox) and **read every word and every icon**. Also confirm the hideout help bar is unchanged in
  each shot set (`help-bar atlas recognised` still logged).
- [ ] **Step 3: Gates.** `ctest` all green. Tour with both switches off vs `build/tour/prompts_off` →
  `RT_REGRESS PASS`. Default tour vs `build/tour/bartext_on2`: checkpoints before the mission 0.00 % (14/15 may
  vary, known); the mission ones (`25_march`, `26_attack_wall`, `27_attack_combo`) differ **only** in rows
  y ≥ 240 — check with a short PIL script comparing the crops y < 240 (max channel diff ≤ 24 per 8×8 block,
  like `gfx_score`). `perf_hideout.py` off vs on: GE ms and binds as before (4.1 ms, 27 binds).
- [ ] **Step 4: Deploy for the user's play check** like part 1.1: copy the new exes (1.1.4, 1.1.5b, vanilla) to
  `build/e2e_portable3` and `build/e2e_portable`, the previous ones first to `build/backup_pre_cmdlist_<date>/`.
- [ ] **Step 5: Hand-off.** Final self-review of the branch (executing-plans, no subagent) → ledger. Write
  `build/autopilot/EN_QUE_NOS_QUEDAMOS_<date>.md` (Spanish: what changed, how to try it — a mission, the strip at
  the bottom —, decisions incl. the rulings, contact sheets), update memory `p3-pc-button-prompts.md`, report to
  the user in Spanish. **Do not push** until the user says so; then branch `push-p3-24` from `p3/main`,
  cherry-pick this plan's commits (spec/plan included), build + ctest there, `git push p3 push-p3-24:main`.
