# Five launcher themes from the user's backgrounds — design spec (25 Oct 2026)

Source images: `C:/Users/torso/Documents/deco/fondos/` (1.jpg 3840x2160, 2.jpg 3840x2160, 3.jpg 4096x2160,
4.png 3840x2160 palette, 5.png 1920x1080 palette). Product truth: `launcher/PRODUCT.md`.

## User decisions (AskUserQuestion, 25 Oct)

- The 5 images ship to beta testers (the user's permission). No author credit requested: author "Patapon Community".
- Composition: impeccable concept-seed dice per theme, with a veto on any roll that covers the characters or
  fights the image; the user picked one per theme (below).
- All five themes are free (no `unlock`).
- Impeccable stays at v4.0.2 (no update).
- Surface: the launcher start screen (`MainMenu`). Visitor mode: Experience, bounded by Operate's rule (the
  start entry and the prompt bar stay obvious). Other screens keep the shared layout and only take the colours,
  font and a legibility veil.

## Composition analysis (6x4 grid, edge density 0-99 / mean luminance 0-255; "calm" = density < 10)

| # | Subject | Calm zones | Busy zones | Palette (share) |
|---|---|---|---|---|
| 1 | Ring-crowned forest, two Patapons (one with a guitar) on a dark teal hill, left-centre | bottom row cols 1-3 (hill, lum ~50); right third mid moderate (21-29) | canopy rows 0-1 everywhere; characters cols 1-3 rows 1-2 | #00647b 29, #caeaf2 21, #07484e 19, #42bfdb 19, #090303 8 |
| 2 | Sea of clouds, lavender-to-peach sky, no characters | everything (3-10); sky band top 30 % lum ~175 | none | #acaec0 20, #bebbc3 15, #f4f5f9 14, #fbfcfd 12, #cad0e0 12 |
| 3 | Giant tilted "P3" logo (blue P, red 3, black outline) centre, blue sky, dandelion puffs right / bottom-left | cols 0-1 rows 1-2 (2-7), cols 4-5 rows 1-2 (7-11) | logo cols 2-3 rows 1-2 (20-32); puff bottom-left | #71b0ef 31, #62b5ed 18, #a8dbf6 9, #6a8abd 5; logo red #d01838 |
| 4 | Pink world, diagonal hill bottom-left to top-right, eye creature top-left, dino creature top-right, sunset strip | centre and right rows 1-3 (0-5) | top-left (24), top-right (16-20) | #f2849d 44, #f7b8b9 29, #c56d7b 14, #e8a3ae 6; flowers #f04878 |
| 5 | Purple night, Uberhero (orange cape) centre-left, giant black boss with green spots top-right, stone pillars, mist | left cols 0-1 rows 1-3 (0-8, lum 16-61); right col 5 rows 1-3 (boss body, lum ~1-22) | boss cols 3-4 row 0 (32); hero col 2 row 2 (44) | #987993 26, #160a1d 21, #000001 19, #463250 15; cape #f08030, spots #b8d878 |

## Grounded structural candidates (start-screen topologies)

A drum command row on the ground · B ground-line marquee · C hanging war banner · D totem column · E eye cursor ·
F slope procession · G speech bubble. Ordered per image by resonance, then rolled (`concept-seed.mjs --scope
surface --mode experience --candidate-count 7`, keys b2e46a24, 69df3f24, 8aa8b5f2, 332a932a, dfbe0c1e).

## Chosen directions (direction contracts)

