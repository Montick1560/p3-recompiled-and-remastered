# Patapon 3: progress struct facts for achievements

Field map used by `games/patapon3/runtime/p3_achievements.cpp` (plan
`docs/superpowers/plans/2026-10-18-launcher-part5b-plan2-game-achievements.md`, Task 1; spec
`docs/superpowers/specs/2026-10-18-launcher-part5b-themes-achievements-design.md` §4). Found on 2026-10-18 with the
runtime's debug socket (port 9998, `DECO/build/tools/ach_probe.py`, `ach_chain_check.py`) and the user's decrypted
saves `DECO/build/save_main/plain_DATA04.bin` / `plain_DATA08.bin` (both 178 848 bytes, never committed).

## The struct

The decrypted `SECURE.BIN` (both layers removed) is byte for byte the progress struct in guest RAM: a dump of the
struct with DATA04 loaded differs from the file in 44 bytes only (play time at `+0x14`/`+0xF504`, `+0x18`, `+0xEDAC`,
`+0xF2BC` and a scratch area at `+0x2B7CA..+0x2B805`).

### Pointer chain (same for every release)

`base = rd32(rd32(0x08AABD94) + 0x50)` (the PataponRichPresence chain), i.e. `global = 0x08AABD94, off1 = 0x50`.

| release | chain | title screen (no save loaded) | hideout with DATA04 |
|---|---|---|---|
| dxd-1.1.4 | `0x08AABD94, 0x50` | P=091D0BA0, S=091D1E40, name "", class 2, level 0 | S=091D9AA0 (1st boot), 091D1E40 (2nd boot): name "Edson㊤", class 4, level 35, missions 11610 |
| dxd-1.1.5b | `0x08AABD94, 0x50` | P=091D0B60, S=091D99A0, name "", class 2, level 0 | S=091D99A0: name "Edson㊤", class 4, level 35, missions 11610 |
| vanilla-eu | `0x08AABD94, 0x50` | P=091D09A0, S=091D0AC0, name "", class 2, level 0 | S=091D0AC0: name "Edson㊤", class 4, level 35, missions 11610 |

The struct moves between boots (091D9AA0 vs 091D1E40 on 1.1.4); the chain does not. With DATA08 loaded on 1.1.4 the
same chain gave S=091D15A0 (name "edd", class 2). On the title screen the struct already exists but is empty: the
hero name is empty (the sanity check fails: name length 0) and the hero level is 0 (outside its plausible range).

Sanity check (Task 7 `sane`): hero name at `+0xF1C8` printable UTF-16, 1..15 units then 0; class at `+0x9520` in
1..34 (PataponRichPresence class list).

## Facts

All little endian. `bit` -1 = whole value; `count` 0 = one value.

| fact | offset | size | signed | bit | count | min | max | DATA04 | DATA08 | evidence |
|---|---|---|---|---|---|---|---|---|---|---|
| missions | 0x10 | 4 | no | -1 | 0 | 0 | 1000000 | 11610 | 2 | Live on 1.1.4 with DATA08: 2 while the save is still in the prologue (Continue goes straight into prologue scenes, no hideout); clearing the prologue mission "¡Avance y ataque!" (autoplay) moved it 2 → 3 and the hideout opened. Nothing else in the header moved but play time and `+0x18`. DATA04: 11610 ≥ every per-class sortie count (largest 10642, Yumiyacha), 1878 h of play. |
| hero.level | 0xF294 | 4 | no | -1 | 0 | 1 | 99 | 35 | 1 | Level of the hero in the current class: DATA04 35 = the game's own save detail "Clase: Yumiyacha Nv 35" (PARAM.SFO written by the game) and the Cuartel; DATA08 1 = the Cuartel screen live ("edd · Yarida · Nv 1 · EXP 0/1000", exp at `+0xF290` = 0, then 100 after the mission). 0 on the title screen (no save). |
| playtime.seconds | 0x14 | 4 | no | -1 | 0 | 0 | 2147483647 | 6762511 | 912 | 6762511 s = 1878:28:31, the save detail says "Tiempo de juego: 1878:28:30"; ticks live once a second. Not used by the catalogue. |
| kaching | 0x20 | 4 | no | -1 | 0 | 0 | 999999999 | 148473199 | 246 | DATA08 246 = "Ka-ching 246" on the Fragua screen live. Not used by the catalogue. |
| archfiend.valor | 0xEDEA | 1 | no | -1 | 0 | 0 | 255 | 4 | 0 | live: replaying the Archfiend of Valor dungeon (boss included) moved this byte 4 → 5 and no other byte of the table |
| archfiend.purity | 0xEE12 | 1 | no | -1 | 0 | 0 | 255 | 10 | 0 | live: replaying the Archfiend of Pureza dungeon (boss included) moved this byte 10 → 11 and no other byte of the table |
| archfiend.justice | 0xEE3A | 1 | no | -1 | 0 | 0 | 255 | 5 | 0 | live: replaying the Archfiend of Justicia dungeon (boss included) moved this byte 5 → 6 and no other byte of the table |
| archfiend.impetuosity | 0xEE62 | 1 | no | -1 | 0 | 0 | 255 | 7 | 0 | live: replaying the Archfiend of Impetuosidad dungeon (boss included) moved this byte 7 → 8 and no other byte of the table |
| archfiend.restraint | 0xEE8A | 1 | no | -1 | 0 | 0 | 255 | 2 | 0 | the one slot left after the six replays (by elimination); 2 in DATA04, 0 in DATA08 |
| archfiend.inflexibility | 0xEEB2 | 1 | no | -1 | 0 | 0 | 255 | 10 | 0 | live: replaying the Archfiend of Inflexibilidad dungeon (boss included) moved this byte 10 → 11 and no other byte of the table |
| archfiend.tolerance | 0xEEDA | 1 | no | -1 | 0 | 0 | 255 | 5 | 0 | live: replaying the Archfiend of Tolerancia dungeon (boss included) moved this byte 5 → 6 and no other byte of the table |

