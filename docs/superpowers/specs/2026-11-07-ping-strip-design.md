# Partner pings in a fixed strip (missions + VS) — design, 7 Nov 2026

Replaces the mission half of `2026-11-02-ping-under-masks-design.md`. The hideout badges stay as they are.

## Problem (user, 7 Nov, with screenshots)

1. A hero dies -> its mask goes -> every number vanishes (`p3_ping_assign` needs one mask per party slot).
2. Missions that run the other way: the masks stay top-left but the order flips (own gold mask first, not last) ->
   the fixed mask order is rejected -> no numbers.
3. VS: the top is a minimap, no masks -> never a number.

## Decision (user-approved)

No anchoring to masks. A fixed strip, one entry per partner, sorted by the game's player number, own excluded.

| Screen | Where (PSP px, 480x272) | Entry |
|---|---|---|
| Mission | top row, centred in the gap between the masks (end x~121) and the loot (7 icons start x~306): zone x 125..300, y ~12 | `2.Blue` + signal icon + `10` (name cut to 6 chars) |
| VS (up to 8 players) | row under the minimap, between the FEVER worm (ends x~143) and the score box (starts x~368): zone x 145..365, y ~51 | `2` + signal icon + `85` (number only) |
| Hideout | unchanged (badges) | unchanged |

- Number = the one the game shows in the hero label ("4.Fuego"). Name = the hero name the label shows.
- Colours / icon frames / thresholds unchanged: green < 80, yellow < 150, red; icon frame = bars, +4 via server; grey
  icon without number while unknown.
- Equal-width cells across the zone, so entries never move when a number changes.
- Zone coordinates come from the user's screenshots (`Pictures/Screenshots/Screenshot 2026-11-07 170649.png`,
  `171022.png`, `171642.png`); the lab confirms them (mission: Shooshookle only, never the iron door; VS: vs.sh).
- Off with `game.show_ping` / `PSPRECOMP_P3_SHOW_PING=0` as today; only while connected online.
