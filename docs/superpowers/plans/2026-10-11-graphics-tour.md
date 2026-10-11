# Graphics polish: automated PPSSPP-vs-runtime tour — plan

Date: 2026-10-11. Status: not started. Scope agreed with the user: polish existing graphics
(no widescreen / 60 fps for now). Measure first, then fix by impact.

## Goal

A repeatable script that drives the runtime and PPSSPP 1.20.4 through the same route with the same
save, captures both at the same checkpoints, scores the differences and produces a side-by-side
report. It finds unknown bugs and later serves as a regression check for every graphics fix.

## Existing pieces (build/tools, not in the repo yet)

- Runtime: `p3drive.py` (Instance: launch with its own savedata copy, `boot_to_hideout`, `keys`),
  `p3key.py <tag> <port> <keys>` (debug socket `B`/`S`, PNG in shots/<tag>/png), `one_up.py`.
- PPSSPP: isolated copy `build/ppsspp_mp` (no TEXTURES, FileLogging on, ad hoc settings — set
  `EnableWlan`/relay back if needed), `ppkey.py <ws port> <keys> <tag>` (WebSocket
  `input.buttons.press`; screenshots via `winshot.ps1` PrintWindow, window = title bar + menu +
  480x272 at 1x), port from `Get-NetTCPConnection` of PPSSPPWindows64 (was 61085).
- PPSSPP boot keys from the language menu: `D x5, X, W8, X, W10, START, W2, D, W1, X, W8, START,
  W8, X, W4, X, W4, X, W20, X, W28, X` -> hideout (Cuartel selected). Runtime boot: p3drive
  `boot_to_hideout` (flaky: sometimes lands in the Armería; detect and recover).
- Hideout order from Heropuerta: R = Armería, Tótem de equipo, Cuartel, Sukopon, Obelisco Maestro.

## Tasks

1. **Capture parity.** Runtime with `PSPRECOMP_RENDER_SCALE=1`, no `PSPRECOMP_TEXTURES`; PPSSPP
   at 1x, no texture pack. Crop the PPSSPP window capture to the 480x272 game area (find the
   offset once from a known frame). Same savedata in both (copy DECO/savedata into
   ppsspp_mp/memstick/PSP/SAVEDATA before each run). Spanish language in both.
2. **Tour script** `build/tools/gfx_tour.py`: boots both (in parallel), then a list of
   checkpoints, each = key sequence + settle time + name, applied to both, capture both:
   title screen; hideout overview + each building selected; Armería, Cuartel (class/equip),
   Forja, Tótem menus; SQUARE DxD settings menu; Obelisco world map + mission info;
   mission start, mid-mission (drum.py march + attack), FEVER, results screen; a dialogue
   bubble; the OSK. Keep the checkpoint list in a data file so it can grow.
3. **Scoring + report.** Per pair: mean abs diff and % pixels differing > threshold (tgadiff-like),
   ignoring small time-dependent regions if needed (animated sky). Publish an HTML report
   (pairs side by side, sorted worst first, with the diff heat map) as an artifact for the user.
4. **Triage.** From the report build a ranked bug list in this plan: missing/black > wrong
   colors/blending > small offsets. Include known open items: `rojito.png` (red marks under
   HUD chest icons, Desktop), `combate.png` tower that sometimes disappears, barracks preview
   light box.
5. **Fix loop** (one bug per commit): park both on the bad checkpoint, dump the display lists
   (runtime debug socket `D`, PPSSPP `tools/gelist.py pp`), diff draw state, fix in the GE
   runtime or recompiler, rerun the tour to confirm no regressions, user check, push.

## Notes

- Never run the tour while the user plays (it kills psprecomp_runtime.exe).
- Time-dependent scenes (animations, beat flash) will never match pixel-exactly: compare
  structure (missing elements) more than exact values.

## Triage (2026-10-11, tour run7: 32 checkpoints, hideout + Campo de entrenamiento)

Hideout menus (Tótem, Armería, Cuartel, equip, DxD menu, world map, mission info) match PPSSPP
almost pixel-exactly (MAD < 0.4). Differences cluster in the mission scene:

1. **Missing: mission command list** on the bottom bar ("Avanzar: □□□○ Atacar: ○○□○ ...") is never
   drawn by the runtime (PPSSPP shows it from the first command on). Every mission frame.
2. **Missing: wind streaks** (thin white arcs across the sky / over the ground) in the mission.
3. **Offset: beat outline** drawn 1 px left in the runtime (x=4 and 473 vs PPSSPP 5 and 474; top/bottom
   rows match) -> line/rect rasterization rule.
4. Functional, not graphics: once the runtime's timer froze at 0:11 with the opening bubble shown
   while PPSSPP went on (run3; not reproduced in run7).
5. Known open items not on this route: `rojito.png` (red marks under HUD chest icons, prologue),
   `combate.png` (combat-gate tower that sometimes disappears), barracks preview light box.

Not bugs: random loading tips / chest items, pulsing building glow, the few seconds the drum lock
puts between the two sides.