Looked at and not usable within the time box (no verified meaning): the 6-byte inventory table at `+0x2C..`
(id, flags, count; 841 ids), the 0x84-byte unit/class records at `+0x9684..+0xEDA4` (level 40 = 0x28 for most
classes in DATA04; their unlock/kind fields are unclear), the two 132-byte flag arrays at `+0x2159C` / `+0x2165C`
(values 0/1/3), `+0xEDCB` (0 → 1 when the prologue mission was cleared, 4 in DATA04) and the last-mission area at
`+0x2A05C..`. No story, Archfiend, Dark Hero, VS, enemy-hero or class-unlock fact could be verified: those
achievements are dropped (below), never guessed.

## Catalogue decisions (the 24 draft ids)

| id | decision | when / reason |
|---|---|---|
| story_prologue | keep | `{"fact": "missions", "op": ">=", "value": 3}` — the prologue is three mandatory missions: DATA08 is in the prologue at 2, the hideout opens when the third is cleared (live, see `missions`). |
| story_first_archfiend | keep (2026-10-18, second pass) | `{"any": [archfiend.* >= 1, the seven]}` |
| story_all_archfiends | keep (2026-10-18, second pass) | `{"all": [archfiend.* >= 1, the seven]}` |
| story_ending | drop | no verified story fact |
| missions_1 | keep | `{"fact": "missions", "op": ">=", "value": 1}` |
| missions_100 | keep | `{"fact": "missions", "op": ">=", "value": 100}` |
| missions_1000 | keep | `{"fact": "missions", "op": ">=", "value": 1000}` |
| heroes_defeated_10 | drop | no verified enemy-hero counter |
| heroes_defeated_100 | drop | no verified enemy-hero counter |
| vs_1 | drop | no verified VS counter |
| vs_50 | drop | no verified VS counter |
| dark_hero_ragewolf | drop | no verified Dark Hero fact |
| dark_hero_naughtyfins | drop | no verified Dark Hero fact |
| dark_hero_sonarchy | drop | no verified Dark Hero fact |
| dark_hero_slogturtle | drop | no verified Dark Hero fact |
| dark_hero_covet_hiss | drop | no verified Dark Hero fact |
| dark_heroes_all | drop | no verified Dark Hero fact |
| hero_level_10 | keep | `{"fact": "hero.level", "op": ">=", "value": 10}` |
| hero_level_30 | keep | `{"fact": "hero.level", "op": ">=", "value": 30}` |
| classes_5 | drop | class-unlock field not verified |
| classes_all | drop | class-unlock field not verified |
| dxd_gigante_king | drop | no verified DLC mission fact |
| secret_heroes_1000 | drop | no verified enemy-hero counter |
| secret_completionist | keep (second pass: its `all` also holds the seven archfiend leaves) | `{"all": [{"fact": "missions", "op": ">=", "value": 1000}, {"fact": "hero.level", "op": ">=", "value": 30}]}` — exactly "every other kept achievement" (those two imply the other five). |

Kept: 7. `story_prologue` survives, so the `arena` theme keeps its lock.

## Earned lists (computed with the table over the saves)

- **DATA04 earns** (sorted): `hero_level_10`, `hero_level_30`, `missions_1`, `missions_100`, `missions_1000`,
  `secret_completionist`, `story_prologue` (7).
- **DATA08 earns** (sorted): `missions_1` (1).

## Second pass (2026-10-18): Archfiends

