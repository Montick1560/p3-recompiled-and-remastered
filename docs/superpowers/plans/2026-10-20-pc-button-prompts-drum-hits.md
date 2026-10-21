# PC button prompts — the drum-hit words — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> The user runs these plans natively ("de forma nativa" = superpowers:executing-plans inline, no helper models,
> final review = self-review, say so in the report) unless they say otherwise.

**Goal:** The PATA / PON / CHAKA / DON words that fly out on every drum hit show the player's mapped key / pad
button instead of the PSP symbol, switching with the last input device (at least DxD 1.1.4 es and en).

**Architecture:** Each drum word is its own 128×128 CLUT4 texture in VRAM. The prompts patcher (one seam
patcher, `psp_prompt_patcher.cpp`) grows from 4 to 8 atlas specs and learns an opaque cell background (so a
redrawn cell inside the drum's black disc does not become a hole). Patapon 3's module registers four drum specs
(one per drum texture) after the help bar and the command list; the existing icon composition draws the button.

**Tech Stack:** C++17 runtime, CMake/Ninja (`DECO/build/rtw`), test-exe-per-file pattern of `runtime/tests/`,
Python + PIL for recon / live checks (no installs).

**Spec:** `docs/superpowers/specs/2026-10-20-pc-button-prompts-drum-hits-design.md` (recon facts, decisions).

## Global Constraints

- Help bars and command list must not change: byte-identical output (they keep atlas indices 0 and 1; the new
  cell background defaults to "none" = today's transparent clear).
- Packed tag = index × 32 + local, 8 bits: at most 8 atlases (7 × 32 + 31 = 255).
- No game data in `runtime/src` (CLAUDE.md §3): drum addresses, cells, signatures live in `games/patapon3/runtime/`.
- Switches unchanged: `PSPRECOMP_BUTTON_ICONS` = gray (default) / color / off; no launcher option.
- No steady-state frame-time cost (composition only on a texture-cache miss).
- Tests / live runs on port **9998** only, `PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`; never touch the user's game
  on 9999; kill only instances your own scripts launched (`to_march.py` 'Q'). Nothing destructive, nothing installed.
- Local branch `patapon3-windows` of `psprecomp/`; commits authored "Patapon Community", **no Co-Authored-By line**
  (user rule). Push only after the user's OK: branch `push-p3-26` from `p3/main`, never with `067ac6e` (debug
  menu, LOCAL ONLY) or `f639180` (icon).
- Talk to the user in Spanish; code, comments, commits in English.

## Recon facts the tasks rely on (2026-10-20, `DECO/build/prompts_ref/p3drums/`)

- `pata_041302C0.png`, `pon_041324C0.png`: decoded drum textures (DxD 1.1.4 es, tour save, training mission).
  128×128, CLUT4, swizzled; texture address = (bufw high byte 0x44 → 0x04) << 24 | low bits of `[DL] ta=`;
  CLUTs `0x04132300` / `0x04134500`. White symbol ink ≈ x 57..68, y 88..99; opaque black disc around it.
- The word shows 0.2–0.4 s after the press. Tools in `DECO/build/tools/texture_recon/` (README):
  `to_march.py OUTDIR 2` (boots 9998 to the training mission, then obeys `OUTDIR/cmd.txt`; mission ≈ 40 s),
  `hitlog.py OUTDIR` (`DRUMS=chaka` one drum per run: only the first press of a run showed its word under a D log),
  `hit_textures.py` (textures present in only some logged lists = the drum words), `texdump.py out.png ADDR 128
  128 128 CLUT 4 1` (`TEXDUMP_PORT=9998`), `hitshots.py OUTDIR [device]` (burst shots of all four words, no D log),
  `preview.cpp` / `show.py` (offline composition; `PREVIEW_DEVICE`, `PREVIEW_RECOGNISE`).
- `ctest` from `DECO/build/rtw`: 90 tests green before this plan.

## File Structure

| File | Responsibility |
|---|---|
| `runtime/include/psp_prompt_atlas.h`, `src/psp_prompt_atlas.cpp` | `kPromptAtlasMax` 8; `PromptAtlasSpec::cell_bg` |
| `runtime/tests/test_prompt_atlas.cpp` | 8-atlas tags, cell background |
| `games/patapon3/runtime/p3_prompts.cpp` | registers the four drum specs after the command list |
| `runtime/tests/test_p3_drum_prompts.cpp` (new) | the drum specs on the real dumps (recognised, cell inside the disc, no hole) |
| `DECO/build/tools/texture_recon/*` | recon / live scripts (already saved) |
| `ARCHITECTURE.md`, `DEBUGGING.md`, `README.md` | drum words join the prompt atlases |

## Review Focus

- A filled cell whose bilinear ring (half a source texel at ×k) leaves the black disc: a black square edge would
  show on the word art. Task 3 tests every cell + ring is inside the disc on the real dumps at ×1 and ×4.
- Icon edges over the opaque background: semi-transparent icon pixels must blend onto the black, not leave
  alpha < 255 texels in the cell (a see-through fringe). Task 1 test.
- Another 128×128 CLUT4 texture (there are many: effects, faces) must never be recognised as a drum: only the
  exact coverage hashes match; Task 3 checks the four drum hashes are distinct and that a non-drum dump
  (`DECO/build/prompts_ref/p3drums/` keeps one, e.g. the enemy mouth sheet) is rejected.
- Text flag on, icons off: a drum spec has no labels and no separators, so its texture must stay byte-identical.
  Task 1 test.
- The HD pack image of a drum (if the pack replaces it): the cell lands at `img.w / spec.width`; Task 4 looks at it
  live and reports if the pack's disc is elsewhere.

---

### Task 1: The patcher holds 8 atlases and can keep a cell's background

**Files:** Modify `runtime/include/psp_prompt_atlas.h`, `runtime/src/psp_prompt_atlas.cpp`,
`runtime/tests/test_prompt_atlas.cpp`.

**Interfaces — Produces** (`psp_prompt_atlas.h`):
```cpp
constexpr int kPromptAtlasMax = 8;  // was 4; index * 32 + local still fits the seam's 8-bit tag
struct PromptAtlasSpec {
    // ... existing fields ...
    uint32_t cell_bg = 0;  // 0 = a redrawn cell (and its bilinear ring) is cleared to transparent (today);
                           // else 0xAARRGGBB: the cleared area is filled with it first and the icon is
                           // alpha-composited over it (a drum's opaque black disc)
};
```

- [ ] **Step 1: Failing tests** in `test_prompt_atlas.cpp`:
  `eight_atlases()` — 8 specs of distinct sizes (`width = 8 << i`… any 8 distinct width/height pairs, each with a
  matching synthetic image and its own `sig_hashes`): the image of spec 7 → `prompt_atlases_recognise == 7 * 32 + 1`;
  a 9th spec is never consulted (an image only it matches → 0).
  `cell_bg_fills_opaque(k)` for k = 1, 4 — a 128×128 image, opaque black (`0xFF000000`) disc everywhere around
  a cell `{Cross, 57, 87, 12, 12}`, `cell_bg = 0xFF000000`, `gray = 0xFE`, `hole = 0x00`: after
  `prompt_atlas_compose(Xbox, Color)` every texel of the cell and its ring has alpha 255 (no hole, no fringe) and
  the cell has non-black icon ink; texels outside cell + ring are unchanged.
  `cell_bg_default_unchanged()` — the existing `spec_for(synthetic_atlas())` composed with and without an explicit
  `cell_bg = 0` gives byte-identical images (default = today).
  `icons_off_no_labels_untouched()` — a spec with no labels and no separators, `prompt_atlas_apply` with
  `prompt_atlas_variant(Keyboard, Gray, false, true)` leaves the image byte-identical.
- [ ] **Step 2:** `cd DECO/build/rtw && ninja test_prompt_atlas && ./test_prompt_atlas` → FAIL to compile
  (`cell_bg` unknown) — then with the field added but unused, `cell_bg_fills_opaque` and `eight_atlases` fail.
- [ ] **Step 3: Implement**: `kPromptAtlasMax = 8`; in `prompt_atlas_compose`, when `spec.cell_bg != 0` fill the
  cleared rectangle (cell + ring) with `cell_bg` instead of alpha 0, draw the icon composited over it (source-over),
  and skip the "transparent texels take the gray" pass for that cell. Update the header comment ("up to 8").
- [ ] **Step 4:** test PASS; `ninja` (whole runtime) OK; `ctest` all green (90/90).
- [ ] **Step 5: Commit** `feat(prompts): eight prompt atlases and an opaque cell background`.

### Task 2: Recon of the four drum textures (es, en) and the HD pack

**Files:** none in the repo; outputs in `DECO/build/prompts_ref/p3drums/` (+ a `notes.txt` there).

- [ ] **Step 1: CHAKA and DON dumps.** For each of `chaka`, `don` (one boot each): `to_march.py
  C:/Users/torso/Documents/deco/build/prompts_ref/p3drums/run_<drum> 2` (background), wait for `READY` in its log,
  then `DRUMS=<drum> HIT_DELAY=0.3 python -I hitlog.py <same dir>`, wait 10 s, `python -I hit_textures.py` →
  the 128×128 texture present in only the hit lists; `texdump.py <dir>/<drum>_<ADDR>.png <ADDR> 128 128 128 <CLUT> 4 1`
  with the 0x04 prefix; then `echo Q >> <dir>/cmd.txt`. Expected: a word + rings + white symbol image like PATA/PON.
  Also keep one non-drum 128×128 CLUT4 dump of the same run as `p3drums/other_<ADDR>.png` (Review Focus 3).
- [ ] **Step 2: EU English.** Same as Step 1 with `LANG_DOWNS=2` for all four drums (four boots, or one boot
  with the burst approach + `PSPRECOMP_PROMPTS_SIG_LOG=1` once Task 3's specs exist: the `candidate ... (drum-…)`
  lines give address and signature of every drum texture). Compare coverage with Spanish (`atlas_words.py`-style
  FNV over the whole texture: `prompt_atlas_signature` over `{0,0,128,128}`); same → shared.
- [ ] **Step 3: Cells.** For each drum dump: the symbol's ink box (white texels, min RGB > 180, in y 80..110) and
  the black disc (opaque texels with max RGB < 40 connected to the symbol, flood fill from the symbol's centre
  through dark texels). Choose the cell = ink box grown by 1 texel each side; check cell + 1-texel ring ⊂ disc.
  Write `notes.txt`: per drum and language, address, CLUT, coverage hash over `{0,0,128,128}`, ink box, disc box,
  chosen cell.
- [ ] **Step 4: HD pack.** `TOUR_TEXTURES=C:/Users/torso/Documents/deco/build/e2e_portable3/textures/INFN00001
  to_march.py <dir>/pack 1`, `hitshots.py <dir>/pack`; look up the drum hashes in the pack's `textures.ini`
  (grep the content hash the runtime logs with `PSPRECOMP_TEX_WATCH=<addr>` if needed). Record in `notes.txt`
  whether the pack replaces the drum textures and where its symbol sits (×4 image).
- [ ] **Step 5:** no commit (recon). Ledger the facts.

### Task 3: The four drum specs (DxD 1.1.4 es / en)

**Files:** Modify `games/patapon3/runtime/p3_prompts.cpp`; create `runtime/tests/test_p3_drum_prompts.cpp`
(registered in the `patapon3` block of `runtime/CMakeLists.txt` like `test_p3_command_labels`, linking
`p3_prompts.cpp`, `src/psp_prompt_atlas.cpp`, `src/psp_prompt_raster.cpp`, `src/psp_prompts.cpp`,
`src/psp_controls.cpp`, `src/psp_input_device.cpp`, `src/psp_texrep_png.cpp`, `src/psp_bar_text.cpp`, stb, the
embedded icon and font data, `p3_prompt_labels.cpp`, `p3_command_labels.cpp`; it defines its own
`psp_prompts_register_atlas` that collects the specs, as `preview.cpp` does;
`PSP_P3_DRUMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../build/prompts_ref/p3drums"` like `test_p3_achievements`'s
save dir).

**Interfaces — Consumes:** Task 1 (`cell_bg`, 8 atlases), Task 2 (`notes.txt`: cells, hashes).
**Produces:** specs `drum-pata` {Square}, `drum-pon` {Circle}, `drum-chaka` {Triangle}, `drum-don` {Cross}:
128×128, `GE_TFMT_CLUT4`, `sig_rects {{0,0,128,128}}`, `sig_hashes` from Task 2 (one per distinct coverage,
commented with languages), one cell each from Task 2, `gray 0xFE`, `hole 0x00`, `cell_bg 0xFF000000`, no labels,
no separators; registered after the command list in `p3_prompts_register()`; header comment describes the
drum textures (VRAM, addresses, recon date).

- [ ] **Step 1: Failing test** `test_p3_drum_prompts.cpp`: collects the registered specs (expects names
  `help-bar, command-list, drum-pata, drum-pon, drum-chaka, drum-don` in that order); for each drum dump PNG in
  `PSP_P3_DRUMS_DIR` (`pata_*.png`, `pon_*.png`, `chaka_*.png`, `don_*.png`, and the EU-English ones if their
  coverage differed): `prompt_atlases_recognise` → index of its drum spec × 32 + 1; `other_*.png` → 0; for k = 1, 4
  compose Keyboard/Gray and Xbox/Color on the ×k resample: every texel of cell + ring has alpha 255, the black of
  the disc is unchanged outside cell + ring, and pixels outside the disc are byte-identical to the input.
- [ ] **Step 2:** `ninja test_p3_drum_prompts && ./test_p3_drum_prompts` → FAIL (no drum specs registered).
- [ ] **Step 3: Implement** the four specs in `p3_prompts.cpp`.
- [ ] **Step 4:** test PASS; `ninja` (1.1.4); `ctest` all green. Offline look: `preview.exe` / `show.py` on each drum
  dump at ×4 for `PREVIEW_DEVICE=0..3` — the button sits in the disc, no box, no hole, word untouched.
- [ ] **Step 5: Commit** `feat(p3): drum-hit words show the mapped button`.

### Task 4: Live verification, gates, docs, deploy, hand-off

**Files:** `ARCHITECTURE.md` (up to 8 prompt atlases; the cell background; the drum words), `DEBUGGING.md`
(`[PROMPTS] drum-… atlas recognised`, the drum recon tools), `README.md` (PC-prompts line: help bars, command
list and drum words).

- [ ] **Step 1: Docs**; commit `docs: PC button prompts on the drum-hit words`.
- [ ] **Step 2: Live**, 9998, scale 2: `to_march.py` + `hitshots.py <dir> <device>` for keyboard, xbox, ps4, ps5
  in es (`LANG_DOWNS=5`) and en (`LANG_DOWNS=2`); Spanish also with `PSPRECOMP_BUTTON_ICONS=color` and `=off`,
  and the HD pack (scale 1). Crop each word (×4) into `build/prompts_ref/p3drums_after_<lang>.png` and **read every
  symbol** against the mapping (keyboard default: □ A, ○ Z, △ S, ✕ X; Xbox □ X, ○ B, △ Y, ✕ A); the log has
  `[PROMPTS] drum-pata atlas recognised` (and the three others). Also run 1.1.5b es and vanilla es once (build the
  three variants first: `DECO/build/rtw`, `v115b/rtw`, `vanilla/rtw`); a release whose drum coverage differs gets
  its hash (back to Task 3 Step 3, test first) or is listed as not covered.
- [ ] **Step 3: Gates.** `ctest` green in the three build dirs. Both-off tour vs `build/tour/prompts_off` →
  `RT_REGRESS PASS`. Default tour vs `build/tour/p2_on`: hideout checkpoints within the known noise (14/15 and
  occasionally 04 animate); `perf_hideout.py` off vs on: GE ms and binds as before (4.0 ms, 27 binds).
  (`DECO/build/tools/texture_recon/gates_p2.sh` is the model; outputs `p3d_*`.)
- [ ] **Step 4: Deploy** like part 2: back up the portables' exes to `build/backup_pre_drums_<date>/`, copy the
  three new exes to `build/e2e_portable3` and `build/e2e_portable` (`game/<release>/psprecomp_runtime.exe`), only
  when no `psprecomp_runtime.exe` from the portables is running.
- [ ] **Step 5: Hand-off.** Self-review of the branch → ledger. `build/autopilot/EN_QUE_NOS_QUEDAMOS_<date>.md`
  (Spanish: what changed, how to try it — hit drums in any mission —, decisions incl. rulings, contact sheets),
  memory `p3-pc-button-prompts.md`, report to the user in Spanish. **Do not push** until the user says so; then
  `push-p3-26` from `p3/main`, cherry-pick this plan's commits (spec/plan included), build + ctest there,
  `git push p3 push-p3-26:main`.