### 1 `bosque` "Bosque de anillos" — Bajada multiplano (fused challenger: multiplane cel dawn; seed b2e46a24)
- THESIS: the camera arrives in the forest the way a cel-animation opening does; the menu grows like one more ring tree. Refuses the centred overlay.
- OWN-WORLD: flat teal planes, white rings, black Patapon silhouettes; Lilita One; menu on a tree-trunk band (#00647b, 88 % opacity) crowned by a ring disc holding the logo; text #e6f7fb, accent #ffe066; the focused entry carries a white ring marker (0.75em circle, 0.18em border).
- STORY: the player sees the band of Patapons, then the menu settles on the right; start is focused.
- FIRST VIEWPORT: backdrop at `center 85%`; trunk band right edge at 6vw, width 30vw, full height; ring disc (30vw) at top holding the logo; entries right-aligned from 34vh, 4.2vh type; prompt bar unchanged.
- FORM: column (`menu_axis` column). Motion: on mount the backdrop goes from `scale(1.14) translateY(-6%)` to identity in 1500 ms `cubic-bezier(.2,.7,.2,1)` (transform only); the band slides in from +4vw with 250 ms delay 1200 ms. `prefers-reduced-motion`: none of it.

### 2 `nubes` "Mar de nubes" — Ojo del cielo (rolled, seed 69df3f24)
- THESIS: a Patapon eye rises over the clouds like the sun and looks at what you choose. Refuses a dark glassy panel.
- OWN-WORLD: light theme. Fredoka (variable, 600); ink #24284a, accent #d9573b, panel #f6f7fb, muted #6c7296, bg #b9bfd6; the logo inked (`filter: brightness(0)`, opacity .85).
- STORY: the eye follows the focus down the list; start first.
- FIRST VIEWPORT: backdrop `center bottom`; eye white (`.main-menu::before`) 24vh disc, 1.1vh ink outline, at left 22vw top 6vh; pupil (`::after`) 9vh ink disc moving toward the entry: x offset up to +5vh, y offset `calc(var(--sel) / max(var(--count) - 1, 1) * 9vh)`; logo right of the eye in the sky band; menu column left-aligned at x 52vw from 36vh; focused entry: accent + an ink dot marker.
- FORM: column. Motion: pupil transition 220 ms ease-out; blink (scaleY to .08) 160 ms every 6 s; reduced motion: no blink, pupil jumps.
- Every other screen: light veil `rgba(246,247,251,.78)` instead of the dark one, prompt bar `rgba(246,247,251,.88)` with ink text.

### 3 `cielo_p3` "Cielo P3" — Marquesina P3 (rolled, seed 8aa8b5f2)
- THESIS: the image's own P3 logo is the title; the menu is the marquee under it. Refuses a second logo.
- OWN-WORLD: Bungee; white fill with a 0.12em black outline (`-webkit-text-stroke`, `paint-order: stroke fill`) and a hard 0.08em shadow #0b1a3a, like the P and the 3; accent #e3203f; panel #eef7ff, panelText #10203a, muted #d6ebff, bg #5aa8e8.
- STORY: logo, then one line of choices; ← → walk it, ↑ ↓ change the version.
- FIRST VIEWPORT: launcher logo hidden; backdrop `center`; version picker centred at 70vh with ▴ ▾; entries a centred wrapping row (max-width 86vw, gap 1.2em 2.2em) from 76vh, 3.4vh type; focused entry red, scale 1.12, rotate -4deg (the P's tilt).
- FORM: row (`menu_axis: "row"`). Motion: the focused entry bounces 0.12em on a 500 ms beat (120 BPM); reduced motion: still.

### 4 `rosa` "Tierra rosa" — La criatura habla (rolled, seed 332a932a)
- THESIS: the dino creature speaks the menu. Refuses a panel floating nowhere.
- OWN-WORLD: Chewy; plum ink #3d1830 on a white speech bubble (92 % opacity, 0.35em plum outline, organic radius 46% / 38%); accent #e0245e; panel #fff0f4, panelText #3d1830, muted #a35a76, bg #f2849d. Focused entry: accent + hand-drawn wavy underline (SVG data URI in accent).
- STORY: the creature at the top right talks; its words are the choices.
- FIRST VIEWPORT: backdrop `center 35%`; bubble right 9vw, top 24vh, width ~34vw, padding 3vh 4vw; tail (`.menu::before`) pointing up-right at the creature's mouth (~x 76vw, y 18vh); version picker as the bubble's first line; logo bottom-left over the pink field, inked plum.
- FORM: column, one entry per line inside the bubble. Motion: bubble pops 0.9→1 in 180 ms from the tail; underline draws in 160 ms; reduced motion: none.

### 5 `uberhero` "Grito de guerra" — Grito de guerra (rolled, seed dfbe0c1e)
- THESIS: Uberhero shouts the menu at the boss. Refuses a calm list.
- OWN-WORLD: Bangers, uppercase, letter-spacing .04em; a white comic burst (clip-path spikes, ≥0.25em black outline) with ink #1a0d22; focused entry on a cape-orange #ff8a2a block skewed -6deg, black text; colours for other screens: bg #160a1d, text #f3e9ff, accent #ff8a2a, panel #2a1a35, panelText #f3e9ff, muted #b49ac4.
- STORY: the hero yells, the player answers.
- FIRST VIEWPORT: backdrop `60% center`; burst left 5vw, top 18vh, ~30vw wide; tail pointing right at the hero's head (~x 41vw, y 43vh); logo top-left above the burst, max-height 14vh; version picker as the burst's first line.
- FORM: column. Motion: the burst shakes (-2deg → 2deg → 0) in 240 ms on mount; the focus block snaps (80 ms, steps); reduced motion: none.

## Shared rules

- Fonts: OFL / Apache 2.0 faces from github.com/google/fonts, saved as `font.ttf` next to their licence file.
- No music or sounds come with these images: the themes ship without them.
- The focused entry never differs by colour alone (ring, dot, tilt, underline, block).
- Every label meets 4.5:1 against what is directly behind it, at 1920x1080, 1280x720 and 1280x800.
- Each theme folder stays under 2.5 MB.
