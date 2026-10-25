# Five start-screen themes from the user's backgrounds — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (native, the user's usual choice for this project) or superpowers:subagent-driven-development to implement this plan task-by-task. Also load the `impeccable` skill (run its `context.mjs --target src/screens/MainMenu.tsx` once; PRODUCT.md now exists) and its `reference/craft-floor.md` before the first UI edit. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Five free launcher themes (`bosque`, `nubes`, `cielo_p3`, `rosa`, `uberhero`) built from `C:/Users/torso/Documents/deco/fondos/`, each recomposing the start screen around its image as decided in the spec, shipped in the beta package.

**Architecture:** Themes stay data: `themes/<id>/` with theme.json, theme.css, a background, a font and a preview. Three small launcher changes make the compositions possible: theme.json may name its background file (`background`, any image type), may ask for a horizontal menu (`menu_axis: "row"`), and the start screen exposes its focus to CSS (`--sel`, `--count`, `data-selected`, `data-axis`). Everything else is theme.css.

**Tech Stack:** Tauri 2 (Rust `src-tauri/`, `cargo test`), React + Vitest (`src/`, `npm test`), Python + Pillow for asset conversion and screenshots (`build/tools/`), FFmpeg not needed.

**Spec:** `psprecomp/docs/superpowers/specs/2026-10-25-launcher-fondos-themes.md` (composition analysis, the user's choices, one direction contract per theme with exact colours, fonts, positions and motion). Product truth: `launcher/PRODUCT.md`.

## Global Constraints

- Never push, never `gh`. Local commits only: launcher repo `C:/Users/torso/Documents/deco/launcher` branch `beta-1`; psprecomp branch `build-2026-10-25` (docs only). Author `-c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com`, no `Co-Authored-By`; each message ends with a `Verified:` line.
- Never write the user's `build/mi_build/.../settings.ini` or savedata from a test: test in the copy `build/plata/lpad/Patapon3Recomp` (refresh its `launcher.exe` and `themes/` from the build; set `[launcher] theme = <id>` there). Game test instances only on ports 9996–9998 (`PSPRECOMP_DEBUG_PORT=9998`).
- Launcher gates: `cd src-tauri && cargo test` and `npm test` green. The package is built last with `P3_NO_BETA_GATE=1 npm run tauri build -- --no-bundle`.
- Theme ids exactly `bosque`, `nubes`, `cielo_p3`, `rosa`, `uberhero`; names "Bosque de anillos", "Mar de nubes", "Cielo P3", "Tierra rosa", "Grito de guerra"; author "Patapon Community"; no `unlock`, no music, no sounds.
- Fonts from `https://github.com/google/fonts` (`ofl/lilitaone/LilitaOne-Regular.ttf`, `ofl/fredoka/Fredoka[wdth,wght].ttf`, `ofl/bungee/Bungee-Regular.ttf`, `apache/chewy/Chewy-Regular.ttf`, `ofl/bangers/Bangers-Regular.ttf`), saved as `font.ttf` with the folder's `OFL.txt` / `LICENSE.txt` beside it. Verify each URL resolves before relying on it.
- Each theme folder < 2.5 MB. Backgrounds: `background.webp` (Pillow, quality 85, original size) except `rosa` and `uberhero`, whose palette PNGs stay `background.png` when smaller.
- Focused entry never marked by colour alone. Labels ≥ 4.5:1 against what is behind them at 1920x1080, 1280x720 and 1280x800. Every animation off under `prefers-reduced-motion: reduce`; animate `transform`/`opacity` only.
- UI copy es first, en second; code, comments and commits in English.

## Review Focus

1. Steam Deck 1280x800 (16:10) and the 1280x720 window crop the cover background differently: bubble tails, the eye and the trunk band must still meet their targets (Task 3 tour runs all three sizes; each theme task checks them).
2. `nubes` is light: every other screen (Opciones, Partidas, Logros, Temas, Reportar problema, install, notices) must stay legible under its veil (Task 5 tour of every screen).
3. English labels are longer ("Report a problem", "Achievements"): row and bubble layouts must not overflow or clip (each theme task runs the tour with `language = en`).
4. The menu has 4 to 7 entries (Partidas, Logros, Temas are conditional) and the version picker is absent with one release: `--count` changes the eye math and the row wrap (Task 2 test `css_hooks_follow_focus` with 4 entries; tour once with a single installed release).
5. A 4K background under a 1.5 s camera move on a weak GPU (Deck): transform-only animation, no layout thrash; reduced motion disables it (Task 4 checks frame pacing by eye in the capture and the reduced-motion path).

---

### Task 1: theme.json `background` and `menu_axis`; shipped themes stay valid

**Files:**
- Modify: `launcher/src-tauri/src/themes.rs` (`Raw`, `ThemeInfo`, `load`, `builtin_default`), `launcher/src/types.ts` (`ThemeInfo`), `launcher/src/test-fixtures.ts` (`theme()` default `menu_axis: "column"`), `launcher/README.md` (theme format lines, both languages; drop "ningún tema incluye arte del juego" / "no theme includes game art" and say the five new themes use backgrounds the project was given permission to ship)
- Test: `themes.rs` unit tests

**Interfaces:**
- Produces: `ThemeInfo.menu_axis: String` (`"column"` | `"row"`, serialised as `menu_axis`); TS `menu_axis: "column" | "row"`. `ThemeInfo.background` = `file(dir, id, raw.background)` when that key is given and the file is safe and present, else `fixed(dir, "background.png")`.

- [ ] **Step 1: Write the failing tests** in `themes.rs`:
  - `background_key_names_any_image`: theme.json `{"background": "bg.webp"}` with `bg.webp` present → `background` ends with `bg.webp`; `{"background": "missing.jpg"}` with a `background.png` present → ends with `background.png`; `{"background": "../x.png"}` → falls back the same way.
  - `menu_axis_values`: `"row"` → `"row"`; absent, `""`, `"diagonal"` → `"column"`; the built-in default → `"column"`.
  - `shipped_themes_are_valid`: for every id in `const SHIPPED: &[&str] = &["arena", "default", "noche"]`, `load(<CARGO_MANIFEST_DIR>/../themes/<id>, id)` is `Ok`, and every file key it names (`font`, `background`, `music`, sounds) resolved to an existing path. Later tasks append their id to `SHIPPED`.
- [ ] **Step 2:** `cargo test themes` → the two new behaviour tests FAIL (unknown field / missing field); `shipped_themes_are_valid` passes (existing themes).
- [ ] **Step 3:** Implement the two keys in `Raw`/`ThemeInfo`/`load`/`builtin_default`, the TS type and the fixture; update the README theme-format lines.
- [ ] **Step 4:** `cargo test` and `npm test` → green.
- [ ] **Step 5:** Commit (launcher) `feat(launcher): themes may name their background and ask for a row menu`.

### Task 2: start screen — row navigation and CSS focus hooks

**Files:**
- Modify: `launcher/src/nav/useNav.ts` (`useListNav`), `launcher/src/screens/MainMenu.tsx`, `launcher/src/components/PromptBar.tsx` (`GLYPHS`), `launcher/src/App.tsx:299` (pass `axis={shown?.menu_axis ?? "column"}`)
- Test: `launcher/src/screens/MainMenu.test.tsx`

**Interfaces:**
- Consumes: Task 1 `ThemeInfo.menu_axis`.
- Produces: `useListNav(count, onConfirm, onBack?, onSide?, axis: "column" | "row" = "column")` — in `"row"` left/right move the index (wrapping) and up/down call `onSide(-1 | 1)`. `MainMenu` prop `axis?: "column" | "row"`. The `.screen.main-menu` element carries `data-axis`, `data-selected="<entry id>"` and inline `style={{ "--sel": index, "--count": entries.length }}`. In row mode the version arrows read ▴ (−1) and ▾ (+1) and the prompt bar items are `select_row` (label `prompt.select`), `version` (label `menu.version`, only with a version picker) and `confirm`. New `GLYPHS` entries: keyboard `select_row: "←→"`, `version: "↑↓"`; xbox and playstation `select_row: "✚"`, `version: "✚"`.

- [ ] **Step 1: Write the failing tests** in `MainMenu.test.tsx`:
  - `row axis: right moves, down steps the version`: render with `axis="row"` and a version `onStep` spy; `{ArrowRight}` → the second entry `aria-selected`; `{ArrowLeft}{ArrowLeft}` → the last entry (wrap); `{ArrowDown}` → `onStep(1)`, `{ArrowUp}` → `onStep(-1)`; the version buttons show ▴ and ▾; the prompt bar shows `←→` and `↑↓`.
  - `css_hooks_follow_focus`: render with `onThemes` only (no saves, achievements, report → entries start, themes, options, exit) → root has `data-axis="column"`, `--count` = `4`, `--sel` = `0`, `data-selected="start"`; `{ArrowDown}` → `--sel` = `1`, `data-selected="themes"`.
  - `column axis keeps left/right for the version`: default axis, `{ArrowRight}` → `onStep(1)` and selection unchanged (pins today's behaviour).
- [ ] **Step 2:** `npx vitest run src/screens/MainMenu` → the row and hooks tests FAIL.
- [ ] **Step 3:** Implement the axis in `useListNav`, the hooks and row prompts in `MainMenu`, the glyphs, and pass `axis` from `App`.
- [ ] **Step 4:** `npm test` → green; `cargo test` unaffected.
- [ ] **Step 5:** Commit (launcher) `feat(launcher): the start screen can run as a row and exposes its focus to theme CSS`.

### Task 3: theme tour and contrast tools, DESIGN.md

**Files:**
- Create: `build/tools/theme_tour.py`, `build/tools/theme_contrast.py`, `launcher/DESIGN.md`

**Interfaces:**
- Produces:
  - `python build/tools/theme_tour.py <theme_id> [--size 1920x1080|1280x720|1280x800] [--lang es|en] [--screens main|all] [--out DIR]`: writes `[launcher] theme`, `[general] language` and `[display] window_mode` (`fullscreen` for 1920x1080, `windowed` otherwise) into the **test copy's** settings.ini, starts its `launcher.exe`, resizes the window for 1280x800 (`SetWindowPos`, client size), captures the start screen at 0 s, 0.4 s and 2.0 s (the motion), then with `--screens all` walks Opciones (every tab), Partidas, Logros, Temas, Reportar problema with real scan codes (as `build/plata/lnav/keys.py` does: no Alt-tap trick on the framed launcher), saves `DIR/<theme>_<size>_<lang>_<screen>.png`, closes the launcher it started (WM_CLOSE on its own PID only) and restores the copy's settings.ini.
  - `python build/tools/theme_contrast.py <image> <x0%,y0%,x1%,y1%> <text #hex>`: prints the WCAG contrast of the text colour against the 10th and 90th luminance percentiles of the region (the worst one first) and exits 1 when the worst is < 4.5.
  - `launcher/DESIGN.md` per impeccable `reference/document.md`: the incumbent system (tokens in `styles.css`, prompt bar, screens) plus the theme rules from the spec's "Shared rules" and the hook contract of Task 2 (`--sel`, `--count`, `data-selected`, `data-axis`, `menu_axis`, `background`).

- [ ] **Step 1:** Write both tools; run `theme_tour.py default --screens all` at the three sizes. Expected: 3 × 6+ PNGs that show the default theme's screens, the copy's settings.ini byte-identical afterwards (md5).
- [ ] **Step 2:** `theme_contrast.py C:/Users/torso/Documents/deco/fondos/2.jpg 50%,30%,80%,80% #ffffff` → prints a ratio < 4.5 and exits 1 (white on clouds); with `#24284a` → ≥ 4.5, exit 0.
- [ ] **Step 3:** Write DESIGN.md.
- [ ] **Step 4:** Commit (launcher) `docs(launcher): DESIGN.md (start screen and theme rules)`; the tools live in `build/tools/` (not in a repo).

### Tasks 4–8: one theme each

Each task builds one theme to its direction contract in the spec (the spec is the authority for colours, font, positions, motion and the focus cue) and follows the same steps. Theme folders live in `launcher/themes/<id>/`.

| Task | id | source | layout | contract |
|---|---|---|---|---|
| 4 | `bosque` | `fondos/1.jpg` | column | spec §1 Bajada multiplano |
| 5 | `nubes` | `fondos/2.jpg` | column, light theme | spec §2 Ojo del cielo |
| 6 | `cielo_p3` | `fondos/3.jpg` | `menu_axis: "row"` | spec §3 Marquesina P3 |
| 7 | `rosa` | `fondos/4.png` | column in a bubble | spec §4 La criatura habla |
| 8 | `uberhero` | `fondos/5.png` | column in a burst | spec §5 Grito de guerra |

**Files (each):** Create `launcher/themes/<id>/theme.json`, `theme.css`, the background, `font.ttf` + licence, `preview.png` (640x360, a downscaled 1920x1080 capture of its own start screen). Modify `themes.rs` `SHIPPED` (+ id).

**Interfaces:** Consumes Task 1 (`background`, `menu_axis`), Task 2 (`--sel`, `--count`, `data-selected`, `data-axis`, row prompts), Task 3 (`theme_tour.py`, `theme_contrast.py`).

- [ ] **Step 1: Failing test:** add the id to `SHIPPED`; `cargo test shipped_themes_are_valid` → FAIL (folder missing).
- [ ] **Step 2:** Make the assets: background per Global Constraints (`bosque`/`nubes`/`cielo_p3` → `background.webp` + `"background": "background.webp"`), the font and its licence, theme.json (name, author, the spec's six colours, `font: "font.ttf"`, `menu_axis` where the table says). `cargo test shipped_themes_are_valid` → PASS.
- [ ] **Step 3:** Write theme.css to the contract (state the contract's five blocks as its opening comment, ≤ 150 words). Scope main-menu rules under `.screen.main-menu`; global rules only for the veil, the prompt bar and the backdrop.
- [ ] **Step 4: Live check:** refresh the test copy (`launcher.exe` from the last `npm run tauri build`, the theme folder), then `theme_tour.py <id>` at 1920x1080, 1280x720 and 1280x800, `--lang es` and `--lang en`, plus `--screens all` once at 1920x1080. Read every capture. Expected: the composition of the contract (what is where), the characters / logo / creatures uncovered, no clipped or overflowing label, the focus cue visible, all screens legible; `theme_contrast.py` on the capture's menu region with the label colour → exit 0 at the three sizes.
- [ ] **Step 5:** `node C:/Users/torso/.claude/skills/impeccable/scripts/detect.mjs --json launcher/themes/<id>/theme.css` → fix real findings (a finding against a device the contract names stays, with a one-line reason in the commit).
- [ ] **Step 6:** Make `preview.png` from the 1920x1080 capture; `du -sh themes/<id>` < 2.5 MB; `cargo test` and `npm test` green.
- [ ] **Step 7:** Commit (launcher) `feat(launcher): <id> theme (<name>)` with `Verified:` (sizes, languages, contrast ratios, folder size).

Theme-specific checks on top of Step 4:
- `bosque`: captures at 0 s / 0.4 s / 2.0 s show the descent and the settled band. Reduced motion: read the `@media (prefers-reduced-motion: reduce)` block and confirm it removes every animation (do not change the user's Windows animation setting to test it).
- `nubes`: after `{ArrowDown}` × 3 the pupil sits lower than at start (two captures); every `--screens all` capture readable under the light veil, including the prompt bar.
- `cielo_p3`: ← → walk the row, ↑ ↓ change the version (watch `settings.ini [launcher] version` in the copy return to its first value after ↑ then ↓); the launcher logo is not drawn. Once, rename the copy's `versions/dxd-1.1.5b` and `versions/vanilla-eu` (copy only) so one release is installed: no version picker, no `↑↓` prompt, the row still centred; rename them back.
- `rosa`: the tail points at the dino's mouth at all three sizes.
- `uberhero`: the burst does not cover the hero's eye or the boss's spots at all three sizes.

### Task 9: final review, package, the user's build and checklist

**Files:**
- Modify: `build/mi_build/Patapon3Recomp/launcher.exe`, `build/mi_build/Patapon3Recomp/themes/` (add the five folders; settings.ini and saves untouched; old launcher.exe to `build/plata/mi_build_prev3/`)
- Create: `build/plata/RESUMEN_TEMAS.md` (Spanish)

- [ ] **Step 1:** Final review per impeccable new-work §7: the `impeccable-finish-reviewer` subagent if the harness has it (the user's skill invocation authorises it), given the spec, PRODUCT.md, DESIGN.md, the captures and the five theme.css; otherwise a fresh in-thread pass, disclosed. Apply material fixes with a capture before/after each.
- [ ] **Step 2:** `cargo test`, `npm test`, then `P3_NO_BETA_GATE=1 npm run tauri build -- --no-bundle` last; refresh mi_build as in Files; open its launcher once (no saving) and check the five cards in Temas.
- [ ] **Step 3:** Write `RESUMEN_TEMAS.md`: per theme one line on what "bien" looks like (the contract's first viewport in plain Spanish) and what to try (pad and keyboard, Ventana / Sin bordes / Pantalla completa, English); note that the themes have no music; note that the next beta update package (`scripts/make-portable.mjs` copies `themes/`) carries them, and that nothing was pushed.
- [ ] **Step 4:** Update memory (themes from fondos, local commits, not pushed).
