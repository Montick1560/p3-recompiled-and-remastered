# PC button prompts, part 1 — the help bars — Design

Date: 2026-10-19. Agreed in chat with the user (brainstorming with mockups in the browser companion); the user
accepted that icons fit the original cells ("vale").

Claude's calls (not asked, flagged to the user): `Gris` is the default style; pads SDL does not identify use the
Xbox set; long key names become symbols; the atlas is recognised by content; composed textures are cached; the
code split core / game module; Kenney credited although CC0 does not require it.

## 1. Intent

The game draws PSP buttons (✕ ○ □ △, L/R, START, SELECT, the D-pad) in its help bars. The user wants the port to
*feel like a PC game*: the help bars show the key or controller button the player actually uses. Not a bug fix
("no es molesto"); a polish feature for everyone who downloads the port (keyboard, Xbox pad, Steam Deck,
DualShock 4, DualSense).

Part 1 = **every help bar** (the strip at the bottom of the hideout, the barracks, the forge, pause menus…). The
user started with "the hideout"; every help bar crops its icons from the same texture (§3), so doing all of them
is the same work (agreed). Part 2 (mission drums / command list, other textures) and part 3 (stray icons: title
screen, tutorials, icons inside dialogue text) are separate projects, not designed here.

## 2. Goals, non-goals, anti-goals

Goals
- The help bar icons show the player's input device, switching at once to the device of the **last input**:
  keyboard; Xbox-style pad (Xbox 360/One/Series, **Steam Deck**, any pad SDL does not identify); **DualShock 4**;
  **DualSense**.
- The icon is the button the player has **mapped** to that PSP action in the launcher (`settings.ini
  [controls]`, `PSPRECOMP_CONTROLS`), for the keyboard and for the pad — never the factory default if the user
  changed it. A pad remap shows the physical button in that pad's style (✕ mapped to pad `b` → **B** on Xbox,
  **○** on a DualSense).
- A launcher option **Iconos de botones** with the styles **Gris** (the game's gray, like the PSP icons today)
  and **Color** (Xbox A green, B red, X blue, Y yellow; PlayStation ✕ ○ □ △ in Kenney's colours).
- Every game language and every release the port builds (DxD 1.1.4, DxD 1.1.5b, vanilla EU).
- Works with and without the HD texture pack.

Non-goals (this part)
- Mission drum icons, the command list, any other texture than the help-bar atlas (part 2).
- Changing the bar's texts ("Confirmar", "Seleccionar"…) or its layout; changing what buttons do.
- Nintendo Switch / other glyph families.
- Live re-mapping while the game runs (controls are read at start-up, as today).

Anti-goals (a failure even if it "works")
- An icon that is not the button that performs the action (wrong mapping, stale device).
- Any measurable frame-time cost, flicker or a frame with a half-updated bar when the device switches.
- Breaking or degrading the HD texture pack; garbage or misaligned icons in some language.

## 3. What the game does (recon 2026-10-19, DxD 1.1.4, Spanish, hideout)

- The whole help bar is the last 11 PRIMs of the frame (through-mode sprites, `vtype 0x800183`): texture
  `0x092E5300`, **256×128 CLUT4, swizzled, bufw 256**, CLUT `0x092E9340` (16 × 8888, gray ramp + red/black).
  Skipping those PRIMs (`X 2038 2048` on the debug socket) removes the entire bar and nothing else.
- That texture is a **pre-rendered atlas**: every action label of the game's help bars in the current language
  ("Cancelar", "Confirmar", "Pantalla de título"…), the **START** and **SELECT** pills, and one row of glyphs at
  the bottom: `: / & □ ○ ✕ △`, D-pad variants (red highlight on a direction), `L/R`, `✚`. The game builds each
  bar by cropping cells of this atlas, so replacing the glyph cells changes every bar. Dump:
  `build/tools/texdump.py` (port arg) on the addresses above, swizzle 1.
- Cell sizes in the Spanish atlas (texels): the glyph row is y 117..127 — `:` x1, `/` 4..7, `&` 10..16,
  □ 19..27, ○ 30..38, ✕ 41..49, △ 52..60, three D-pad variants 63..71 / 74..82 / 85..93, `L/R` 96..111,
  ✚ 115..123 (so a face button cell is **9×11**, `L/R` **16×11**); START pill x 1..37 and SELECT pill x 41..79,
  y 100..113 (**~38×14**). The game draws each cell at that size on screen (×4 at 1080p): **a new icon must fit
  the old cell** — the bar's layout cannot grow (the approved mockups drew "LB/RB" a little wider than `L/R`;
  in the build it is squeezed into the 16-texel cell).
- The atlas is language-specific (labels are translated). Assumption to verify: the glyph row and the pills sit
  at the same cells in every language. The HD pack ships per-language UI textures (`ui/language/*`), so it very
  likely replaces this atlas too.

## 4. Decisions

1. **Technique: change the glyph cells inside the atlas**, not an overlay on top of the frame. When the game
   binds the help-bar atlas, the runtime substitutes a version whose glyph cells (and START/SELECT pills) are
   redrawn for the current device and style; labels and every other pixel stay the game's (or the HD pack's).
   The game keeps positioning the icons. Composed images are cached per (atlas, device, style, mapping) — no
   per-frame work; a device switch only selects another cached texture.