The user replayed the Archfiend dungeons on their own save (port 9999, read-only snapshots before / after,
`DECO/build/tools/ach_snap.py`, `DECO/build/ach_snaps/`). An 8-slot table at `+0xEDCC` (0x28 bytes per slot) holds,
at slot + 0x1E, how many times each Archfiend was defeated; slot 8 is 0 in both saves and unused. Slots: 1 Valor,
2 Pureza, 3 Justicia, 4 Impetuosidad, 5 Contención (by elimination), 6 Inflexibilidad, 7 Tolerancia. Each replay
moved only its own byte by 1 (and `missions` by 1). The first u32 of each slot (29, 100, 40, 36, 71, 58, 48) did not
move and is not used.

New ids (never published before): `archfiend_valor`, `archfiend_purity`, `archfiend_justice`, `archfiend_impetuosity`,
`archfiend_restraint`, `archfiend_inflexibility`, `archfiend_tolerance` (each `archfiend.<name> >= 1`). Back from the
drop list with their draft meaning: `story_first_archfiend`, `story_all_archfiends` (they never had a `when`, so no
one could have earned them). English names are literal translations of the Spanish release, not checked against
the English release.

- **DATA04 earns now** (sorted): `archfiend_impetuosity`, `archfiend_inflexibility`, `archfiend_justice`,
  `archfiend_purity`, `archfiend_restraint`, `archfiend_tolerance`, `archfiend_valor`, `hero_level_10`,
  `hero_level_30`, `missions_1`, `missions_100`, `missions_1000`, `secret_completionist`, `story_all_archfiends`,
  `story_first_archfiend`, `story_prologue` (16).
- **DATA08 earns** (sorted): `missions_1` (1).

## Goruru (not used, 2026-10-18)

Beating the special mission "Goruru, el engendro legendario" once moved one byte, `+0xEEA2` (slot 6 + 0x0E), 5 → 6
(0 in DATA08); beating it again moved nothing specific. So it is not a defeat counter (a "best level beaten" max is
likely). Left out until it is checked with the game's debug mode.

## Third pass (2026-10-18): team stats (Tótem de equipo)

The team totem screen of the user's save shows "Misiones coops. completadas 1597, Enfrentamientos completados 994,
Reclutado 222, Puntos de tu equipo 106180, Tiempo de juego 1843:53:45" for its last sync (2025/03/08). Those exact
values are a block at `+0x22500` (coop `+0x22534`, versus `+0x22538`, recruited `+0x22540`, points `+0x2254C`,
play time `+0x22584`). The same layout sits 0x13080 bytes earlier at `+0xF480` with the live values (play time
`+0xF504` = the live play time, verified ticking; coop 1622, versus 1009, recruited 222, points 106202 in DATA04,
all ≥ the synced ones); DATA08 is 0 in both blocks. Facts use the live block:

| fact | offset | size | signed | bit | count | min | max | DATA04 | DATA08 | evidence |
|---|---|---|---|---|---|---|---|---|---|---|
| team.coop_missions | 0xF4B4 | 4 | no | -1 | 0 | 0 | 1000000 | 1622 | 0 | totem "Misiones coops. completadas" (synced copy 1597) |
| team.versus | 0xF4B8 | 4 | no | -1 | 0 | 0 | 1000000 | 1009 | 0 | totem "Enfrentamientos completados" (synced copy 994) |
| team.recruited | 0xF4C0 | 4 | no | -1 | 0 | 0 | 1000000 | 222 | 0 | totem "Reclutado" 222 |
| team.points | 0xF4CC | 4 | no | -1 | 0 | 0 | 100000000 | 106202 | 0 | totem "Puntos de tu equipo" (synced copy 106180) |

Not seen moving live (needs online play); verified by the screen values, the twin layout and the live play time.
Achievements: `vs_1` (versus ≥ 1) and `vs_50` (≥ 50) back with their draft meaning ("desafío VS" reworded to the
game's "enfrentamiento"), new `coop_1`, `coop_100`, `recruit_10`, `team_points_10000`; `secret_completionist` gains
their top leaves. DATA04 earns all 22; DATA08 earns `missions_1`. "% victorias" (56 %) is not stored as a number
in the block and "Máx. nivel de clase" is not in it.

### Team ranking screen (Rango inter. del equipo)

"Héroes derrotados — Edson 15,542" = the synced block's `+0x22558` (0x3CB6) exactly; live copy `+0xF4D8` = 15732 in
DATA04, 0 in DATA08. Fact `team.heroes_defeated` (0xF4D8, 4 bytes, 0..100000000). `heroes_defeated_10`,
`heroes_defeated_100` and hidden `secret_heroes_1000` come back with their draft meaning; `secret_completionist`
gains `team.heroes_defeated >= 1000`. DATA04 earns all 25; DATA08 earns `missions_1`.
