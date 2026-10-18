# Achievements v1 — contract between the game, the launcher and theme authors

Source: `docs/superpowers/specs/2026-10-18-launcher-part5b-themes-achievements-design.md` §2, §3, §4.

## Catalogue — `games/patapon3/achievements.json`

Official and fixed: the project defines it, every player has the same list. The build copies it next to the exe
(`game/<release>/achievements.json` in the portable package); the launcher reads the current release's copy.

```
{ "version": 1, "draft"?: true,
  "achievements": [ { "id": "<[a-z0-9_]{1,64}>", "icon": "<emoji>", "hidden": bool,
                      "name": {"es", "en"}, "desc": {"es", "en"},
                      "when"?: { "fact", "op": ">=" | "==" | "!=", "value" } | {"all": [...]} | {"any": [...]} } ] }
```

- An `id` never changes meaning once published (pushed with a verified `when`); a dropped id is never reused for
  another meaning. A draft id that never had a `when` may come back with its draft meaning. `secret_completionist`
  ("every other achievement") gains the leaves of new achievements; whoever earned it keeps it.
- Every entry has a `when` verified against the user's saves and the game (`docs/p3_achievement_facts.md`);
  the game evaluates them (psp_achievements). An id published with a `when` never changes; a dropped id is
  never reused.
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