2. **Recognise the atlas by content**, not by address: the release/language/HD pack may move it or redraw it.
   The builder chooses how (e.g. a hash of the glyph row region of the original texture, as the texture
   replacement already hashes textures).
3. **Icons: Kenney "Input Prompts" 1.5** (CC0, kenney.nl; attribution not required, credited anyway in the
   launcher credits and the repo). Families: Keyboard & Mouse, Xbox Series, PlayStation Series (DualShock 4 =
   `playstation4_*`, DualSense = `playstation5_*`, shared `playstation_*`). Steam Deck uses the Xbox set (same
   A/B/X/Y layout; the user plays on one). Only the icons used are vendored, with Kenney's `License.txt`.
4. **Icons are enlarged to fill the bar's height** (user: at Kenney's proportions L1/R1 and the DualSense OPTIONS
   are too small in a ~10 px bar). Mockups the user approved: `bar_xbox_gris/color`, `bar_teclado`,
   `bar_ps4_*`, `bar_ps5k_*` in `DECO/.superpowers/brainstorm/1917-1792470707/content/` (hand-drawn stand-ins
   except the PS5 ones).
5. **Per device** (approved):
   | PSP | Keyboard | Xbox / Steam Deck | DualShock 4 | DualSense |
   |---|---|---|---|---|
   | ✕ ○ □ △ | the mapped key | A B X Y (as mapped) | ✕ ○ □ △ (as mapped) | ✕ ○ □ △ (as mapped) |
   | L / R | mapped keys | LB / RB | L1 / R1 | L1 / R1 |
   | START | mapped key (wide pill fits "Espacio") | Menu ☰ | OPTIONS (labelled) | OPTIONS (slanted, ≡) |
   | SELECT | mapped key | View ⧉ | SHARE | CREATE |
   | D-pad | the mapped arrow keys | D-pad | D-pad | D-pad |
   An action with several keys (✕ = `X, Return` by default) shows the **first** one (user agreed).
6. **Device detection**: last input wins (a key press → keyboard; a pad button or a stick/trigger past its dead
   zone → that pad). The pad family comes from SDL's controller type (PS4 / PS5 / everything else = Xbox style).
7. **Fit**: every icon is drawn inside its original cell (§3). Keyboard keys whose name does not fit a 9-texel
   cell use a symbol: Enter ↵, Space ␣, Tab ⇥, Backspace ⌫, arrows ← ↑ → ↓; the START/SELECT pills fit a short
   word ("Espacio", "Tab").
8. **Code placement** (`CLAUDE.md` rule: no game data in the runtime core): the generic parts — last-input device
   tracking, the controller family, the icon set, composing a texture with replaced rectangles and substituting
   it at bind time — live in `runtime/`; what is Patapon 3's — recognising the help-bar atlas and its cell table
   per language, which cell is which PSP button — lives in `games/patapon3/runtime/`.
9. **Launcher option** in `games/patapon3/game_options.json` (tab general): **Iconos de botones** = `Gris`
   (default) / `Color`, through a new env variable read by the runtime.

## 5. Left to the builder

- Exact glyph cell rectangles (from the game's UVs) and how they are found per language; how the replacement
  hooks into the existing texture path (`psp_texrep` / `ge_texture_bind`) and its cache; composition resolution
  (match the HD atlas when the pack is on).
- Gray tint value (match the atlas text gray) and how keyboard keys without a Kenney icon are drawn (a keycap
  with the key's short name).
- Where the vendored icons live and how they reach the exe folder (like `achievements.json`).
- A test/forcing switch for the device (for tours and unit tests), the initial device at start-up.

## 6. Verification

- Unit tests: mapping → icon choice per device (including remaps and the "first key" rule), atlas recognition.
- Live on 9998: screenshots of the hideout bar for keyboard / Xbox / DualShock 4 / DualSense, Gris and Color,
  with and without the HD pack, in at least two game languages; a forced device switch shows no torn frame.
- `ctest`, `rt_regress.py`, `perf_hideout.py` (no frame-time change).
- The user plays and approves before any push.
