# Ping under the party masks (missions) + one ping meaning + first own asset — Design

Date: 2 Nov 2026. Builds on `ping-names` (u15: ping under hero badges / above name labels, real badge bars).

## Why

Players need to see who is lagging, during the whole mission. The u15 placement has two problems the user hit:

1. **In missions the number sits on the hero's name label**, which the game hides when the hero leaves the camera
   (and then every number vanishes, deferred minor of u15) and which crowds the field.
2. **The numbers disagree between screens.** In the hideout the user saw "Fuego 163 ms, me 63 ms"; Fuego saw the
   opposite. Not a slot swap: u15 shows our *own* hero our server RTT, but a *partner's* hero the RTT of the path
   between the two of us (direct route, or ours + theirs through the server). Each player therefore sees every
   partner as the slow one.

The user also wants to try **new art assets** in the port; this is the first one.

## What the user decided (2 Nov, chat + mockup page, option B)

- **Where (missions):** under each party mask at the top-left corner (the masks with the health bar that every online
  mission shows), *below the health bar*.
- **Look:** a small 3-bar signal icon followed by the ping as small text, no "ms" (mockup B). Bars lit = 3 / 2 / 1
  by quality, unlit bars grey; icon and number in the quality colour.
- **Asset:** Claude draws the icon ("esas barritas no se ven mal, dibújalo tú"), as a new image the runtime loads.
- **Recon mission:** "Shooshookle, la insaciable" (Finca de la Impetuosidad, Free): nothing defeats the heroes there.
  **Never open the iron door** in that mission.

## Calls Claude made (the user may overrule)

- **(User, 2 Nov, after the data below) A ping belongs to a connection, not to a player.** A partner's mask shows
  the real RTT of the route the game uses with that partner now: the direct path's measured p50, or through the
  server the measured relay p50 / our server RTT + theirs (u15's partner rule, kept). Both players of a pair see
  (almost) the same number, each on the other's mask. **Our own mask and our own hideout badge get no number** (and
  the badge keeps the game's look): our server RTT there made every partner look like the slow one.
  Data (beta.db copy, 30 Oct - 2 Nov, 453 player-to-player links): 336 direct (74 %, median 103 ms), 117 through
  the server (median ~200 ms); direct was faster in all 323 links that measured both (median gain 115 ms). Showing
  server RTTs would hide reality for most games.
- **The icon tells the route:** a second set of frames with a small server mark for links going through the server
  (shows when the direct connection failed). With 3-4 players each screen shows its own link to each partner; a
  player with a bad line is red on everyone's screen.
- **Missions: the number leaves the name label** (masks replace it; no duplicate). Hideout badges keep their number
  and real bars, now with the new meaning.
- **Unknown ping** (first seconds, a u14 partner through the server, whose `rtt` the server lacks): the icon in
  grey with all bars unlit, no number.
- **Thresholds and colours unchanged:** < 80 green (80,220,90), < 150 yellow (255,210,40), else red (240,70,60).
- **Size:** text about 7.5 PSP px tall (smaller than u15's 12.5), icon about 7×6 PSP px, both scaled with the render
  target and pixel-snapped.
- **Draw order:** right after the mask's own PRIMs (not at list end), so pause menus and game windows drawn later
  cover it like the rest of the HUD.
- **The asset lives in a file**, `assets/ping_signal.png` next to the exe (repo: `games/patapon3/assets/`), 8 frames
  side by side: 0-3 lit bars direct, then 0-3 lit bars with the server mark; white lit bars, dark grey unlit, black
  outline, tinted by multiply. Missing or
  unreadable file: text only, logged once. Replacing the PNG reskins it — that is the asset test. Shipping: the
  build copies it next to the exe, the beta package script copies `assets/`.
- **Option unchanged:** `game.show_ping` / `PSPRECOMP_P3_SHOW_PING=0` turns all of it off.

## Unknowns the recon must answer (first task of the plan)

- The mask HUD's GE signature (PRIM type, vertex type, texture, CLUT, count) and the health bar's, with their boxes.
- The u15 hideout report (user saw ElFuegoxd 163 / self 63, ElFuegoxd saw the reverse; their 2 Nov games went
  through the server, ElFuegoxd's server RTT 131-174 ms): with relay forced, do both screens put each number on the
  right hero? A swap on the visitor's screen would be a u15 bug to fix here.
- Which mask belongs to which player: draw order vs party slot, and whether our own mask is first, last or in slot
  order — with 2 players and, if the third instance joins, 3. Same check on DxD 1.1.4 and 1.1.5b (vanilla if the
  lab save reaches the mission).
- Whether the masks are drawn in every mission phase (walking, boss, pause, results) — "always visible" is checked,
  not assumed.

## Non-goals

- No change to the server (it already sends what is needed).
- No per-player "line quality" number (server RTT): not what the game feels in direct games.
- No briefing (CUARTEL) number, no chat, no new launcher UI.
- No general asset/mod loader: one PNG, one consumer. A loader comes when a second asset needs it.

## Anti-goals

- Numbers on the wrong mask (worse than none): if the masks cannot be matched to slots, draw nothing.
- Any cost offline / single player beyond the existing signature compare.
- Covering the health bars or the masks.

## Left to the builder

PRIM signature details, exact pixel offsets, the PNG's resolution and drawing, how the tinted quad is drawn
(the existing tag shader takes a texture and a colour).
