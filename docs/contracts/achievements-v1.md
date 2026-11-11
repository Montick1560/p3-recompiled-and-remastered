# Achievements v1 — contract between the game, the launcher and theme authors

Source: `docs/superpowers/specs/2026-10-18-launcher-part5b-themes-achievements-design.md` §2, §3, §4.

## Catalogue — `games/patapon3/achievements.json`

Official and fixed: the project defines it, every player has the same list. The build copies it next to the exe
(`game/<release>/achievements.json` in the portable package); the launcher reads the current release's copy.

```
{ "version": 1, "draft"?: true,
  "rarities"?: { "<key>": { "color": "#RRGGBB", "ms": int, "name": {"es", "en"} }, ... },
  "achievements": [ { "id": "<[a-z0-9_]{1,64}>", "icon": "<emoji>", "hidden": bool, "rarity"?: "<key>",
                      "name": {"es", "en"}, "desc": {"es", "en"},
                      "when"?: { "fact", "op": ">=" | "==" | "!=", "value" } | {"all": [...]} | {"any": [...]} } ] }
```

- An `id` never changes meaning once published (pushed with a verified `when`); a dropped id is never reused for
  another meaning. A draft id that never had a `when` may come back with its draft meaning. `secret_completionist`
  ("every other achievement") gains the leaves of new achievements; whoever earned it keeps it.
- Every entry has a `when` verified against the user's saves and the game (`docs/p3_achievement_facts.md`);
  the game evaluates them (psp_achievements). An id published with a `when` never changes; a dropped id is
  never reused.
- Optional top-level `"credits": {"es", "en"}`: one line the launcher shows at the bottom of Logros (for the
  sources a catalogue owes ideas to). The game ignores it.
- `rarities` lists the tiers in order, lowest first; the first tier is the default. A tier whose `color` is not
  `#RRGGBB` is left out (one `[ACH]` log line); `ms` (unlock toast time) is clamped to 2000..10000. No table =
  one built-in tier `comun` ("Común"/"Common", `#231815`, 4000 ms).
- A missing `rarity`, or one the table does not list, is the first tier.
- Images: `assets/achievements/<id>.jpg` next to the catalogue, 512×512 JPEG (made from the artist's PNG by
  `games/patapon3/scripts/achievement_images.py`); `assets/achievements/_default.jpg` stands in for an id without one.
- Readers ignore unknown keys. Another `version` = no catalogue.

## Earned — `<portable root>/achievements/unlocked.json`

```
{ "version": 1, "earned": { "<id>": { "date": "<ISO-8601 UTC>", "save": "DATA04", "hero": "Edson",
                                      "release": "dxd-1.1.4" } } }
```

- Global on the PC (every release shares it), append-only. **Written only by the game** (temp file + rename);
  the launcher only reads it. Missing, empty or corrupt = nothing earned.
- An earned id the catalogue does not list is ignored by the launcher.

## Themes

A community theme may require an official achievement with `"unlock": "<id>"` in its `theme.json`; it stays
locked until that id is earned. An id that is not in the catalogue keeps the theme locked forever. Themes can
never add achievements.
