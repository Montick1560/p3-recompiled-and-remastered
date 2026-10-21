# PC button prompts — the drum-hit words — Design

Date: 2026-10-20. Asked by the user after part 2 ("mejor los tambores en el juego … PATA, PON, DON, CHAKA
aparece con el botón asignado, ahorita siempre aparecerán con el botón de play … ¿podrás ponerle el botón
asignado dinámicamente? (al menos para es/en), crea el plan"). It comes before the old "part 3" idea (icons
inside the hint panel / dialogue text), which stays for later. Parts 1, 1.1 and 2 (help bars, command list) are
pushed (`p3/main` 5ce7127); their specs sit next to this one.

## 1. Intent

When a drum is hit in a mission, its word flies out next to the hero — PATA, PON, CHAKA, DON — with a small
white PSP symbol (□ ○ △ ✕) in a black disc under the word. That symbol should be the key or pad button the
player actually uses for that drum, switching with the last input device and following remaps, exactly as the
help bars and the command list now do. The drum words are the most frequent prompt in the game: every beat.

## 2. What the game does (recon 2026-10-20, DxD 1.1.4 Spanish, training mission of the tour save)

Tools: `DECO/build/tools/texture_recon/to_march.py`, `hitlog.py`, `hit_textures.py`, `hitshots.py`, `near.py`
(kit README). Dumps: `DECO/build/prompts_ref/p3drums/`.

- Each drum word is **its own texture**: 128×128 CLUT4, swizzled, in **VRAM** (the GE's texture-buffer-width
  high byte is 0x44 → address 0x04xxxxxx): PATA at `0x041302C0` (CLUT `0x04132300`), PON at `0x041324C0`
  (CLUT `0x04134500`). The texture holds the word art (drum colour + black outline), three faint rings and the
  symbol: white (`0xFEFEFE`/`0xF5F5F5`) ink at about x 57..68, y 88..99, in an opaque black disc. 16 colours,
  each drum its own palette. CHAKA and DON were seen on screen (green / yellow, same layout) but not dumped yet.
- The word is drawn only for a moment, 0.2–0.4 s after the press; the textures are used only in those frames.
- The runtime's texture-patch seam (`psp_ge_texpatch.h`) already sees VRAM textures (same bind path as the HD
  pack), so the technique of parts 1/2 applies: recognise each drum texture by its coverage, redraw the symbol
  cell with the icon.

## 3. Decisions (Claude's, flagged to the user)

1. **Same technique, same switches.** One prompt-atlas spec per drum texture (names `drum-pata`, `drum-pon`,
   `drum-chaka`, `drum-don`), registered after the help bar and the command list. `PSPRECOMP_BUTTON_ICONS`
   (gray / color / off) covers them; there is no text to redraw (the words are art, not text). No new launcher
   option.
2. **The patcher holds up to 8 atlases** (was 4): 2 + 4 drums = 6. The packed tag stays index × 32 + local and
   still fits the seam's 8 bits (7 × 32 + 31 = 255). Help bar and command list keep indices 0 and 1, so their
   tags and cache keys do not move.
3. **The cleared cell keeps the disc.** Today a redrawn cell is cleared to transparent; inside the drum's black
   disc that would punch a hole. A spec gains a cell background colour (opaque black for the drums; default
   unchanged = transparent, so the help bar and the command list stay byte-identical).
4. **Icon colours:** the game's symbol is white, so the drums tint neutral icon pixels white (`gray = 0xFE`) and
   fill the icon's cut-out with the disc black (`hole = 0x00`). Color style keeps Kenney's colours, as before.
5. **Languages / releases:** at least DxD 1.1.4 es and en (the user's minimum). The drum words are onomatopoeia
   and probably the same texture in every language and release; every release/language whose dump has the same
   coverage gets it for free, others get an extra signature hash when cheap (one boot each), else are listed as
   not covered.
6. **HD pack:** if the pack replaces these textures, the icon is composed on the pack image at
   `img.w / spec.width` like parts 1/2; if the pack image's disc sits elsewhere, that is a finding to report,
   not to paper over.

## 4. Goals / anti-goals

Goals: every drum word shows the mapped button of its drum for the last-used device, gray or colour, with and
without the HD pack, es and en at least; the symbol switches as soon as the device does.
Anti-goals: any change to the help bars or the command list (byte-identical); a hole or a square box around the
new icon; an icon outside the black disc; any steady-state frame-time cost; the debug-menu patch set or the icon
commit in a push.

## 5. Verification

- Unit: 8-atlas packing (index 7 → tag 224 + local, no overflow), the cell background fill (no alpha-0 texel left
  in a filled cell; default spec unchanged), the drum specs (cells inside the 128×128 texture, inside the disc
  measured on the dumps); all earlier prompt tests unchanged and green.
- Offline: the preview tool composes each dumped drum texture for keyboard / Xbox / PS4 / PS5 at ×4.
- Live on 9998 (`PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00`, scale 2): burst shots of the four words per device,
  gray and colour, icons off, HD pack (scale 1), es and en; `[PROMPTS] drum-… atlas recognised` in the log.
- Gates: `ctest`; both-off tour = `prompts_off` (PASS); default tour vs `p2_on` (hideout checkpoints unchanged);
  `perf_hideout.py` unchanged.
- Push only after the user's play check (next push branch `push-p3-26`).
