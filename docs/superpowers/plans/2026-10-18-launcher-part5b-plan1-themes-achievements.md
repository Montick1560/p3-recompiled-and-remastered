# Launcher part 5b, Plan 1 — Themes + Achievements screens (launcher) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Expected execution (confirm with the user before starting):** like part 5 — a NEW session; each of Tasks 1–6 is
> a brief for **DeepSeek v4.1 flash at max effort** (`python ~/.claude/skills/albanil-grok/grok-build.py <brief> --repo
> C:/Users/torso/Documents/deco --model "deepseek/deepseek-flash#max" --timeout 3600 --msg "<no-commit message>"`);
> Claude reads every diff, re-runs every gate, commits and decides; Task 7 is Claude's. Briefs live in
> `DECO/build/briefs_p5b/` (same template as `DECO/build/briefs_p5/`: plan path, line range, "verbatim", no commit,
> gates, report). **Before every run: `Get-Process opencode` must be empty** (ask the user to close their own
> OpenCode; never kill it). One OpenCode session at a time; Monitor reports file changes and "STALL" after 20 min.

**Goal:** The launcher gets menu themes with music, UI sounds and an animated background, a Themes gallery that
previews and applies them, and a Steam-like Achievements screen fed by the official catalogue and by what the game
records in `achievements/unlocked.json`; themes can require an official achievement.

**Architecture:** Two new Rust modules: `achievements.rs` reads the official catalogue from the current release's
folder (`game/<id>/achievements.json`, copied there by psprecomp's build) and the earned list
(`<root>/achievements/unlocked.json`, read only — the game writes it in Plan 2); `themes.rs` lists
`themes/<id>/theme.json` with every new optional key, refuses unsafe file names and computes locks from the
achievements. Tauri commands `list_themes`, `set_theme`, `get_achievements`. The web UI keeps one "shown" theme
(applied, or the one previewed in Temas): CSS variables, a fixed backdrop (looping muted video or image), a
`ThemeAudio` object for looping music and move/confirm/back sounds. Music and video pause while the game runs.
Two new screens, `Achievements` (Logros) and `Themes` (Temas), linked to each other.

**Tech Stack:** Rust 1.96 + Tauri 2.12 (launcher), serde_json; React 19 + TSX + Vitest 5 + Testing Library;
Node 22 `node:test` for scripts; FFmpeg 8.1 (on PATH, winget build) only to (re)generate the sample media.

**Spec:** `docs/superpowers/specs/2026-10-18-launcher-part5b-themes-achievements-design.md` (approved; read §2, §3
and §6 first). Plan 2 (game: progress struct, evaluator, toast) is a separate plan written after this one ships.

## Global Constraints

- Theme folder `themes/<id>/`, `<id>` = folder name matching `[a-z0-9_-]+`; `theme.json` keys, all optional:
  `name`, `author`, `colors` (`bg`, `text`, `accent`, `panel`, `panelText`, `muted`), `font`, `background_video`,
  `music`, `music_volume` (0–100, default 100), `sounds` {`move`, `confirm`, `back`}, `unlock`; fixed optional files
  `background.png`, `logo.png`, `preview.png`, `theme.css`. The part 1 form `{"colors": {...}, "font": null}` must
  keep loading.
- File names inside a theme: relative, `/`-separated, no `..`, no `.` segment, no absolute path, no `\`, no `:`.
  A bad or missing file drops that field (one `[THEME]` stderr line); a `theme.json` that cannot be read skips the
  theme (one line). `default` is always listed first (built-in fallback when its folder is missing) and is never
  locked. `unlock` naming an id that is not in the catalogue = locked forever (one line).
- Catalogue `game/<current release>/achievements.json`, else the first `game/*/achievements.json` by name:
  `{"version": 1, "achievements": [{"id", "icon", "hidden", "name": {"es","en"}, "desc": {"es","en"}, "when"?}]}`;
  ids `[a-z0-9_]{1,64}`; unknown keys (`when`, `draft`) ignored by the launcher. Another `version` = no catalogue.
- Earned `<root>/achievements/unlocked.json` = `{"version": 1, "earned": {"<id>": {"date", "save", "hero",
  "release"}}}`. Missing / empty / corrupt = nothing earned. **The launcher never creates or writes this file.**
- Settings (contract `psprecomp/docs/contracts/settings-v1.md`, still v1): `[launcher] theme = <id>` (default
  `default`, written only by `set_theme`), `[launcher] music_volume` = `0`,`10`,…,`100` (default `70`, an `enum` in
  `core_options.json`, shown in Options › General). `[game] achievement_toasts` is Plan 2's.
- Music volume = theme `music_volume` / 100 × `[launcher] music_volume` / 100. Music and the background video pause
  while the game runs and resume when the launcher comes back; no UI sounds while the game runs.
- Main menu order: Iniciar · Partidas · Logros · Temas · Opciones · Salir.
- No game art or audio in themes: the sample media are synthesized by `scripts/make-sample-themes.mjs` (FFmpeg).
- The catalogue shipped by this plan is a **draft** (`"draft": true`, no `when`): Plan 2 verifies every entry and
  may drop some; nothing in this plan may depend on an entry other than `story_prologue` (the `arena` theme's lock).
- Strings shown to the player only in `lang/es.json` / `lang/en.json`; code, comments and commits in English.
- Commits: launcher on `main`, psprecomp on `patapon3-windows`, both with
  `git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit`; body ends with
  `Verified: …` and `Agent: …`; **no `Co-Authored-By`**. Push nothing (launcher `origin` and psprecomp `p3` only on
  the user's explicit OK).

## Review Focus

1. A broken or hostile community theme (bad JSON, `../` paths, missing files, `music_volume: 250`, a folder named
   `Mayus`) never breaks the launcher: the theme or the field is dropped (Task 2 `unsafe_or_missing_files_are_dropped`,
   `broken_and_badly_named_themes_are_skipped`).
2. `unlocked.json` missing, empty, corrupt or from a future game: nothing earned, the file stays byte-identical
   (Task 1 `earned_missing_empty_or_corrupt_is_nothing_and_never_written`).
3. `settings.ini` says `theme = arena` but the achievement is no longer earned (save folder moved, file deleted):
   default is shown, the setting is left alone (Task 2 `themes_follow_achievements_and_settings`).
4. While the game runs nothing in the launcher costs CPU or makes noise: music and video paused, no sounds from the
   gamepad's nav events (Task 3 Start test `theme music pauses while the game runs`, Task 7 CPU check).
5. Logros ↔ Temas cross-links never stack screens (Esc ping-pong) (Task 5 `ThemesFlow.test.tsx`).

## File map

psprecomp (`DECO/psprecomp`, branch `patapon3-windows`):
- `games/patapon3/achievements.json` (new, T1), `runtime/CMakeLists.txt` (T1 copy rule),
  `docs/contracts/achievements-v1.md` (new, T1), `docs/contracts/settings-v1.md` (T2).

launcher (`DECO/launcher`, branch `main`):
- T1: `src-tauri/src/achievements.rs` (new), `src-tauri/src/lib.rs`, `scripts/make-portable.mjs`,
  `scripts/make-portable.test.mjs`.
- T2: `src-tauri/src/themes.rs` (new), `src-tauri/src/achievements.rs`, `src-tauri/src/assets.rs`,
  `src-tauri/src/lib.rs`, `src-tauri/src/settings.rs` (test), `src-tauri/core_options.json`,
  `src-tauri/tauri.conf.json`.
- T3: `src/types.ts`, `src/api.ts`, `src/theme.ts`, `src/theme.test.ts` (new), `src/audio.ts` (new),
  `src/audio.test.ts` (new), `src/test-fixtures.ts` (new), `src/components/Backdrop.tsx` (new), `src/App.tsx`,
  `src/screens/MainMenu.tsx`, `src/screens/MainMenu.test.tsx`, `src/screens/Start.test.tsx`, `src/setupTests.ts`,
  `src/styles.css`, `lang/es.json`, `lang/en.json`, `src-tauri/tauri.conf.json`.
- T4: `src/achievements.ts` (new), `src/achievements.test.ts` (new), `src/screens/Achievements.tsx` (new),
  `src/screens/Achievements.test.tsx` (new), `src/App.tsx`, `src/styles.css`, `lang/es.json`, `lang/en.json`.
- T5: `src/screens/Themes.tsx` (new), `src/screens/Themes.test.tsx` (new), `src/screens/ThemesFlow.test.tsx`
  (new), `src/App.tsx`, `src/styles.css`, `lang/es.json`, `lang/en.json`.
- T6: `scripts/make-sample-themes.mjs` (new), `scripts/themes.test.mjs` (new), `themes/default/theme.json`,
  `themes/noche/*` (new), `themes/arena/*` (new), `README.md`.

Gates (exact commands, Git Bash):
- Rust: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q 2>&1 | grep -E "^test result|FAILED|panicked"`
  and `cargo build -q 2>&1 | grep -c warning` (must print `0`).
- UI: `cd /c/Users/torso/Documents/deco/launcher && npx vitest run 2>&1 | tail -5` and `npx tsc --noEmit`.
- Scripts: `cd /c/Users/torso/Documents/deco/launcher && node --test scripts/*.test.mjs 2>&1 | tail -8`.
- Baseline before Task 1 (2026-10-18): cargo 87 passed + 2 ignored, Vitest 53/53, tsc clean.

---

### Task 1: Official catalogue (draft), its copy rule, and reading achievements (psprecomp + launcher Rust)

**Files:**
- Create: `psprecomp/games/patapon3/achievements.json`, `psprecomp/docs/contracts/achievements-v1.md`,
  `launcher/src-tauri/src/achievements.rs`
- Modify: `psprecomp/runtime/CMakeLists.txt` (after the `_game_options` block, ~line 340),
  `launcher/src-tauri/src/lib.rs`, `launcher/scripts/make-portable.mjs`, `launcher/scripts/make-portable.test.mjs`

**Interfaces:**
- Consumes: `crate::options::Localized { pub es: String, pub en: String }`, `crate::releases::{valid_id, release_dir}`,
  `versions_payload(root).current: Option<String>` (lib.rs).
- Produces: `achievements::{Achievement { id, icon: String, hidden: bool, name: Localized, desc: Localized },
  Earned { date, save, hero, release: String }, Snapshot { catalogue: Vec<Achievement>, earned: BTreeMap<String, Earned> },
  catalogue_path(root, current: Option<&str>) -> Option<PathBuf>, load_catalogue(&Path) -> Vec<Achievement>,
  unlocked_path(root) -> PathBuf, load_earned(root) -> BTreeMap<String, Earned>, snapshot(root, current) -> Snapshot}`;
  Tauri command `get_achievements() -> Snapshot` (JSON `{catalogue: [...], earned: {...}}`).

- [ ] **Step 1: Write the draft catalogue** `psprecomp/games/patapon3/achievements.json` (exactly this; UTF-8, no BOM):

```json
{
  "version": 1,
  "draft": true,
  "achievements": [
    { "id": "story_prologue", "icon": "📜", "hidden": false, "name": { "es": "El despertar", "en": "The Awakening" }, "desc": { "es": "Completa el prólogo.", "en": "Clear the prologue." } },
    { "id": "story_first_archfiend", "icon": "👹", "hidden": false, "name": { "es": "Primer archidemonio", "en": "First Archfiend" }, "desc": { "es": "Derrota a tu primer archidemonio.", "en": "Defeat your first Archfiend." } },
    { "id": "story_all_archfiends", "icon": "🔥", "hidden": false, "name": { "es": "Siete pecados", "en": "Seven Sins" }, "desc": { "es": "Derrota a todos los archidemonios.", "en": "Defeat every Archfiend." } },
    { "id": "story_ending", "icon": "🌅", "hidden": false, "name": { "es": "Fin del viaje", "en": "Journey's End" }, "desc": { "es": "Llega al final de la historia.", "en": "Reach the end of the story." } },
    { "id": "missions_1", "icon": "🥁", "hidden": false, "name": { "es": "Primer tambor", "en": "First Drum" }, "desc": { "es": "Completa una misión.", "en": "Clear a mission." } },
    { "id": "missions_100", "icon": "🎖️", "hidden": false, "name": { "es": "Veterano", "en": "Veteran" }, "desc": { "es": "Completa 100 misiones.", "en": "Clear 100 missions." } },
    { "id": "missions_1000", "icon": "🏅", "hidden": false, "name": { "es": "Incansable", "en": "Tireless" }, "desc": { "es": "Completa 1000 misiones.", "en": "Clear 1000 missions." } },
    { "id": "heroes_defeated_10", "icon": "⚔️", "hidden": false, "name": { "es": "Cazahéroes", "en": "Hero Hunter" }, "desc": { "es": "Derrota a 10 héroes enemigos.", "en": "Defeat 10 enemy heroes." } },
    { "id": "heroes_defeated_100", "icon": "🗡️", "hidden": false, "name": { "es": "Azote de héroes", "en": "Bane of Heroes" }, "desc": { "es": "Derrota a 100 héroes enemigos.", "en": "Defeat 100 enemy heroes." } },
    { "id": "vs_1", "icon": "🏟️", "hidden": false, "name": { "es": "Primer VS", "en": "First VS" }, "desc": { "es": "Completa un desafío VS.", "en": "Clear a VS challenge." } },
    { "id": "vs_50", "icon": "🏆", "hidden": false, "name": { "es": "Campeón del coliseo", "en": "Arena Champion" }, "desc": { "es": "Completa 50 desafíos VS.", "en": "Clear 50 VS challenges." } },
    { "id": "dark_hero_ragewolf", "icon": "🐺", "hidden": false, "name": { "es": "Lobo domado", "en": "Wolf Tamed" }, "desc": { "es": "Derrota a Ragewolf.", "en": "Defeat Ragewolf." } },
    { "id": "dark_hero_naughtyfins", "icon": "🦈", "hidden": false, "name": { "es": "Aletas quietas", "en": "Fins Stilled" }, "desc": { "es": "Derrota a Naughtyfins.", "en": "Defeat Naughtyfins." } },
    { "id": "dark_hero_sonarchy", "icon": "🦇", "hidden": false, "name": { "es": "Silencio", "en": "Silence" }, "desc": { "es": "Derrota a Sonarchy.", "en": "Defeat Sonarchy." } },
    { "id": "dark_hero_slogturtle", "icon": "🐢", "hidden": false, "name": { "es": "Caparazón roto", "en": "Shell Cracked" }, "desc": { "es": "Derrota a Slogturtle.", "en": "Defeat Slogturtle." } },
    { "id": "dark_hero_covet_hiss", "icon": "🐍", "hidden": false, "name": { "es": "Sin codicia", "en": "No More Greed" }, "desc": { "es": "Derrota a Covet-Hiss.", "en": "Defeat Covet-Hiss." } },
    { "id": "dark_heroes_all", "icon": "🌑", "hidden": false, "name": { "es": "Ocaso de los Héroes Oscuros", "en": "Dusk of the Dark Heroes" }, "desc": { "es": "Derrota a los cinco Héroes Oscuros.", "en": "Defeat all five Dark Heroes." } },
    { "id": "hero_level_10", "icon": "⭐", "hidden": false, "name": { "es": "Héroe en ciernes", "en": "Budding Hero" }, "desc": { "es": "Lleva al héroe al nivel 10.", "en": "Bring the hero to level 10." } },
    { "id": "hero_level_30", "icon": "🌟", "hidden": false, "name": { "es": "Superhéroe", "en": "Superhero" }, "desc": { "es": "Lleva al héroe al nivel 30.", "en": "Bring the hero to level 30." } },
    { "id": "classes_5", "icon": "🎭", "hidden": false, "name": { "es": "Muchas caras", "en": "Many Faces" }, "desc": { "es": "Desbloquea 5 clases.", "en": "Unlock 5 classes." } },
    { "id": "classes_all", "icon": "👑", "hidden": false, "name": { "es": "Maestro de clases", "en": "Class Master" }, "desc": { "es": "Desbloquea todas las clases.", "en": "Unlock every class." } },
    { "id": "dxd_gigante_king", "icon": "🗿", "hidden": false, "name": { "es": "Rey caído", "en": "Fallen King" }, "desc": { "es": "Derrota al Gigante King (DxD).", "en": "Defeat the Gigante King (DxD)." } },
    { "id": "secret_heroes_1000", "icon": "💀", "hidden": true, "name": { "es": "Leyenda negra", "en": "Dark Legend" }, "desc": { "es": "Derrota a 1000 héroes enemigos.", "en": "Defeat 1000 enemy heroes." } },
    { "id": "secret_completionist", "icon": "🎨", "hidden": true, "name": { "es": "Patapon total", "en": "Total Patapon" }, "desc": { "es": "Consigue todos los demás logros.", "en": "Earn every other achievement." } }
  ]
}
```

- [ ] **Step 2: Write the contract** `psprecomp/docs/contracts/achievements-v1.md`:

````markdown
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

- An `id` never changes once published (pushed with a verified `when`); a dropped id is never reused.
- `draft: true` (launcher part 5b Plan 1): names and texts are provisional and no entry has `when` yet. The game
  (Plan 2) evaluates only entries with a `when`; an entry enters for good only when its fact is verified.
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
````

- [ ] **Step 3: Copy rule in `psprecomp/runtime/CMakeLists.txt`.** Right after the `endif()` that closes the
  `_game_options` block (the one copying `game_options.json`), add:

```cmake
# Official achievement catalogue (docs/contracts/achievements-v1.md), read by the launcher.
set(_achievements "${CMAKE_CURRENT_SOURCE_DIR}/../games/${PSPRECOMP_GAME}/achievements.json")
if(EXISTS "${_achievements}")
    add_custom_command(TARGET psprecomp_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_achievements}"
                "$<TARGET_FILE_DIR:psprecomp_runtime>/achievements.json")
endif()
```

- [ ] **Step 4: Check the copy rule.** Run: `cmake --build C:/Users/torso/Documents/deco/build/rtw 2>&1 | tail -2 &&
  ls -la C:/Users/torso/Documents/deco/build/rtw/achievements.json`
  Expected: the build ends without errors (ninja relinks because the link command changed) and the file exists with
  the same size as `games/patapon3/achievements.json`. If the exe is locked (the user is playing from `build/rtw`),
  stop and report instead of retrying.

- [ ] **Step 5: Write the failing tests.** Create `launcher/src-tauri/src/achievements.rs` with only the test module
  below, and add `mod achievements;` to `launcher/src-tauri/src/lib.rs` (alphabetical: right before `mod assets;`).

```rust
#[cfg(test)]
mod tests {
    use super::*;
    const CAT: &str = r#"{"version":1,"achievements":[
      {"id":"story_prologue","icon":"📜","name":{"es":"El despertar","en":"The Awakening"},"desc":{"es":"Completa el prólogo.","en":"Clear the prologue."}},
      {"id":"secret_x","icon":"❔","hidden":true,"name":{"es":"S","en":"S"},"desc":{"es":"d","en":"d"},"when":{"fact":"a.b","op":">=","value":1}},
      {"id":"Bad Id","name":{"es":"x","en":"x"},"desc":{"es":"x","en":"x"}},
      {"id":"story_prologue","name":{"es":"dup","en":"dup"},"desc":{"es":"x","en":"x"}}]}"#;
    fn put(root: &Path, rel: &str, text: &str) {
        let p = root.join(rel);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, text).unwrap();
    }
    #[test] fn catalogue_keeps_valid_unique_entries_and_ignores_when() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "game/dxd-1.1.4/achievements.json", CAT);
        let c = load_catalogue(&d.path().join("game/dxd-1.1.4/achievements.json"));
        assert_eq!(c.iter().map(|a| a.id.as_str()).collect::<Vec<_>>(), ["story_prologue", "secret_x"]);
        assert_eq!(c[0].name.es, "El despertar");
        assert!(c[1].hidden && !c[0].hidden);
    }
    #[test] fn catalogue_errors_give_nothing() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "a.json", "{");
        put(d.path(), "b.json", r#"{"version":2,"achievements":[]}"#);
        assert!(load_catalogue(&d.path().join("a.json")).is_empty());
        assert!(load_catalogue(&d.path().join("b.json")).is_empty());
        assert!(load_catalogue(&d.path().join("missing.json")).is_empty());
    }
    #[test] fn catalogue_comes_from_the_current_release_then_any() {
        let d = tempfile::tempdir().unwrap();
        assert_eq!(catalogue_path(d.path(), Some("dxd-1.1.4")), None);
        put(d.path(), "game/vanilla-eu/achievements.json", CAT);
        assert_eq!(catalogue_path(d.path(), Some("dxd-1.1.4")), Some(d.path().join("game/vanilla-eu/achievements.json")));
        put(d.path(), "game/dxd-1.1.4/achievements.json", CAT);
        assert_eq!(catalogue_path(d.path(), Some("dxd-1.1.4")), Some(d.path().join("game/dxd-1.1.4/achievements.json")));
        assert_eq!(catalogue_path(d.path(), Some("../x")), Some(d.path().join("game/dxd-1.1.4/achievements.json")),
                   "a bad id falls back to the first by name");
    }
    #[test] fn earned_missing_empty_or_corrupt_is_nothing_and_never_written() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "game/dxd-1.1.4/achievements.json", CAT);
        assert!(snapshot(d.path(), Some("dxd-1.1.4")).earned.is_empty());
        assert!(!unlocked_path(d.path()).exists(), "reading never creates the file");
        for bad in ["", "{", "[]", r#"{"version":1,"earned":{"story_prologue":"yes"}}"#] {
            put(d.path(), "achievements/unlocked.json", bad);
            assert!(snapshot(d.path(), Some("dxd-1.1.4")).earned.is_empty(), "{bad:?}");
            assert_eq!(std::fs::read_to_string(unlocked_path(d.path())).unwrap(), bad, "left as it was");
        }
    }
    #[test] fn earned_entries_follow_the_catalogue() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "game/dxd-1.1.4/achievements.json", CAT);
        put(d.path(), "achievements/unlocked.json", r#"{"version":1,"earned":{
            "story_prologue":{"date":"2026-10-18T12:00:00Z","save":"DATA04","hero":"Edson","release":"dxd-1.1.4"},
            "gone_id":{"date":"2026-10-18T12:00:00Z"}}}"#);
        let s = snapshot(d.path(), Some("dxd-1.1.4"));
        assert_eq!(s.earned.keys().collect::<Vec<_>>(), ["story_prologue"]);
        assert_eq!(s.earned["story_prologue"].hero, "Edson");
        assert_eq!(s.earned["story_prologue"].release, "dxd-1.1.4");
    }
    #[test] fn the_official_catalogue_is_valid() {
        let p = format!("{}/../../psprecomp/games/patapon3/achievements.json", env!("CARGO_MANIFEST_DIR"));
        let raw: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&p).unwrap()).unwrap();
        let n = raw["achievements"].as_array().unwrap().len();
        let c = load_catalogue(Path::new(&p));
        assert_eq!(c.len(), n, "every entry is valid and unique");
        assert!(n >= 20, "{n}");
        assert!(c.iter().any(|a| a.id == "story_prologue"), "the sample theme 'arena' needs it");
        assert!(c.iter().any(|a| a.hidden));
        for a in &c {
            assert!(!a.icon.is_empty() && !a.name.es.is_empty() && !a.name.en.is_empty()
                    && !a.desc.es.is_empty() && !a.desc.en.is_empty(), "{}", a.id);
        }
    }
}
```

- [ ] **Step 6: Run them to see them fail.**
  Run: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q achievements 2>&1 | tail -5`
  Expected: compile errors (`cannot find function load_catalogue`, `snapshot`, …).

- [ ] **Step 7: Implement** — put this above the test module in `achievements.rs`:

```rust
//! The official achievement catalogue (game/<id>/achievements.json, copied there by psprecomp's
//! build) and what this PC earned (achievements/unlocked.json, written only by the game).
//! Contract: psprecomp docs/contracts/achievements-v1.md.
use crate::options::Localized;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

#[derive(Deserialize, Serialize, Clone, Debug, PartialEq)]
pub struct Achievement {
    pub id: String,
    #[serde(default)] pub icon: String,
    #[serde(default)] pub hidden: bool,
    pub name: Localized,
    pub desc: Localized,
}

/// One earned achievement: when (ISO-8601 UTC), with which save and hero, in which release.
#[derive(Deserialize, Serialize, Clone, Debug, PartialEq, Default)]
pub struct Earned {
    #[serde(default)] pub date: String,
    #[serde(default)] pub save: String,
    #[serde(default)] pub hero: String,
    #[serde(default)] pub release: String,
}

#[derive(Serialize, Debug, Default, PartialEq)]
pub struct Snapshot { pub catalogue: Vec<Achievement>, pub earned: BTreeMap<String, Earned> }

#[derive(Deserialize)]
struct Catalogue { version: u32, achievements: Vec<Achievement> }

#[derive(Deserialize)]
struct Unlocked { #[serde(default)] earned: BTreeMap<String, Earned> }

pub fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 64 && id.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
}

/// game/<current>/achievements.json, else the first game/*/achievements.json by name (every
/// release carries the same catalogue).
pub fn catalogue_path(root: &Path, current: Option<&str>) -> Option<PathBuf> {
    if let Some(id) = current.filter(|id| crate::releases::valid_id(id)) {
        let p = crate::releases::release_dir(root, id).join("achievements.json");
        if p.is_file() { return Some(p); }
    }
    let mut found: Vec<PathBuf> = std::fs::read_dir(root.join("game")).ok()?
        .flatten().map(|e| e.path().join("achievements.json")).filter(|p| p.is_file()).collect();
    found.sort();
    found.into_iter().next()
}

/// The catalogue's valid, unique entries; an unreadable file or another version gives none.
pub fn load_catalogue(path: &Path) -> Vec<Achievement> {
    let parsed = std::fs::read_to_string(path).map_err(|e| e.to_string())
        .and_then(|t| serde_json::from_str::<Catalogue>(&t).map_err(|e| e.to_string()));
    let c = match parsed {
        Ok(c) if c.version == 1 => c,
        Ok(c) => { eprintln!("[ACH] {}: version {} is not 1", path.display(), c.version); return vec![]; }
        Err(e) => { eprintln!("[ACH] {}: {e}", path.display()); return vec![]; }
    };
    let mut out: Vec<Achievement> = Vec::new();
    for a in c.achievements {
        if !valid_id(&a.id) || out.iter().any(|b| b.id == a.id) {
            eprintln!("[ACH] {}: entry {:?} skipped", path.display(), a.id);
            continue;
        }
        out.push(a);
    }
    out
}

pub fn unlocked_path(root: &Path) -> PathBuf { root.join("achievements").join("unlocked.json") }

/// What the game recorded; missing, empty or corrupt = nothing earned. Read only: the launcher
/// never writes this file.
pub fn load_earned(root: &Path) -> BTreeMap<String, Earned> {
    std::fs::read_to_string(unlocked_path(root)).ok()
        .and_then(|t| serde_json::from_str::<Unlocked>(&t).ok())
        .map(|u| u.earned).unwrap_or_default()
}

/// The catalogue plus the earned entries it lists (an id it does not list is not shown).
pub fn snapshot(root: &Path, current: Option<&str>) -> Snapshot {
    let catalogue = catalogue_path(root, current).map(|p| load_catalogue(&p)).unwrap_or_default();
    let mut earned = load_earned(root);
    earned.retain(|id, _| catalogue.iter().any(|a| &a.id == id));
    Snapshot { catalogue, earned }
}
```

  In `lib.rs`, add the command (next to `list_versions`) and register it in `generate_handler!` (append
  `get_achievements` after `open_texture_page`):

```rust
#[tauri::command]
fn get_achievements() -> achievements::Snapshot {
    let root = settings::root_dir();
    let current = versions_payload(&root).current;
    achievements::snapshot(&root, current.as_deref())
}
```

- [ ] **Step 8: Run the Rust gates.** Expected: all pass (87 + 6 = 93 passed, 2 ignored); `cargo build` prints `0`
  warnings.

- [ ] **Step 9: Package the catalogue.** In `launcher/scripts/make-portable.mjs`: in the header comment change
  `(exe + DLLs +\n// game_options.json + release.json)` to `(exe + DLLs +\n// game_options.json + release.json + achievements.json)`,
  and change the copy condition line to:

```js
    if (f === exe || f.endsWith(".dll") || f === "game_options.json" || f === "release.json" || f === "achievements.json") cpSync(join(dir, f), join(dst, f));
```

  In `scripts/make-portable.test.mjs`, `fakeGame` gets one more line after the `release.json` write:

```js
  writeFileSync(join(dir, "achievements.json"), JSON.stringify({ version: 1, achievements: [] }));
```

  and the first test gets, after `assert.ok(existsSync(join(out, "game", "vanilla-eu", "release.json")));`:

```js
  assert.ok(existsSync(join(out, "game", "dxd-1.1.4", "achievements.json")), "the official catalogue travels with each release");
```

- [ ] **Step 10: Run the scripts gate.** Expected: `# pass 3` (or the current count), `# fail 0`.

- [ ] **Step 11: Commit (two repos).**

```bash
cd /c/Users/torso/Documents/deco/psprecomp
git add games/patapon3/achievements.json docs/contracts/achievements-v1.md runtime/CMakeLists.txt
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "feat(achievements): draft official catalogue, contract and build copy" -m "Verified: cmake --build build/rtw copies achievements.json next to the exe" -m "Agent: <builder>"
cd /c/Users/torso/Documents/deco/launcher
git add src-tauri/src/achievements.rs src-tauri/src/lib.rs scripts/make-portable.mjs scripts/make-portable.test.mjs
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "feat(launcher): read the achievement catalogue and what the game earned" -m "Verified: cargo test <N> passed; node --test scripts <N> pass" -m "Agent: <builder>"
```

---

### Task 2: Themes (Rust): format, safety, locks, commands, music volume option, asset scope

**Files:**
- Create: `launcher/src-tauri/src/themes.rs`
- Modify: `launcher/src-tauri/src/achievements.rs` (add `impl Snapshot`), `launcher/src-tauri/src/assets.rs`
  (drop `get_theme`), `launcher/src-tauri/src/lib.rs`, `launcher/src-tauri/src/settings.rs` (one test),
  `launcher/src-tauri/core_options.json`, `launcher/src-tauri/tauri.conf.json`,
  `psprecomp/docs/contracts/settings-v1.md`

**Interfaces:**
- Consumes: Task 1 `achievements::{Achievement, Earned, Snapshot, snapshot}`; `settings::{launcher_get, launcher_set,
  root_dir}`; `versions_payload`.
- Produces: `themes::{ThemeInfo, Sounds, DEFAULT, valid_id, safe_name, load, list(root, &Snapshot) -> Vec<ThemeInfo>,
  applied(&[ThemeInfo], Option<&str>) -> &ThemeInfo}`; `Snapshot::{knows, is_earned}`; Tauri commands
  `list_themes() -> {themes: ThemeInfo[], applied: string}`, `set_theme(id) -> Result<(), String>`; `get_theme` is
  removed. `ThemeInfo` JSON: `{id, name, author, colors, font, background, background_video, logo, preview, css,
  music, music_volume, sounds: {move, confirm, back}, unlock, locked}` (files = absolute paths or `null`).
  Option `launcher.music_volume` in `get_options`.

- [ ] **Step 1: Add the lock helpers to `achievements.rs`** (right after the `Snapshot` struct):

```rust
impl Snapshot {
    pub fn knows(&self, id: &str) -> bool { self.catalogue.iter().any(|a| a.id == id) }
    pub fn is_earned(&self, id: &str) -> bool { self.earned.contains_key(id) }
}
```

- [ ] **Step 2: Write the failing tests.** Create `launcher/src-tauri/src/themes.rs` with only this test module and
  add `mod themes;` to `lib.rs` (alphabetical: after `mod settings;`/`mod sfo;`, before `mod textures;`).

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use crate::achievements::{Achievement, Earned, Snapshot};
    use crate::options::Localized;
    fn put(root: &Path, rel: &str, bytes: &[u8]) {
        let p = root.join(rel);
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, bytes).unwrap();
    }
    fn ach(ids: &[&str], earned: &[&str]) -> Snapshot {
        let l = |s: &str| Localized { es: s.into(), en: s.into() };
        Snapshot {
            catalogue: ids.iter().map(|id| Achievement { id: id.to_string(), icon: "x".into(), hidden: false, name: l(id), desc: l(id) }).collect(),
            earned: earned.iter().map(|id| (id.to_string(), Earned::default())).collect(),
        }
    }
    #[test] fn old_form_still_loads() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "themes/default/theme.json", br##"{"colors":{"bg":"#0b0b0e"},"font":null}"##);
        put(d.path(), "themes/default/background.png", b"png");
        put(d.path(), "themes/default/theme.css", b".x{}");
        let l = list(d.path(), &Snapshot::default());
        assert_eq!(l.len(), 1);
        let t = &l[0];
        assert_eq!((t.id.as_str(), t.name.as_str(), t.colors["bg"].as_str()), ("default", "default", "#0b0b0e"));
        assert!(t.background.as_deref().unwrap().ends_with("background.png"));
        assert_eq!((t.logo.clone(), t.font.clone(), t.music.clone(), t.css.as_deref()), (None, None, None, Some(".x{}")));
        assert_eq!((t.music_volume, t.locked), (100, false));
    }
    #[test] fn new_form_with_every_key() {
        let d = tempfile::tempdir().unwrap();
        for f in ["bg.webm", "music.ogg", "move.ogg", "confirm.ogg", "sfx/back.ogg", "font.ttf", "preview.png", "logo.png"] {
            put(d.path(), &format!("themes/noche/{f}"), b"x");
        }
        put(d.path(), "themes/noche/theme.json", r##"{"name":"Noche","author":"Patapon Community","colors":{"accent":"#f5a400"},
            "font":"font.ttf","background_video":"bg.webm","music":"music.ogg","music_volume":80,
            "sounds":{"move":"move.ogg","confirm":"confirm.ogg","back":"sfx/back.ogg"}}"##.as_bytes());
        let l = list(d.path(), &Snapshot::default());
        assert_eq!(l.iter().map(|t| t.id.as_str()).collect::<Vec<_>>(), ["default", "noche"], "built-in default first");
        assert_eq!(l[0].name, "Patapon");
        let t = &l[1];
        assert_eq!((t.name.as_str(), t.author.as_str(), t.music_volume), ("Noche", "Patapon Community", 80));
        for p in [&t.font, &t.background_video, &t.music, &t.sounds.mv, &t.sounds.confirm, &t.sounds.back, &t.preview, &t.logo] {
            assert!(p.as_deref().is_some_and(|p| Path::new(p).is_file()), "{p:?}");
        }
    }
    #[test] fn unsafe_or_missing_files_are_dropped() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "secret.ogg", b"x");
        put(d.path(), "themes/bad/theme.json", br#"{"music":"../../secret.ogg","background_video":"C:/x.webm",
            "font":"a\\b.ttf","sounds":{"move":"/abs.ogg","confirm":"missing.ogg","back":"./x.ogg"},"music_volume":250}"#);
        let t = &list(d.path(), &Snapshot::default())[1];
        assert_eq!((t.music.clone(), t.background_video.clone(), t.font.clone()), (None, None, None));
        assert_eq!(t.sounds, Sounds::default());
        assert_eq!(t.music_volume, 100, "clamped");
        for n in ["a.ogg", "sfx/a.ogg"] { assert!(safe_name(n), "{n}"); }
        for n in ["", "../a", "a/../b", "a\\b", "C:a", "/a", "a//b", "./a"] { assert!(!safe_name(n), "{n}"); }
    }
    #[test] fn broken_and_badly_named_themes_are_skipped() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "themes/rota/theme.json", b"{");
        put(d.path(), "themes/Mayus/theme.json", b"{}");
        put(d.path(), "themes/vacia/readme.txt", b"no theme.json");
        put(d.path(), "themes/ok/theme.json", b"{}");
        assert_eq!(list(d.path(), &Snapshot::default()).iter().map(|t| t.id.as_str()).collect::<Vec<_>>(), ["default", "ok"]);
    }
    #[test] fn locks_follow_official_earned_achievements() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "themes/default/theme.json", br#"{"unlock":"story_prologue"}"#);
        put(d.path(), "themes/arena/theme.json", br#"{"unlock":"story_prologue"}"#);
        put(d.path(), "themes/fake/theme.json", br#"{"unlock":"made_up"}"#);
        let lock = |s: &Snapshot| list(d.path(), s).into_iter().map(|t| (t.id, t.locked)).collect::<Vec<(String, bool)>>();
        assert_eq!(lock(&ach(&["story_prologue"], &[])),
                   [("default".to_string(), false), ("arena".to_string(), true), ("fake".to_string(), true)]);
        assert_eq!(lock(&ach(&["story_prologue"], &["story_prologue"])),
                   [("default".to_string(), false), ("arena".to_string(), false), ("fake".to_string(), true)]);
        assert_eq!(list(d.path(), &Snapshot::default())[0].unlock, None, "default can never be locked");
    }
    #[test] fn applied_theme_falls_back_to_default() {
        let d = tempfile::tempdir().unwrap();
        put(d.path(), "themes/noche/theme.json", b"{}");
        put(d.path(), "themes/arena/theme.json", br#"{"unlock":"story_prologue"}"#);
        let l = list(d.path(), &ach(&["story_prologue"], &[]));
        assert_eq!(applied(&l, Some("noche")).id, "noche");
        assert_eq!(applied(&l, Some("arena")).id, "default", "locked");
        assert_eq!(applied(&l, Some("gone")).id, "default");
        assert_eq!(applied(&l, None).id, "default");
    }
}
```

- [ ] **Step 3: Run them to see them fail.**
  Run: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q themes 2>&1 | tail -5`
  Expected: compile errors (`cannot find function list`, `safe_name`, …).

- [ ] **Step 4: Implement** — put this above the test module in `themes.rs`:

```rust
//! Launcher themes: themes/<id>/theme.json plus optional files (psprecomp spec 2026-10-18 part 5b
//! §3.1). Every key is optional; an unreadable theme is skipped, `default` always exists, and a
//! theme whose `unlock` achievement is not earned is listed but locked.
use crate::achievements::Snapshot;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Serialize, Debug, Clone, PartialEq, Default)]
pub struct Sounds {
    #[serde(rename = "move")] pub mv: Option<String>,
    pub confirm: Option<String>,
    pub back: Option<String>,
}

/// A theme as the UI uses it; files are absolute paths (the UI turns them into asset URLs).
#[derive(Serialize, Debug, Clone, PartialEq)]
pub struct ThemeInfo {
    pub id: String, pub name: String, pub author: String,
    pub colors: BTreeMap<String, String>, pub font: Option<String>,
    pub background: Option<String>, pub background_video: Option<String>,
    pub logo: Option<String>, pub preview: Option<String>, pub css: Option<String>,
    pub music: Option<String>, pub music_volume: u32, pub sounds: Sounds,
    pub unlock: Option<String>, pub locked: bool,
}

#[derive(Deserialize, Default)]
struct RawSounds {
    #[serde(rename = "move", default)] mv: Option<String>,
    #[serde(default)] confirm: Option<String>,
    #[serde(default)] back: Option<String>,
}

#[derive(Deserialize, Default)]
struct Raw {
    #[serde(default)] name: Option<String>,
    #[serde(default)] author: Option<String>,
    #[serde(default)] colors: BTreeMap<String, String>,
    #[serde(default)] font: Option<String>,
    #[serde(default)] background_video: Option<String>,
    #[serde(default)] music: Option<String>,
    #[serde(default)] music_volume: Option<u32>,
    #[serde(default)] sounds: RawSounds,
    #[serde(default)] unlock: Option<String>,
}

pub const DEFAULT: &str = "default";

pub fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '-')
}

/// A file name a theme may use: relative, '/'-separated, no '.', '..', '\\' or ':'.
pub fn safe_name(name: &str) -> bool {
    !name.is_empty() && !name.starts_with('/') && !name.contains('\\') && !name.contains(':')
        && name.split('/').all(|c| !c.is_empty() && c != "." && c != "..")
}

/// The theme's file `name` when it is safe and exists; else None (one log line when a name was given).
fn file(dir: &Path, id: &str, name: Option<&str>) -> Option<String> {
    let n = name?;
    if !safe_name(n) { eprintln!("[THEME] {id}: unsafe file name {n:?} ignored"); return None; }
    let p = dir.join(n);
    if !p.is_file() { eprintln!("[THEME] {id}: {n} not found"); return None; }
    Some(p.to_string_lossy().into_owned())
}

/// Fixed-name files (background.png, logo.png, preview.png): optional, never logged.
fn fixed(dir: &Path, name: &str) -> Option<String> {
    let p = dir.join(name);
    p.is_file().then(|| p.to_string_lossy().into_owned())
}

pub fn load(dir: &Path, id: &str) -> Result<ThemeInfo, String> {
    let text = std::fs::read_to_string(dir.join("theme.json")).map_err(|e| e.to_string())?;
    let raw: Raw = serde_json::from_str(&text).map_err(|e| e.to_string())?;
    Ok(ThemeInfo {
        id: id.to_string(),
        name: raw.name.filter(|s| !s.is_empty()).unwrap_or_else(|| id.to_string()),
        author: raw.author.unwrap_or_default(),
        colors: raw.colors,
        font: file(dir, id, raw.font.as_deref()),
        background: fixed(dir, "background.png"),
        background_video: file(dir, id, raw.background_video.as_deref()),
        logo: fixed(dir, "logo.png"),
        preview: fixed(dir, "preview.png"),
        css: std::fs::read_to_string(dir.join("theme.css")).ok(),
        music: file(dir, id, raw.music.as_deref()),
        music_volume: raw.music_volume.unwrap_or(100).min(100),
        sounds: Sounds {
            mv: file(dir, id, raw.sounds.mv.as_deref()),
            confirm: file(dir, id, raw.sounds.confirm.as_deref()),
            back: file(dir, id, raw.sounds.back.as_deref()),
        },
        unlock: if id == DEFAULT { None } else { raw.unlock.filter(|s| !s.is_empty()) },
        locked: false,
    })
}

fn builtin_default() -> ThemeInfo {
    ThemeInfo { id: DEFAULT.into(), name: "Patapon".into(), author: String::new(), colors: BTreeMap::new(), font: None,
        background: None, background_video: None, logo: None, preview: None, css: None, music: None,
        music_volume: 100, sounds: Sounds::default(), unlock: None, locked: false }
}

/// Every theme under <root>/themes: `default` first, then by id; broken ones skipped (one log line).
pub fn list(root: &Path, ach: &Snapshot) -> Vec<ThemeInfo> {
    let dir = root.join("themes");
    let mut ids: Vec<String> = std::fs::read_dir(&dir)
        .map(|rd| rd.flatten().filter(|e| e.path().is_dir())
            .filter_map(|e| e.file_name().to_str().map(str::to_string)).collect())
        .unwrap_or_default();
    ids.sort();
    let mut out = Vec::new();
    for id in ids {
        if !valid_id(&id) { eprintln!("[THEME] {id}: folder name is not [a-z0-9_-]; skipped"); continue; }
        match load(&dir.join(&id), &id) {
            Ok(mut t) => {
                if let Some(u) = &t.unlock {
                    if !ach.knows(u) { eprintln!("[THEME] {id}: unlock {u:?} is not an official achievement; locked"); }
                    t.locked = !ach.is_earned(u);
                }
                out.push(t);
            }
            Err(e) => eprintln!("[THEME] {id}: theme.json: {e}; skipped"),
        }
    }
    match out.iter().position(|t| t.id == DEFAULT) {
        Some(i) => { let d = out.remove(i); out.insert(0, d); }
        None => out.insert(0, builtin_default()),
    }
    out
}

/// The theme to show: `[launcher] theme` when it exists and is unlocked, else `default` (list[0]).
pub fn applied<'a>(list: &'a [ThemeInfo], chosen: Option<&str>) -> &'a ThemeInfo {
    chosen.and_then(|c| list.iter().find(|t| t.id == c && !t.locked)).unwrap_or(&list[0])
}
```

- [ ] **Step 5: Run the themes tests.** Run: `cargo test -q themes 2>&1 | grep -E "^test result|FAILED|panicked"`.
  Expected: `6 passed`.

- [ ] **Step 6: Commands in `lib.rs` — write the failing test first.** Add to `lib.rs`'s `mod tests`:

```rust
    fn put_theme(root: &std::path::Path, id: &str, json: &str) {
        std::fs::create_dir_all(root.join("themes").join(id)).unwrap();
        std::fs::write(root.join("themes").join(id).join("theme.json"), json).unwrap();
    }
    #[test] fn themes_follow_achievements_and_settings() {
        let d = tempfile::tempdir().unwrap();
        let root = d.path();
        put(root, "dxd-1.1.4", true);
        std::fs::write(releases::release_dir(root, "dxd-1.1.4").join("achievements.json"),
            r#"{"version":1,"achievements":[{"id":"story_prologue","name":{"es":"a","en":"a"},"desc":{"es":"a","en":"a"}}]}"#).unwrap();
        put_theme(root, "noche", "{}");
        put_theme(root, "arena", r#"{"unlock":"story_prologue"}"#);
        let p = themes_payload(root);
        assert_eq!(p.applied, "default");
        assert_eq!(p.themes.iter().map(|t| (t.id.as_str(), t.locked)).collect::<Vec<_>>(),
                   [("default", false), ("arena", true), ("noche", false)]);
        assert!(choose_theme(root, "arena").unwrap_err().contains("locked"));
        assert!(choose_theme(root, "gone").is_err());
        assert_eq!(settings::launcher_get(root, "theme"), None, "refusals write nothing");
        choose_theme(root, "noche").unwrap();
        assert_eq!(themes_payload(root).applied, "noche");
        settings::launcher_set(root, "theme", "arena").unwrap();  // chosen while it was earned, file lost since
        assert_eq!(themes_payload(root).applied, "default", "a locked choice shows default");
        assert_eq!(settings::launcher_get(root, "theme").as_deref(), Some("arena"), "and the setting is left alone");
        std::fs::create_dir_all(root.join("achievements")).unwrap();
        std::fs::write(root.join("achievements/unlocked.json"),
            r#"{"version":1,"earned":{"story_prologue":{"date":"2026-10-18T12:00:00Z","hero":"Edson"}}}"#).unwrap();
        let p = themes_payload(root);
        assert_eq!(p.applied, "arena");
        assert!(p.themes.iter().all(|t| !t.locked));
    }
```

  Run: `cargo test -q themes_follow 2>&1 | tail -3` → compile error (`themes_payload` not found).

- [ ] **Step 7: Implement the commands in `lib.rs`** (after `get_achievements`), register `list_themes, set_theme`
  in `generate_handler!` and remove `assets::get_theme` from it:

```rust
#[derive(Serialize)]
struct ThemesPayload { themes: Vec<themes::ThemeInfo>, applied: String }

/// Every theme with its lock (from the current release's catalogue and unlocked.json) and the
/// one to show.
fn themes_payload(root: &std::path::Path) -> ThemesPayload {
    let current = versions_payload(root).current;
    let list = themes::list(root, &achievements::snapshot(root, current.as_deref()));
    let applied = themes::applied(&list, settings::launcher_get(root, "theme").as_deref()).id.clone();
    ThemesPayload { themes: list, applied }
}

fn choose_theme(root: &std::path::Path, id: &str) -> Result<(), String> {
    match themes_payload(root).themes.iter().find(|t| t.id == id) {
        None => Err(format!("no theme {id}")),
        Some(t) if t.locked => Err(format!("theme {id} is locked")),
        Some(_) => settings::launcher_set(root, "theme", id),
    }
}

#[tauri::command]
fn list_themes() -> ThemesPayload { themes_payload(&settings::root_dir()) }

#[tauri::command]
fn set_theme(id: String) -> Result<(), String> { choose_theme(&settings::root_dir(), &id) }
```

  In `assets.rs`: delete the `Theme` struct and `get_theme`, delete `use serde::Serialize;`, and change the module
  doc line to `//! lang/<code>.json from the portable root, as data for the web UI (themes: themes.rs).`

- [ ] **Step 8: Asset scope = the themes folder only.** In `tauri.conf.json` change
  `"assetProtocol": { "enable": true, "scope": ["**"] }` to `"assetProtocol": { "enable": true, "scope": [] }`.
  In `lib.rs` `setup`, right after `let root = settings::root_dir();`, add:

```rust
            // Theme files are the only local files the web UI loads through the asset protocol.
            app.asset_protocol_scope().allow_directory(root.join("themes"), true).ok();
```

- [ ] **Step 9: Music volume option — failing test first.** Add to `settings.rs` `mod tests`:

```rust
    #[test] fn music_volume_is_a_launcher_option() {
        let d = root();
        assert_eq!(load(d.path()).unwrap()["launcher.music_volume"], "70");
        let mut ok = BTreeMap::new();
        ok.insert("launcher.music_volume".to_string(), "30".to_string());
        save(d.path(), &ok).unwrap();
        assert_eq!(launcher_get(d.path(), "music_volume").as_deref(), Some("30"));
        let mut bad = BTreeMap::new();
        bad.insert("launcher.music_volume".to_string(), "35".to_string());
        assert!(save(d.path(), &bad).is_err(), "steps of 10 only");
        set_last_version(d.path(), "dxd-1.1.4").unwrap();
        assert_eq!(load(d.path()).unwrap()["launcher.music_volume"], "30", "launcher keys live together");
    }
```

  Run: `cargo test -q music_volume 2>&1 | tail -3` → FAIL (no key `launcher.music_volume`).

- [ ] **Step 10: Add the option** to `src-tauri/core_options.json` as the second element of `options` (right after
  the `general.language` object, keep the file's 2-space formatting):

```json
    {
      "section": "launcher",
      "key": "music_volume",
      "type": "enum",
      "values": ["0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"],
      "default": "70",
      "tab": "general",
      "label": { "es": "Volumen de la música", "en": "Music volume" },
      "description": { "es": "Música de los temas del launcher (0 = sin música).", "en": "Launcher theme music (0 = no music)." }
    },
```

- [ ] **Step 11: Contract.** In `psprecomp/docs/contracts/settings-v1.md`, right after the paragraph
  "`[launcher]` (e.g. `version = dxd-1.1.5b`, the last played release) belongs to the launcher; the game ignores the
  section without logging." add:

```markdown
Launcher keys: `version` (last played release), `save_<release>` (the chosen save), `ppsspp_savedata` (the PPSSPP
folder), `theme` (the applied launcher theme id, default `default`; a missing or locked one shows `default` and the
key is left alone) and `music_volume` (`0`, `10`, …, `100`, default `70`; the only `[launcher]` key in
`core_options.json`, so Options › General shows it). Themes and achievements: `docs/contracts/achievements-v1.md`.
```

- [ ] **Step 12: Run every Rust gate.** Expected: 93 + 6 + 1 + 1 = 101 passed, 2 ignored; `0` warnings. The UI is
  untouched in this task (`api.ts` still has `getTheme`, now a command that no longer exists; Task 3 replaces it), so
  Vitest and tsc stay as they were.

- [ ] **Step 13: Commit (two repos).**

```bash
cd /c/Users/torso/Documents/deco/launcher
git add src-tauri/src/themes.rs src-tauri/src/achievements.rs src-tauri/src/assets.rs src-tauri/src/lib.rs src-tauri/src/settings.rs src-tauri/core_options.json src-tauri/tauri.conf.json
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "feat(launcher): themes with music, sounds, video and achievement locks" -m "Verified: cargo test <N> passed, 0 warnings" -m "Agent: <builder>"
cd /c/Users/torso/Documents/deco/psprecomp
git add docs/contracts/settings-v1.md
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "docs(contract): launcher theme and music_volume keys" -m "Verified: launcher settings tests" -m "Agent: <builder>"
```

---

### Task 3: UI plumbing — shown theme, backdrop, music and sounds, menu entries

**Files:**
- Create: `src/audio.ts`, `src/audio.test.ts`, `src/theme.test.ts`, `src/test-fixtures.ts`, `src/components/Backdrop.tsx`
- Modify: `src/types.ts`, `src/api.ts`, `src/theme.ts`, `src/App.tsx`, `src/screens/MainMenu.tsx`,
  `src/screens/MainMenu.test.tsx`, `src/screens/Start.test.tsx`, `src/setupTests.ts`, `src/styles.css`,
  `lang/es.json`, `lang/en.json`, `src-tauri/tauri.conf.json` (all under `DECO/launcher`)

**Interfaces:**
- Consumes: commands `list_themes`, `set_theme`, `get_achievements` (Tasks 1–2).
- Produces: types `ThemeSounds`, `ThemeInfo`, `ThemesPayload`, `Achievement`, `Earned`, `AchievementsSnapshot`;
  `api.listThemes(): Promise<ThemesPayload>`, `api.setTheme(id): Promise<void>`,
  `api.getAchievements(): Promise<AchievementsSnapshot>`; `withUrls(t, conv): ThemeInfo`, `applyTheme(t: ThemeInfo)`;
  `effectiveVolume(themeVolume: number, launcherVolume: string | undefined): number`, `class ThemeAudio {
  setMusic(src: string | null, volume: number, paused: boolean); setSounds(s: ThemeSounds); sound(kind: SoundKind) }`
  (music element in the DOM as `<audio id="theme-music" hidden>`); `<Backdrop video image paused />`;
  test helpers `theme(over)`, `achievement(id, over)` in `src/test-fixtures.ts`; `MainMenu` props
  `onAchievements?`, `onThemes?` (and no `background` prop); App state `themes`, `applied`, `shown`, `achievements`,
  `loadThemes()`.

- [ ] **Step 1: Types and API.** Append to `src/types.ts`:

```ts
// A launcher theme (themes/<id>/theme.json). File fields are absolute paths from Rust and asset URLs
// once withUrls() ran.
export type ThemeSounds = { move: string | null; confirm: string | null; back: string | null };
export type ThemeInfo = { id: string; name: string; author: string; colors: Record<string, string>; font: string | null;
  background: string | null; background_video: string | null; logo: string | null; preview: string | null;
  css: string | null; music: string | null; music_volume: number; sounds: ThemeSounds; unlock: string | null;
  locked: boolean };
export type ThemesPayload = { themes: ThemeInfo[]; applied: string };
// The official catalogue (game/<id>/achievements.json) and what this PC earned (achievements/unlocked.json).
export type Achievement = { id: string; icon: string; hidden: boolean; name: Localized; desc: Localized };
export type Earned = { date: string; save: string; hero: string; release: string };
export type AchievementsSnapshot = { catalogue: Achievement[]; earned: Record<string, Earned> };
```

  In `src/api.ts`: add `AchievementsSnapshot, ThemesPayload` to the type import, delete the `getTheme` line and add
  after `getLang`:

```ts
  listThemes: () => invoke<ThemesPayload>("list_themes"),
  setTheme: (id: string) => invoke<void>("set_theme", { id }),
  getAchievements: () => invoke<AchievementsSnapshot>("get_achievements"),
```

- [ ] **Step 2: Test helpers and jsdom media stubs.** Create `src/test-fixtures.ts`:

```ts
// Shared test data for the theme and achievement screens (imported by *.test.ts(x) only).
import type { Achievement, ThemeInfo } from "./types";

export function theme(over: Partial<ThemeInfo> = {}): ThemeInfo {
  return { id: "default", name: "Patapon", author: "", colors: {}, font: null, background: null, background_video: null,
    logo: null, preview: null, css: null, music: null, music_volume: 100, sounds: { move: null, confirm: null, back: null },
    unlock: null, locked: false, ...over };
}

export function achievement(id: string, over: Partial<Achievement> = {}): Achievement {
  return { id, icon: "🏆", hidden: false, name: { es: `Logro ${id}`, en: `Achievement ${id}` },
    desc: { es: `Desc ${id}`, en: `Desc en ${id}` }, ...over };
}
```

  Replace `src/setupTests.ts` with:

```ts
import "@testing-library/jest-dom/vitest";

// jsdom has no media playback: play() resolves and pause() does nothing, like a silent player.
Object.defineProperty(HTMLMediaElement.prototype, "play", { configurable: true, writable: true, value: function play() { return Promise.resolve(); } });
Object.defineProperty(HTMLMediaElement.prototype, "pause", { configurable: true, writable: true, value: function pause() {} });
```

- [ ] **Step 3: Failing tests for `theme.ts` and `audio.ts`.** Create `src/theme.test.ts`:

```ts
import { applyTheme, withUrls } from "./theme";
import { theme } from "./test-fixtures";

test("withUrls converts every file and keeps the rest", () => {
  const t = withUrls(theme({ id: "noche", music: "C:/p/music.ogg", sounds: { move: "C:/p/m.ogg", confirm: null, back: null },
    background_video: "C:/p/bg.webm", css: ".a{}" }), (p) => `asset://${p}`);
  expect(t.music).toBe("asset://C:/p/music.ogg");
  expect(t.sounds).toEqual({ move: "asset://C:/p/m.ogg", confirm: null, back: null });
  expect(t.background_video).toBe("asset://C:/p/bg.webm");
  expect(t.logo).toBeNull();
  expect(t.css).toBe(".a{}");
});

test("a new theme replaces the colors, font and css of the previous one", () => {
  const root = document.documentElement;
  applyTheme(theme({ colors: { bg: "#111111", accent: "#222222" }, font: "asset://f.ttf", css: ".x{}" }));
  expect(root.style.getPropertyValue("--c-accent")).toBe("#222222");
  expect(root.style.getPropertyValue("--font")).toContain("ThemeFont");
  applyTheme(theme({ colors: { bg: "#333333" } }));
  expect(root.style.getPropertyValue("--c-bg")).toBe("#333333");
  expect(root.style.getPropertyValue("--c-accent")).toBe("");
  expect(root.style.getPropertyValue("--font")).toBe("");
  expect(document.getElementById("theme-css")!.textContent).toBe("");
});
```

  Create `src/audio.test.ts`:

```ts
import { effectiveVolume, ThemeAudio } from "./audio";

type Fake = { src: string; loop: boolean; volume: number; paused: boolean; currentTime: number;
  play: ReturnType<typeof vi.fn>; pause: ReturnType<typeof vi.fn> };
function maker(log: Fake[], flags: { reject: boolean }) {
  return (src: string) => {
    const el: Fake = { src, loop: false, volume: 1, paused: true, currentTime: 3, play: vi.fn(), pause: vi.fn() };
    el.play.mockImplementation(() => {
      if (flags.reject) return Promise.reject(new Error("NotAllowedError"));
      el.paused = false;
      return Promise.resolve();
    });
    el.pause.mockImplementation(() => { el.paused = true; });
    log.push(el);
    return el as unknown as HTMLAudioElement;
  };
}

test("effective volume multiplies both and defaults to 70", () => {
  expect(effectiveVolume(80, "50")).toBeCloseTo(0.4);
  expect(effectiveVolume(100, undefined)).toBeCloseTo(0.7);
  expect(effectiveVolume(100, "abc")).toBeCloseTo(0.7);
  expect(effectiveVolume(250, "100")).toBe(1);
  expect(effectiveVolume(100, "0")).toBe(0);
});

test("music loops, follows the theme and pauses while the game runs", () => {
  const els: Fake[] = [];
  const a = new ThemeAudio(maker(els, { reject: false }));
  a.setMusic("m1.ogg", 0.5, false);
  expect(els).toHaveLength(1);
  expect(els[0]).toMatchObject({ src: "m1.ogg", loop: true, volume: 0.5, paused: false });
  a.setMusic("m1.ogg", 0.3, false);
  expect(els).toHaveLength(1);
  expect(els[0].volume).toBe(0.3);
  expect(els[0].play).toHaveBeenCalledTimes(1);
  a.setMusic("m1.ogg", 0.3, true);
  expect(els[0].paused).toBe(true);
  a.setMusic("m1.ogg", 0.3, false);
  expect(els[0].play).toHaveBeenCalledTimes(2);
  a.setMusic("m2.ogg", 0.3, false);
  expect(els[0].paused).toBe(true);
  expect(els[1]).toMatchObject({ src: "m2.ogg", paused: false });
  a.setMusic(null, 0.3, false);
  expect(els[1].paused).toBe(true);
});

test("volume 0 never plays", () => {
  const els: Fake[] = [];
  new ThemeAudio(maker(els, { reject: false })).setMusic("m.ogg", 0, false);
  expect(els[0].play).not.toHaveBeenCalled();
});

test("a blocked autoplay is retried on the next key", async () => {
  const flags = { reject: true };
  const els: Fake[] = [];
  const a = new ThemeAudio(maker(els, flags));
  a.setMusic("m.ogg", 0.5, false);
  await new Promise((r) => setTimeout(r, 0));
  expect(els[0].paused).toBe(true);
  flags.reject = false;
  window.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown" }));
  expect(els[0].paused).toBe(false);
  expect(els[0].play).toHaveBeenCalledTimes(2);
});

test("sounds restart from the beginning and missing ones are silent", () => {
  const els: Fake[] = [];
  const a = new ThemeAudio(maker(els, { reject: false }));
  a.setSounds({ move: "move.ogg", confirm: null, back: "back.ogg" });
  expect(els.map((e) => e.src)).toEqual(["move.ogg", "back.ogg"]);
  a.sound("move");
  expect(els[0].currentTime).toBe(0);
  expect(els[0].play).toHaveBeenCalledOnce();
  a.sound("confirm");
  expect(els[1].play).not.toHaveBeenCalled();
});
```

  Run: `npx vitest run src/theme.test.ts src/audio.test.ts 2>&1 | tail -5` → FAIL (`withUrls` / `./audio` missing).

- [ ] **Step 4: Implement `src/theme.ts`** (replace the whole file):

```ts
// Applies a launcher theme to the document: colors become CSS variables --c-<name> (the ones the
// previous theme set are removed first), an optional font becomes the ThemeFont face, theme.css is
// appended. withUrls() turns the theme's file paths into URLs the web view can load.
import type { ThemeInfo } from "./types";

let setColors: string[] = [];

function styleTag(id: string): HTMLStyleElement {
  let el = document.getElementById(id) as HTMLStyleElement | null;
  if (!el) {
    el = document.createElement("style");
    el.id = id;
    document.head.appendChild(el);
  }
  return el;
}

export function withUrls(t: ThemeInfo, conv: (p: string) => string): ThemeInfo {
  const u = (p: string | null) => (p ? conv(p) : null);
  return { ...t, font: u(t.font), background: u(t.background), background_video: u(t.background_video), logo: u(t.logo),
    preview: u(t.preview), music: u(t.music), sounds: { move: u(t.sounds.move), confirm: u(t.sounds.confirm), back: u(t.sounds.back) } };
}

export function applyTheme(theme: ThemeInfo): void {
  const root = document.documentElement;
  for (const name of setColors) root.style.removeProperty(name);
  setColors = Object.keys(theme.colors).map((n) => `--c-${n}`);
  for (const [name, value] of Object.entries(theme.colors)) root.style.setProperty(`--c-${name}`, value);
  if (theme.font) {
    styleTag("theme-font").textContent = `@font-face { font-family: ThemeFont; src: url("${theme.font}"); }`;
    root.style.setProperty("--font", "ThemeFont, system-ui, sans-serif");
  } else {
    styleTag("theme-font").textContent = "";
    root.style.removeProperty("--font");
  }
  styleTag("theme-css").textContent = theme.css ?? "";
}
```

- [ ] **Step 5: Implement `src/audio.ts`:**

```ts
// Theme music and UI sounds. The music loops while the launcher is shown and pauses while the game
// runs; an autoplay the web view blocks is retried on the next key or click.
import type { ThemeSounds } from "./types";

export type SoundKind = keyof ThemeSounds;

// theme music_volume (0..100) × settings [launcher] music_volume (0..100, default 70), as 0..1.
export function effectiveVolume(themeVolume: number, launcherVolume: string | undefined): number {
  const n = Number.parseInt(launcherVolume ?? "", 10);
  const l = Number.isNaN(n) ? 70 : Math.min(100, Math.max(0, n));
  const t = Math.min(100, Math.max(0, themeVolume));
  return (t / 100) * (l / 100);
}

export class ThemeAudio {
  private music: HTMLAudioElement | null = null;
  private musicSrc: string | null = null;
  private paused = true;
  private sounds = new Map<SoundKind, HTMLAudioElement>();
  private retryArmed = false;

  constructor(private make: (src: string) => HTMLAudioElement = (src) => new Audio(src)) {}

  setMusic(src: string | null, volume: number, paused: boolean): void {
    this.paused = paused || volume <= 0;
    if (src !== this.musicSrc) {
      this.dropMusic();
      this.musicSrc = src;
      if (src) {
        const el = this.make(src);
        el.loop = true;
        // In the DOM (hidden) so the end-to-end checks can read its state through DevTools.
        if (el instanceof HTMLMediaElement) { el.id = "theme-music"; el.hidden = true; document.body.appendChild(el); }
        this.music = el;
      }
    }
    if (!this.music) return;
    this.music.volume = volume;
    if (this.paused) this.music.pause();
    else this.start(this.music);
  }

  setSounds(s: ThemeSounds): void {
    this.sounds = new Map();
    for (const k of ["move", "confirm", "back"] as const) {
      const src = s[k];
      if (src) this.sounds.set(k, this.make(src));
    }
  }

  sound(kind: SoundKind): void {
    const el = this.sounds.get(kind);
    if (!el) return;
    el.currentTime = 0;
    Promise.resolve(el.play()).catch(() => {});
  }

  private dropMusic(): void {
    if (!this.music) return;
    this.music.pause();
    if (this.music instanceof HTMLMediaElement) this.music.remove();
    this.music = null;
  }

  private start(el: HTMLAudioElement): void {
    if (!el.paused) return;
    Promise.resolve(el.play()).catch(() => this.armRetry());
  }

  private armRetry(): void {
    if (this.retryArmed) return;
    this.retryArmed = true;
    const again = () => {
      window.removeEventListener("keydown", again, true);
      window.removeEventListener("pointerdown", again, true);
      this.retryArmed = false;
      if (this.music && !this.paused) this.start(this.music);
    };
    window.addEventListener("keydown", again, true);
    window.addEventListener("pointerdown", again, true);
  }
}
```

- [ ] **Step 6: Run the two test files.** Expected: 7 passed.

- [ ] **Step 7: Backdrop.** Create `src/components/Backdrop.tsx`:

```tsx
import { useEffect, useRef } from "react";

// The shown theme's background behind every screen: a muted looping video, else an image. The video
// pauses while the game runs so it costs nothing then.
export function Backdrop({ video, image, paused }: { video: string | null; image: string | null; paused: boolean }) {
  const ref = useRef<HTMLVideoElement>(null);
  useEffect(() => {
    const v = ref.current;
    if (!v) return;
    if (paused) v.pause();
    else Promise.resolve(v.play()).catch(() => {});
  }, [paused, video]);
  if (video) return <video ref={ref} className="backdrop" data-testid="backdrop" src={video} autoPlay={!paused} loop muted playsInline />;
  if (image) return <div className="backdrop" data-testid="backdrop" style={{ backgroundImage: `url(${image})` }} />;
  return null;
}
```

- [ ] **Step 8: Main menu entries — failing test first.** In `src/screens/MainMenu.test.tsx` remove every
  ` background=""` (the prop goes away; `sed -i 's/ background=""//g' src/screens/MainMenu.test.tsx`) and add:

```tsx
test("Logros and Temas sit between Partidas and Opciones", async () => {
  const onAchievements = vi.fn(), onThemes = vi.fn();
  render(<MainMenu strings={{ ...strings, "menu.saves": "Partidas", "menu.achievements": "Logros", "menu.themes": "Temas" }} logo=""
    onStart={vi.fn()} onOptions={vi.fn()} onExit={vi.fn()} busy={false} onSaves={vi.fn()}
    onAchievements={onAchievements} onThemes={onThemes} />);
  expect(screen.getAllByRole("option").map((o) => o.textContent)).toEqual(["Iniciar", "Partidas", "Logros", "Temas", "Opciones", "Salir"]);
  await userEvent.keyboard("{ArrowDown}{ArrowDown}{Enter}");
  expect(onAchievements).toHaveBeenCalledOnce();
  await userEvent.keyboard("{ArrowDown}{Enter}");
  expect(onThemes).toHaveBeenCalledOnce();
});
```

  Run: `npx vitest run src/screens/MainMenu.test.tsx 2>&1 | tail -5` → the new test FAILS.

- [ ] **Step 9: Implement `MainMenu.tsx`.** Change the `Props` type and the entries and root `div` (rest unchanged):

```tsx
type Props = { strings: Record<string, string>; logo: string; busy: boolean;
  version?: { label: string; onStep(dir: 1 | -1): void }; onSaves?: () => void; saveLine?: string;
  onAchievements?: () => void; onThemes?: () => void;
  onStart(): void; onOptions(): void; onExit(): void };

export function MainMenu(p: Props) {
  const entries = [
    { id: "start", label: t(p.strings, "menu.start"), run: () => { if (!p.busy) p.onStart(); } },
    ...(p.onSaves ? [{ id: "saves", label: t(p.strings, "menu.saves"), run: p.onSaves }] : []),
    ...(p.onAchievements ? [{ id: "achievements", label: t(p.strings, "menu.achievements"), run: p.onAchievements }] : []),
    ...(p.onThemes ? [{ id: "themes", label: t(p.strings, "menu.themes"), run: p.onThemes }] : []),
    { id: "options", label: t(p.strings, "menu.options"), run: p.onOptions },
    { id: "exit", label: t(p.strings, "menu.exit"), run: p.onExit },
  ];
```

  and the root element becomes `<div className="screen main-menu">` (the backdrop draws the background now).
  Strings — `lang/es.json`: `"menu.achievements": "Logros", "menu.themes": "Temas"`; `lang/en.json`:
  `"menu.achievements": "Achievements", "menu.themes": "Themes"` (add them after `"menu.save_line"`).

- [ ] **Step 10: App test for music while the game runs — failing first.** In `src/screens/Start.test.tsx` replace
  the `getTheme: …` line of the hoisted `api` with:

```ts
  listThemes: vi.fn(async () => ({ applied: "default", themes: [{ id: "default", name: "Patapon", author: "", colors: {},
    font: null, background: null, background_video: null, logo: null, preview: null, css: null, music: "C:/t/music.ogg",
    music_volume: 100, sounds: { move: null, confirm: null, back: null }, unlock: null, locked: false }] })),
  getAchievements: vi.fn(async () => ({ catalogue: [], earned: {} })),
```

  and add at the end:

```tsx
test("theme music pauses while the game runs; themes reload when it returns", async () => {
  const play = vi.spyOn(HTMLMediaElement.prototype, "play");
  const pause = vi.spyOn(HTMLMediaElement.prototype, "pause");
  api.checkInstall.mockResolvedValue({ exe: true, disc0: true, ascii_path: true });
  let finish: (v: unknown) => void = () => {};
  api.startGame.mockImplementation(() => new Promise((r) => { finish = r; }));
  render(<App />);
  await waitFor(() => expect(play).toHaveBeenCalled());
  expect(document.getElementById("theme-music")).toHaveAttribute("src", "C:/t/music.ogg");
  const loads = api.listThemes.mock.calls.length;
  pause.mockClear();
  await userEvent.click(await screen.findByText("Iniciar"));
  await waitFor(() => expect(pause).toHaveBeenCalled());
  play.mockClear();
  finish({ kind: "Menu" });
  await waitFor(() => expect(api.listThemes.mock.calls.length).toBeGreaterThan(loads));
  await waitFor(() => expect(play).toHaveBeenCalled());
  play.mockRestore();
  pause.mockRestore();
});
```

  Run: `npx vitest run src/screens/Start.test.tsx 2>&1 | tail -5` → FAIL (App still calls `getTheme`).

- [ ] **Step 11: Rewire `src/App.tsx`.** Changes, in file order:
  1. Imports: add `import { effectiveVolume, ThemeAudio, type SoundKind } from "./audio";`, change the theme import to
     `import { applyTheme, withUrls } from "./theme";`, add `import { Backdrop } from "./components/Backdrop";` and
     `import { onNav } from "./nav/events";`, and extend the type import to
     `import type { AchievementsSnapshot, Identity, NavAction, OptionDef, TextureStatus, ThemeInfo, Versions } from "./types";`.
  2. Below the `type Screen = …` line add:

```tsx
// Which UI sound a navigation action plays (none for reset).
const SOUND: Partial<Record<NavAction, SoundKind>> = { up: "move", down: "move", left: "move", right: "move",
  tab_prev: "move", tab_next: "move", confirm: "confirm", back: "back" };
```

  3. Delete `const [art, setArt] = useState({ logo: "", background: "" });` and add after `const [saveLine, …]`:

```tsx
  const [themes, setThemes] = useState<ThemeInfo[]>([]);
  const [applied, setApplied] = useState("default");
  const [shown, setShown] = useState<ThemeInfo | null>(null);  // the applied theme, or the one previewed in Temas
  const [achievements, setAchievements] = useState<AchievementsSnapshot>({ catalogue: [], earned: {} });
  const audio = useRef<ThemeAudio | null>(null);
  if (!audio.current) audio.current = new ThemeAudio();
```

  4. Replace the whole first `useEffect` (the one that calls `loadOptions()` and `api.getTheme()`) with:

```tsx
  // Themes and achievements load together: an achievement the game just recorded can unlock a theme.
  const loadThemes = useCallback(async () => {
    try {
      const [p, a] = await Promise.all([api.listThemes(), api.getAchievements()]);
      const list = p.themes.map((th) => withUrls(th, convertFileSrc));
      setThemes(list);
      setApplied(p.applied);
      setShown(list.find((th) => th.id === p.applied) ?? list[0] ?? null);
      setAchievements(a);
    } catch { /* plain colors from styles.css, no achievements */ }
  }, []);

  useEffect(() => {
    void loadOptions();
    void loadThemes();
  }, [loadOptions, loadThemes]);

  useEffect(() => { if (shown) applyTheme(shown); }, [shown]);
```

  5. After the `busyRef` declaration is in scope (it already exists above), add — right after the effect from
     item 4:

```tsx
  // Theme music loops while the launcher is shown and pauses while the game runs (busy); no UI
  // sounds then either (the gamepad keeps sending nav events while the window is hidden).
  const volume = effectiveVolume(shown?.music_volume ?? 100, values["launcher.music_volume"]);
  useEffect(() => { audio.current!.setMusic(shown?.music ?? null, volume, busy); }, [shown, volume, busy]);
  useEffect(() => { audio.current!.setSounds(shown?.sounds ?? { move: null, confirm: null, back: null }); }, [shown]);
  useEffect(() => onNav((a) => {
    const kind = SOUND[a];
    if (kind && !busyRef.current) audio.current!.sound(kind);
  }), []);
  useEffect(() => () => audio.current?.setMusic(null, 0, true), []);
```

     (`busyRef` is declared before `lang` today; keep it there — the effects only read `busyRef.current`.)
  6. In `start()`, right after `const o = await api.startGame(id);` add `void loadThemes();  // the game may have
     earned achievements`.
  7. In the `MainMenu` element replace `logo={art.logo} background={art.background}` with
     `logo={shown?.logo ?? themes[0]?.logo ?? ""}` (a theme without a logo shows the default one).
  8. The returned JSX becomes:

```tsx
  return (
    <main data-testid="app">
      <Backdrop video={shown?.background_video ?? null} image={shown?.background ?? null} paused={busy} />
      {body}
      {warning && screen === "menu" && <p className="warning" role="status">{warning}</p>}
    </main>
  );
```

  (`applied`, `setApplied` and `achievements` are read by Tasks 4–5; `tsconfig.json` has no `noUnusedLocals`, so
  they compile unused until then.)

- [ ] **Step 12: Styles.** In `src/styles.css`, in the `.screen { … }` rule replace
  `background: var(--c-bg) center / cover no-repeat;` with `background: transparent;`, and append:

```css
/* Theme background behind every screen (a muted looping video or an image); screens other than the
   menu, Temas and Logros darken it so their text stays readable. */
.backdrop {
  position: fixed; inset: 0; width: 100%; height: 100%; object-fit: cover; z-index: 0; pointer-events: none;
  background: center / cover no-repeat;
}
main > .screen { z-index: 1; }
.backdrop ~ .screen:not(.main-menu):not(.themes):not(.achievements) { background: rgba(0, 0, 0, 0.55); }
```

- [ ] **Step 13: Autoplay in WebView2.** In `src-tauri/tauri.conf.json`, add to the `main` window object (keep wry's
  default flags, which setting this key replaces):

```json
"additionalBrowserArgs": "--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection --autoplay-policy=no-user-gesture-required"
```

- [ ] **Step 14: Run every UI gate and the Rust gates.** Expected: Vitest 53 + 2 + 5 + 1 + 1 = 62 passed, tsc clean,
  cargo unchanged (101 + 2 ignored, 0 warnings — `tauri.conf.json` is read by `tauri-build`).

- [ ] **Step 15: Commit.**

```bash
cd /c/Users/torso/Documents/deco/launcher
git add src lang src-tauri/tauri.conf.json
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "feat(launcher): theme backdrop, music and UI sounds; Logros and Temas menu entries" -m "Verified: vitest <N> passed, tsc clean" -m "Agent: <builder>"
```

---

### Task 4: Logros (Achievements screen)

**Files:**
- Create: `src/achievements.ts`, `src/achievements.test.ts`, `src/screens/Achievements.tsx`,
  `src/screens/Achievements.test.tsx`
- Modify: `src/App.tsx`, `src/styles.css`, `lang/es.json`, `lang/en.json` (under `DECO/launcher`)

**Interfaces:**
- Consumes: Task 3 types, `theme()` / `achievement()` fixtures, App state `achievements`, `themes`, `loadThemes`;
  `Tabs`, `PromptBar`, `onNav`.
- Produces: `FILTERS = ["all", "earned", "pending"]`, `counter(s) -> {earned, total, pct}`, `filtered(s, f)`,
  `formatDate(iso, lang)`, `earnedLine(e, lang)`; `<Achievements strings lang snapshot themes focus? onTheme?
  onBack />` where `onTheme(themeId: string, achievementId: string)`; App: Screen values `"achievements"` and
  `"themes"`, state `achFocus`, `achBack: "menu" | "themes"`, `openAchievements(focus?: string, from?: "menu" | "themes")`.

- [ ] **Step 1: Failing tests for the helpers.** Create `src/achievements.test.ts`:

```ts
import { counter, earnedLine, filtered } from "./achievements";
import { achievement } from "./test-fixtures";
import type { AchievementsSnapshot } from "./types";

const s: AchievementsSnapshot = { catalogue: [achievement("a"), achievement("b"), achievement("c")],
  earned: { a: { date: "2026-10-18T12:00:00Z", save: "DATA04", hero: "Edson", release: "dxd-1.1.4" } } };

test("counter and filters", () => {
  expect(counter(s)).toEqual({ earned: 1, total: 3, pct: 33 });
  expect(counter({ catalogue: [], earned: {} })).toEqual({ earned: 0, total: 0, pct: 0 });
  expect(filtered(s, "earned").map((a) => a.id)).toEqual(["a"]);
  expect(filtered(s, "pending").map((a) => a.id)).toEqual(["b", "c"]);
  expect(filtered(s, "all")).toHaveLength(3);
});

test("earned line: the date in the player's language, then the hero", () => {
  expect(earnedLine(s.earned.a, "es")).toBe("18 oct 2026 · Edson");
  expect(earnedLine(s.earned.a, "en")).toBe("18 Oct 2026 · Edson");
  expect(earnedLine({ date: "x", save: "", hero: "Edson", release: "" }, "es")).toBe("Edson");
  expect(earnedLine({ date: "2026-10-18T12:00:00Z", save: "", hero: "", release: "" }, "es")).toBe("18 oct 2026");
});
```

  Run: `npx vitest run src/achievements.test.ts 2>&1 | tail -5` → FAIL (module missing).

- [ ] **Step 2: Implement `src/achievements.ts`:**

```ts
// Pure helpers for the Logros screen: the counter, the filters and the earned line.
import type { Achievement, AchievementsSnapshot, Earned } from "./types";

export const FILTERS = ["all", "earned", "pending"] as const;
export type Filter = (typeof FILTERS)[number];

export function counter(s: AchievementsSnapshot): { earned: number; total: number; pct: number } {
  const total = s.catalogue.length;
  const earned = s.catalogue.filter((a) => s.earned[a.id]).length;
  return { earned, total, pct: total ? Math.round((100 * earned) / total) : 0 };
}

export function filtered(s: AchievementsSnapshot, f: Filter): Achievement[] {
  return s.catalogue.filter((a) => f === "all" || (f === "earned") === !!s.earned[a.id]);
}

export function formatDate(iso: string, lang: "es" | "en"): string {
  const d = new Date(iso);
  if (!iso || Number.isNaN(d.getTime())) return "";
  return new Intl.DateTimeFormat(lang === "en" ? "en-GB" : "es-ES", { day: "numeric", month: "short", year: "numeric" }).format(d);
}

export function earnedLine(e: Earned, lang: "es" | "en"): string {
  return [formatDate(e.date, lang), e.hero].filter(Boolean).join(" · ");
}
```

  Run the file again → 2 passed.

- [ ] **Step 3: Failing screen tests.** Create `src/screens/Achievements.test.tsx`:

```tsx
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Achievements } from "./Achievements";
import { achievement, theme } from "../test-fixtures";
import type { AchievementsSnapshot } from "../types";

vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
const strings: Record<string, string> = { "achievements.title": "LOGROS", "achievements.counter": "{earned} / {total} · {pct} %",
  "achievements.filter.all": "Todos", "achievements.filter.earned": "Conseguidos", "achievements.filter.pending": "Pendientes",
  "achievements.hidden": "Logro oculto.", "achievements.opens_theme": "🎨 Abre el tema {name}",
  "achievements.none": "Esta versión no trae logros.", "achievements.empty": "Nada por aquí todavía." };
const snap: AchievementsSnapshot = {
  catalogue: [achievement("story_prologue"), achievement("missions_1"), achievement("secret_x", { hidden: true }),
    achievement("secret_y", { hidden: true })],
  earned: { story_prologue: { date: "2026-10-18T12:00:00Z", save: "DATA04", hero: "Edson", release: "dxd-1.1.4" },
            secret_y: { date: "2026-10-18T12:00:00Z", save: "DATA04", hero: "", release: "dxd-1.1.4" } },
};
const themes = [theme(), theme({ id: "arena", name: "Arena", unlock: "story_prologue" }),
  theme({ id: "sombra", name: "Sombra", unlock: "secret_x", locked: true })];
const names = () => screen.getAllByRole("option").map((o) => o.querySelector("b")!.textContent);

test("counter, earned line, pending and hidden entries", () => {
  render(<Achievements strings={strings} lang="es" snapshot={snap} themes={themes} onBack={vi.fn()} />);
  expect(screen.getByTestId("achievements-counter")).toHaveTextContent("2 / 4 · 50 %");
  expect(names()).toEqual(["Logro story_prologue", "Logro missions_1", "???", "Logro secret_y"]);
  expect(screen.getByText("18 oct 2026 · Edson")).toBeInTheDocument();
  expect(screen.getAllByRole("option")[1]).toHaveClass("pending");
  expect(screen.getByText("Logro oculto.")).toBeInTheDocument();
  expect(screen.getByText("🎨 Abre el tema Arena")).toBeInTheDocument();
  expect(screen.queryByText("🎨 Abre el tema Sombra")).toBeNull();  // a hidden achievement does not reveal its theme
});

test("Q and E switch Todos / Conseguidos / Pendientes", async () => {
  render(<Achievements strings={strings} lang="es" snapshot={snap} themes={themes} onBack={vi.fn()} />);
  await userEvent.keyboard("e");
  expect(names()).toEqual(["Logro story_prologue", "Logro secret_y"]);
  await userEvent.keyboard("e");
  expect(names()).toEqual(["Logro missions_1", "???"]);
  await userEvent.keyboard("qq");
  expect(names()).toHaveLength(4);
});

test("focus selects an entry; Enter on one that unlocks a theme opens it", async () => {
  const onTheme = vi.fn();
  render(<Achievements strings={strings} lang="es" snapshot={snap} themes={themes} focus="missions_1" onTheme={onTheme} onBack={vi.fn()} />);
  expect(screen.getAllByRole("option")[1]).toHaveAttribute("aria-selected", "true");
  await userEvent.keyboard("{Enter}");
  expect(onTheme).not.toHaveBeenCalled();
  await userEvent.keyboard("{ArrowUp}{Enter}");
  expect(onTheme).toHaveBeenCalledWith("arena", "story_prologue");
});

test("no catalogue and an empty filter say so; Esc goes back", async () => {
  const onBack = vi.fn();
  const { unmount } = render(<Achievements strings={strings} lang="es" snapshot={{ catalogue: [], earned: {} }} themes={[]} onBack={onBack} />);
  expect(screen.getByText("Esta versión no trae logros.")).toBeInTheDocument();
  expect(screen.getByTestId("achievements-counter")).toHaveTextContent("0 / 0 · 0 %");
  await userEvent.keyboard("{Escape}");
  expect(onBack).toHaveBeenCalledOnce();
  unmount();
  render(<Achievements strings={strings} lang="es" snapshot={{ catalogue: [achievement("a")], earned: {} }} themes={[]} onBack={vi.fn()} />);
  await userEvent.keyboard("e");
  expect(screen.getByText("Nada por aquí todavía.")).toBeInTheDocument();
});
```

  Run: `npx vitest run src/screens/Achievements.test.tsx 2>&1 | tail -5` → FAIL (module missing).

- [ ] **Step 4: Implement `src/screens/Achievements.tsx`:**

```tsx
import { useEffect, useRef, useState } from "react";
import { counter, earnedLine, FILTERS, filtered } from "../achievements";
import { PromptBar } from "../components/PromptBar";
import { Tabs } from "../components/Tabs";
import { t } from "../i18n";
import { onNav } from "../nav/events";
import type { AchievementsSnapshot, Device, NavAction, ThemeInfo } from "../types";

type Props = {
  strings: Record<string, string>; lang: "es" | "en"; snapshot: AchievementsSnapshot; themes: ThemeInfo[];
  focus?: string;                                            // achievement to select on open (from a locked theme)
  onTheme?: (themeId: string, achievementId: string) => void;  // Enter on an achievement that unlocks a theme
  onBack(): void;
};

// Logros: every official achievement with what this PC earned (date and hero), Q/E filters Todos /
// Conseguidos / Pendientes, hidden ones as ??? until earned, and a link to the theme each unlocks.
export function Achievements(p: Props) {
  const [filter, setFilter] = useState(0);
  const list = filtered(p.snapshot, FILTERS[filter]);
  const [index, setIndex] = useState(() => Math.max(0, list.findIndex((a) => a.id === p.focus)));
  const [device, setDevice] = useState<Device>("keyboard");
  const sel = Math.min(index, Math.max(0, list.length - 1));
  const selected = useRef<HTMLLIElement>(null);
  useEffect(() => { selected.current?.scrollIntoView?.({ block: "nearest" }); }, [sel, filter]);

  const visible = (id: string, hidden: boolean) => !hidden || !!p.snapshot.earned[id];
  const themeOf = (id: string) => p.themes.find((th) => th.unlock === id);
  const open = (i: number) => {
    const a = list[i];
    const th = a && visible(a.id, a.hidden) ? themeOf(a.id) : undefined;
    if (th && p.onTheme) p.onTheme(th.id, a.id);
  };
  const handle = (a: NavAction) => {
    const n = list.length;
    if (a === "tab_prev" || a === "tab_next") {
      setFilter((filter + (a === "tab_next" ? 1 : FILTERS.length - 1)) % FILTERS.length);
      setIndex(0);
    } else if (a === "up" && n) setIndex((sel - 1 + n) % n);
    else if (a === "down" && n) setIndex((sel + 1) % n);
    else if (a === "confirm") open(sel);
    else if (a === "back") p.onBack();
  };
  const ref = useRef(handle);
  ref.current = handle;
  useEffect(() => onNav((a, d) => { setDevice(d); ref.current(a); }), []);

  const c = counter(p.snapshot);
  return (
    <div className="screen achievements" data-testid="achievements">
      <h1 className="title">{t(p.strings, "achievements.title")}</h1>
      <p className="achievements-counter" data-testid="achievements-counter">
        {t(p.strings, "achievements.counter", { earned: String(c.earned), total: String(c.total), pct: String(c.pct) })}
      </p>
      <Tabs labels={FILTERS.map((f) => t(p.strings, `achievements.filter.${f}`))} index={filter}
        onPick={(i) => { setFilter(i); setIndex(0); }} />
      {p.snapshot.catalogue.length === 0 ? (
        <p className="panel achievements-empty">{t(p.strings, "achievements.none")}</p>
      ) : list.length === 0 ? (
        <p className="panel achievements-empty">{t(p.strings, "achievements.empty")}</p>
      ) : (
        <ul className="panel achievement-list" role="listbox">
          {list.map((a, i) => {
            const e = p.snapshot.earned[a.id];
            const shown = visible(a.id, a.hidden);
            const th = shown ? themeOf(a.id) : undefined;
            return (
              <li key={a.id} ref={i === sel ? selected : undefined} role="option" aria-selected={i === sel}
                  className={`achievement${e ? " earned" : " pending"}${i === sel ? " selected" : ""}`}
                  onMouseEnter={() => setIndex(i)} onClick={() => { setIndex(i); open(i); }}>
                <span className="achievement-icon">{shown ? a.icon : "❔"}</span>
                <span className="achievement-text">
                  <b>{shown ? a.name[p.lang] : "???"}</b>
                  <span>{shown ? a.desc[p.lang] : t(p.strings, "achievements.hidden")}</span>
                  {th && <span className="achievement-theme">{t(p.strings, "achievements.opens_theme", { name: th.name })}</span>}
                </span>
                <span className="achievement-date">{e ? earnedLine(e, p.lang) : ""}</span>
              </li>
            );
          })}
        </ul>
      )}
      <PromptBar device={device} items={[
        { id: "category", label: t(p.strings, "prompt.filter") },
        { id: "select", label: t(p.strings, "prompt.select") },
        { id: "confirm", label: t(p.strings, "prompt.confirm") },
        { id: "back", label: t(p.strings, "prompt.back") },
      ]} />
    </div>
  );
}
```

  Run the screen tests → 4 passed.

- [ ] **Step 5: Route it in `App.tsx`.**
  1. `import { Achievements } from "./screens/Achievements";`
  2. Extend `type Screen` with `| "achievements" | "themes"`.
  3. Right after the sound effects block Task 3 added (the `useEffect(() => () => audio.current?.setMusic(null, 0, true), []);`
     line — so `loadThemes` is declared above it) add:

```tsx
  const [achFocus, setAchFocus] = useState<string | undefined>(undefined);
  const [achBack, setAchBack] = useState<"menu" | "themes">("menu");
  const openAchievements = (focus?: string, from: "menu" | "themes" = "menu") => {
    setAchFocus(focus);
    setAchBack(from);
    setScreen("achievements");
    void loadThemes();
  };
```

  4. Before the final `} else {` of the body chain add:

```tsx
  } else if (screen === "achievements") {
    body = <Achievements strings={strings} lang={lang} snapshot={achievements} themes={themes} focus={achFocus}
      onBack={() => setScreen(achBack)} />;
```

  5. `MainMenu` gets `onAchievements={() => openAchievements()}`.

- [ ] **Step 6: Strings and styles.** `lang/es.json` (after `"menu.themes"`):

```json
  "achievements.title": "LOGROS",
  "achievements.counter": "{earned} / {total} · {pct} %",
  "achievements.filter.all": "Todos",
  "achievements.filter.earned": "Conseguidos",
  "achievements.filter.pending": "Pendientes",
  "achievements.hidden": "Logro oculto.",
  "achievements.opens_theme": "🎨 Abre el tema {name}",
  "achievements.none": "Esta versión no trae logros.",
  "achievements.empty": "Nada por aquí todavía.",
  "prompt.filter": "Filtro",
```

  `lang/en.json`:

```json
  "achievements.title": "ACHIEVEMENTS",
  "achievements.counter": "{earned} / {total} · {pct} %",
  "achievements.filter.all": "All",
  "achievements.filter.earned": "Earned",
  "achievements.filter.pending": "Pending",
  "achievements.hidden": "Hidden achievement.",
  "achievements.opens_theme": "🎨 Unlocks the {name} theme",
  "achievements.none": "This release has no achievements.",
  "achievements.empty": "Nothing here yet.",
  "prompt.filter": "Filter",
```

  Append to `src/styles.css`:

```css
/* Logros */
.achievements { justify-content: flex-start; padding-top: 5vh; gap: 1.5vh; }
.achievements-counter { margin: 0; font-size: clamp(16px, 2.6vh, 30px); color: var(--c-accent); }
.achievement-list { list-style: none; margin: 0; max-height: 58vh; overflow-y: auto; }
.achievement {
  display: flex; align-items: center; gap: 1em; padding: 0.5em 0.8em; border-radius: 0.35em; cursor: pointer;
  font-size: clamp(14px, 2.1vh, 25px);
}
.achievement.pending { opacity: 0.55; }
.achievement.selected { background: var(--c-accent); color: #000; opacity: 1; }
.achievement-icon { font-size: 1.8em; width: 1.6em; text-align: center; flex: none; }
.achievement-text { display: flex; flex-direction: column; gap: 0.15em; flex: 1; min-width: 0; }
.achievement-theme { font-size: 0.85em; }
.achievement-date { flex: none; font-size: 0.85em; white-space: nowrap; }
.achievements-empty { text-align: center; font-size: clamp(16px, 2.4vh, 28px); padding: 3vh 2vw; }
```

- [ ] **Step 7: Gates.** Vitest 62 + 2 + 4 = 68 passed, tsc clean.

- [ ] **Step 8: Commit.**

```bash
cd /c/Users/torso/Documents/deco/launcher
git add src lang
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "feat(launcher): Logros screen with counter, filters, hidden entries and theme links" -m "Verified: vitest <N> passed, tsc clean" -m "Agent: <builder>"
```

---

### Task 5: Temas (Themes gallery) and the cross-links

**Files:**
- Create: `src/screens/Themes.tsx`, `src/screens/Themes.test.tsx`, `src/screens/ThemesFlow.test.tsx`
- Modify: `src/App.tsx`, `src/styles.css`, `lang/es.json`, `lang/en.json` (under `DECO/launcher`)

**Interfaces:**
- Consumes: Task 3 `api.setTheme`, App `themes`, `applied`, `setApplied`, `shown`/`setShown`, `achievements`;
  Task 4 `openAchievements`, `achFocus`/`setAchFocus`, `achBack`, `Achievements.onTheme(themeId, achievementId)`.
- Produces: `COLS = 3`; `<Themes strings lang themes applied catalogue focus? onPreview(theme) onApply(id)
  onAchievement(achievementId, themeId) onBack />`; App `themesFocus`, `themesBack: "menu" | "achievements"`,
  `openThemes(focus?, from?)`, `applyChosen(id)`.

- [ ] **Step 1: Failing screen tests.** Create `src/screens/Themes.test.tsx`:

```tsx
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Themes } from "./Themes";
import { achievement, theme } from "../test-fixtures";

vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
const strings: Record<string, string> = { "themes.title": "TEMAS", "themes.applied": "✓ En uso", "themes.needs": "Consigue: {name}",
  "themes.locked_forever": "No se puede desbloquear", "themes.apply": "Aplicar" };
const list = [
  theme(),
  theme({ id: "noche", name: "Noche", author: "Patapon Community", preview: "asset://noche/preview.png" }),
  theme({ id: "arena", name: "Arena", unlock: "story_prologue", locked: true }),
  theme({ id: "sombra", name: "Sombra", unlock: "secret_x", locked: true }),
  theme({ id: "fake", name: "Falso", unlock: "made_up", locked: true }),
];
const catalogue = [achievement("story_prologue", { name: { es: "El despertar", en: "The Awakening" } }),
  achievement("secret_x", { hidden: true })];
function setup(over: Partial<Parameters<typeof Themes>[0]> = {}) {
  const props = { strings, lang: "es" as const, themes: list, applied: "default", catalogue,
    onPreview: vi.fn(), onApply: vi.fn(), onAchievement: vi.fn(), onBack: vi.fn(), ...over };
  render(<Themes {...props} />);
  return props;
}
const selected = () => screen.getAllByRole("option").findIndex((o) => o.getAttribute("aria-selected") === "true");

test("cards show name, state and preview; the applied one is focused and previewed", () => {
  const p = setup();
  expect(selected()).toBe(0);
  expect(screen.getByText("✓ En uso")).toBeInTheDocument();
  expect(screen.getByText("Patapon Community")).toBeInTheDocument();
  expect(screen.getByText("Consigue: El despertar")).toBeInTheDocument();
  expect(screen.getByText("Consigue: ???")).toBeInTheDocument();  // a hidden achievement keeps its name secret
  expect(screen.getByText("No se puede desbloquear")).toBeInTheDocument();
  expect(screen.getAllByRole("option")[1].querySelector("img")).toHaveAttribute("src", "asset://noche/preview.png");
  expect(p.onPreview).toHaveBeenLastCalledWith(list[0]);
});

test("moving previews, Enter applies, Esc returns to the applied theme", async () => {
  const p = setup();
  await userEvent.keyboard("{ArrowRight}");
  expect(p.onPreview).toHaveBeenLastCalledWith(list[1]);
  await userEvent.keyboard("{Enter}");
  expect(p.onApply).toHaveBeenCalledWith("noche");
  await userEvent.keyboard("{Escape}");
  expect(p.onPreview).toHaveBeenLastCalledWith(list[0]);
  expect(p.onBack).toHaveBeenCalledOnce();
});

test("a locked card previews but opens its achievement instead of applying", async () => {
  const p = setup();
  await userEvent.keyboard("{ArrowRight}{ArrowRight}");
  expect(p.onPreview).toHaveBeenLastCalledWith(list[2]);
  await userEvent.keyboard("{Enter}");
  expect(p.onApply).not.toHaveBeenCalled();
  expect(p.onAchievement).toHaveBeenCalledWith("story_prologue", "arena");
  await userEvent.keyboard("{ArrowDown}{Enter}");  // last row: Falso, an unknown achievement — nothing to open
  expect(selected()).toBe(4);
  expect(p.onAchievement).toHaveBeenCalledOnce();
});

test("the grid moves by rows and opens on a given theme", async () => {
  setup({ focus: "fake" });
  expect(selected()).toBe(4);
  await userEvent.keyboard("{ArrowUp}");
  expect(selected()).toBe(1);
  await userEvent.keyboard("{ArrowLeft}{ArrowLeft}");
  expect(selected()).toBe(0);
});
```

  Run: `npx vitest run src/screens/Themes.test.tsx 2>&1 | tail -5` → FAIL (module missing).

- [ ] **Step 2: Implement `src/screens/Themes.tsx`:**

```tsx
import { useEffect, useRef, useState } from "react";
import { PromptBar } from "../components/PromptBar";
import { t } from "../i18n";
import { onNav } from "../nav/events";
import type { Achievement, Device, NavAction, ThemeInfo } from "../types";

export const COLS = 3;  // matches .theme-grid's grid-template-columns

type Props = {
  strings: Record<string, string>; lang: "es" | "en"; themes: ThemeInfo[]; applied: string; catalogue: Achievement[];
  focus?: string;                                              // theme to select on open (from Logros)
  onPreview(theme: ThemeInfo): void;                           // the focused card's look and music, live
  onApply(id: string): void;
  onAchievement(achievementId: string, themeId: string): void;   // Enter on a locked card
  onBack(): void;
};

// Temas: a grid of theme cards. Focusing one previews it (background, colours, music); Enter applies it,
// or on a locked one opens the achievement it needs; Esc returns to the applied theme.
export function Themes(p: Props) {
  const [index, setIndex] = useState(() => Math.max(0, p.themes.findIndex((th) => th.id === (p.focus ?? p.applied))));
  const [device, setDevice] = useState<Device>("keyboard");
  const sel = Math.min(index, Math.max(0, p.themes.length - 1));
  const focused = p.themes[sel];
  const selected = useRef<HTMLLIElement>(null);
  useEffect(() => {
    if (focused) p.onPreview(focused);
    selected.current?.scrollIntoView?.({ block: "nearest" });
  }, [focused?.id]);  // eslint-disable-line react-hooks/exhaustive-deps

  const needs = (th: ThemeInfo) => p.catalogue.find((a) => a.id === th.unlock);
  const confirm = (i: number) => {
    const th = p.themes[i];
    if (!th) return;
    if (!th.locked) p.onApply(th.id);
    else if (needs(th)) p.onAchievement(th.unlock!, th.id);
  };
  const back = () => {
    const ap = p.themes.find((th) => th.id === p.applied);
    if (ap) p.onPreview(ap);
    p.onBack();
  };
  const handle = (a: NavAction) => {
    const n = p.themes.length;
    if (a === "back") { back(); return; }
    if (!n) return;
    if (a === "left") setIndex(Math.max(0, sel - 1));
    else if (a === "right") setIndex(Math.min(n - 1, sel + 1));
    else if (a === "up") setIndex(sel - COLS >= 0 ? sel - COLS : sel);
    else if (a === "down") setIndex(sel + COLS < n ? sel + COLS : Math.floor(sel / COLS) < Math.floor((n - 1) / COLS) ? n - 1 : sel);
    else if (a === "confirm") confirm(sel);
  };
  const ref = useRef(handle);
  ref.current = handle;
  useEffect(() => onNav((a, d) => { setDevice(d); ref.current(a); }), []);

  const meta = (th: ThemeInfo) => {
    if (th.id === p.applied) return t(p.strings, "themes.applied");
    if (!th.locked) return th.author;
    const need = needs(th);
    if (!need) return t(p.strings, "themes.locked_forever");
    return t(p.strings, "themes.needs", { name: need.hidden ? "???" : need.name[p.lang] });
  };
  return (
    <div className="screen themes" data-testid="themes">
      <h1 className="title">{t(p.strings, "themes.title")}</h1>
      <ul className="theme-grid" role="listbox">
        {p.themes.map((th, i) => {
          const img = th.preview ?? th.background;
          return (
            <li key={th.id} ref={i === sel ? selected : undefined} role="option" aria-selected={i === sel}
                className={`theme-card${i === sel ? " selected" : ""}${th.locked ? " locked" : ""}`}
                onMouseEnter={() => setIndex(i)} onClick={() => { setIndex(i); confirm(i); }}>
              {img ? <img className="theme-preview" src={img} alt="" /> : (
                <span className="theme-preview theme-swatch" style={{ background: th.colors.bg ?? "var(--c-bg)" }}>
                  <i style={{ background: th.colors.accent ?? "var(--c-accent)" }} />
                  <i style={{ background: th.colors.panel ?? "var(--c-panel)" }} />
                </span>
              )}
              <span className="theme-name">{th.locked ? "🔒 " : ""}{th.name}</span>
              <span className="theme-meta">{meta(th)}</span>
            </li>
          );
        })}
      </ul>
      <PromptBar device={device} items={[
        { id: "select", label: t(p.strings, "prompt.select") },
        { id: "confirm", label: t(p.strings, "themes.apply") },
        { id: "back", label: t(p.strings, "prompt.back") },
      ]} />
    </div>
  );
}
```

  Run the screen tests → 4 passed.

- [ ] **Step 3: Failing flow test (cross-links never stack, apply survives).** Create
  `src/screens/ThemesFlow.test.tsx`:

```tsx
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";

vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ close: vi.fn() }) }));
vi.mock("@tauri-apps/api/webview", () => ({ getCurrentWebview: () => ({ onDragDropEvent: vi.fn(async () => () => {}) }) }));
vi.mock("@tauri-apps/api/core", () => ({ convertFileSrc: (p: string) => p, invoke: vi.fn() }));
const api = vi.hoisted(() => {
  const th = (o: Record<string, unknown>) => ({ id: "default", name: "Patapon", author: "", colors: {}, font: null, background: null,
    background_video: null, logo: null, preview: null, css: null, music: null, music_volume: 100,
    sounds: { move: null, confirm: null, back: null }, unlock: null, locked: false, ...o });
  return {
    th,
    getOptions: vi.fn(async () => ({ options: [], values: { "general.language": "es" }, warnings: [] })),
    getLang: vi.fn(async () => ({ "menu.start": "Iniciar", "menu.options": "Opciones", "menu.exit": "Salir",
      "menu.achievements": "Logros", "menu.themes": "Temas", "themes.needs": "Consigue: {name}",
      "achievements.opens_theme": "🎨 Abre el tema {name}" })),
    listThemes: vi.fn(async () => ({ applied: "default", themes: [th({}),
      th({ id: "arena", name: "Arena", unlock: "story_prologue", locked: true }),
      th({ id: "noche", name: "Noche", colors: { bg: "#123456" } })] })),
    getAchievements: vi.fn(async () => ({ catalogue: [{ id: "story_prologue", icon: "📜", hidden: false,
      name: { es: "El despertar", en: "The Awakening" }, desc: { es: "Completa el prólogo.", en: "Clear the prologue." } }], earned: {} })),
    setTheme: vi.fn(async () => {}),
    listVersions: vi.fn(async () => ({ releases: [{ id: "dxd-1.1.4", name: "P3", disc_id: "X", eboot_sha256: "0" }],
      installed: ["dxd-1.1.4"], current: "dxd-1.1.4" })),
    listSaves: vi.fn(async () => ({ supported: false, slots: [], all_slots: [], chosen: null, continues: null, free: null })),
  };
});
vi.mock("../api", () => ({ api }));
import { App } from "../App";

test("Temas and Logros link to each other without stacking screens", async () => {
  render(<App />);
  await userEvent.click(await screen.findByText("Temas"));
  expect(await screen.findByTestId("themes")).toBeInTheDocument();
  await userEvent.keyboard("{ArrowRight}{Enter}");   // locked Arena -> the achievement it needs
  expect(await screen.findByTestId("achievements")).toBeInTheDocument();
  expect(screen.getByRole("option", { selected: true })).toHaveTextContent("El despertar");
  await userEvent.keyboard("{Enter}");               // -> back to Temas on Arena, not a new layer
  expect(await screen.findByTestId("themes")).toBeInTheDocument();
  expect(screen.getByRole("option", { selected: true })).toHaveTextContent("Arena");
  await userEvent.keyboard("{Escape}");
  expect(await screen.findByText("Iniciar")).toBeInTheDocument();
  expect(api.setTheme).not.toHaveBeenCalled();
});

test("applying a theme saves it and keeps it after leaving the gallery", async () => {
  render(<App />);
  await userEvent.click(await screen.findByText("Temas"));
  await userEvent.keyboard("{ArrowRight}{ArrowRight}{Enter}");
  expect(api.setTheme).toHaveBeenCalledWith("noche");
  await userEvent.keyboard("{Escape}");
  expect(await screen.findByText("Iniciar")).toBeInTheDocument();
  expect(document.documentElement.style.getPropertyValue("--c-bg")).toBe("#123456");
});
```

  Run: `npx vitest run src/screens/ThemesFlow.test.tsx 2>&1 | tail -5` → FAIL (no Temas entry yet).

- [ ] **Step 4: Wire `App.tsx`.**
  1. `import { Themes } from "./screens/Themes";`
  2. After `openAchievements` add:

```tsx
  const [themesFocus, setThemesFocus] = useState<string | undefined>(undefined);
  const [themesBack, setThemesBack] = useState<"menu" | "achievements">("menu");
  const openThemes = (focus?: string, from: "menu" | "achievements" = "menu") => {
    setThemesFocus(focus);
    setThemesBack(from);
    setScreen("themes");
  };
  // Saves [launcher] theme; the shown theme already is the previewed one.
  const applyChosen = async (id: string) => {
    try { await api.setTheme(id); } catch { return; }
    setApplied(id);
    const th = themes.find((x) => x.id === id);
    if (th) setShown(th);
  };
  // Temas <-> Logros links: from a screen the other one opened, the link goes back to it instead of
  // opening a new layer, so Esc never ping-pongs between the two.
  const themesToAchievement = (achievementId: string, themeId: string) => {
    setThemesFocus(themeId);
    if (themesBack === "achievements") { setAchFocus(achievementId); setScreen("achievements"); }
    else openAchievements(achievementId, "themes");
  };
  const achievementToTheme = (themeId: string, achievementId: string) => {
    setAchFocus(achievementId);
    if (achBack === "themes") { setThemesFocus(themeId); setScreen("themes"); }
    else openThemes(themeId, "achievements");
  };
```

  3. The `achievements` branch gets `onTheme={achievementToTheme}`; add the themes branch right after it:

```tsx
  } else if (screen === "themes") {
    body = <Themes strings={strings} lang={lang} themes={themes} applied={applied} catalogue={achievements.catalogue}
      focus={themesFocus} onPreview={setShown} onApply={(id) => { void applyChosen(id); }}
      onAchievement={themesToAchievement} onBack={() => setScreen(themesBack)} />;
```

  4. `MainMenu` gets `onThemes={() => openThemes()}`.

- [ ] **Step 5: Strings and styles.** `lang/es.json`:

```json
  "themes.title": "TEMAS",
  "themes.applied": "✓ En uso",
  "themes.needs": "Consigue: {name}",
  "themes.locked_forever": "No se puede desbloquear",
  "themes.apply": "Aplicar",
```

  `lang/en.json`:

```json
  "themes.title": "THEMES",
  "themes.applied": "✓ In use",
  "themes.needs": "Earn: {name}",
  "themes.locked_forever": "Cannot be unlocked",
  "themes.apply": "Apply",
```

  Append to `src/styles.css`:

```css
/* Temas */
.themes { justify-content: flex-start; padding-top: 5vh; gap: 2vh; }
.theme-grid {
  list-style: none; margin: 0; padding: 0.4em; display: grid; grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 2vh 1.5vw; width: min(1400px, 90vw); max-height: 70vh; overflow-y: auto; box-sizing: border-box;
}
.theme-card {
  display: flex; flex-direction: column; gap: 0.4em; padding: 0.8em; border-radius: 0.6em; cursor: pointer;
  background: var(--c-panel); color: var(--c-panelText); border: 3px solid transparent;
  font-size: clamp(14px, 2.1vh, 25px);
}
.theme-card.selected { border-color: var(--c-accent); }
.theme-card.locked .theme-preview { filter: grayscale(1) brightness(0.6); }
.theme-preview { display: block; width: 100%; aspect-ratio: 16 / 9; object-fit: cover; border-radius: 0.35em; }
.theme-swatch { display: flex; align-items: flex-end; gap: 0.4em; padding: 0.6em; box-sizing: border-box; }
.theme-swatch i { display: block; width: 2em; height: 2em; border-radius: 0.3em; }
.theme-name { font-weight: 700; }
.theme-meta { font-size: 0.85em; opacity: 0.8; min-height: 1.2em; }
```

- [ ] **Step 6: Gates.** Vitest 68 + 4 + 2 = 74 passed, tsc clean.

- [ ] **Step 7: Commit.**

```bash
cd /c/Users/torso/Documents/deco/launcher
git add src lang
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "feat(launcher): Temas gallery with live preview, locks and links to Logros" -m "Verified: vitest <N> passed, tsc clean" -m "Agent: <builder>"
```

---

### Task 6: Sample themes, their generator, and docs

**Files:**
- Create: `scripts/make-sample-themes.mjs`, `scripts/themes.test.mjs`, `themes/noche/theme.json`,
  `themes/noche/theme.css`, `themes/arena/theme.json`, `themes/arena/theme.css`, generated
  `themes/noche/{bg.webm,preview.png,music.ogg,move.ogg,confirm.ogg,back.ogg}`,
  `themes/arena/{music.ogg,move.ogg,confirm.ogg,back.ogg}`
- Modify: `themes/default/theme.json`, `README.md` (under `DECO/launcher`)

**Interfaces:**
- Consumes: the theme format (Task 2), `story_prologue` in the official catalogue (Task 1).
- Produces: three shipped themes `default` (unchanged look), `noche` (video background, music, sounds),
  `arena` (music, sounds, locked by `story_prologue`).

- [ ] **Step 1: Failing test.** Create `scripts/themes.test.mjs`:

```js
// Every theme shipped in themes/ loads: valid id, theme.json parses, every file it names is safe and
// exists, media stays small, and a lock names an official achievement.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";

const safe = (n) => typeof n === "string" && n !== "" && !n.startsWith("/") && !/[\\:]/.test(n)
  && n.split("/").every((c) => c !== "" && c !== "." && c !== "..");
const catalogue = JSON.parse(readFileSync("../psprecomp/games/patapon3/achievements.json", "utf8"));
const official = new Set(catalogue.achievements.map((a) => a.id));

test("shipped themes are complete, safe and small", () => {
  const themes = readdirSync("themes");
  assert.ok(themes.includes("default") && themes.length >= 3, themes.join());
  for (const id of themes) {
    assert.match(id, /^[a-z0-9_-]+$/);
    const t = JSON.parse(readFileSync(join("themes", id, "theme.json"), "utf8"));
    assert.ok(t.name && t.author, `${id}: name and author`);
    const files = [t.font, t.background_video, t.music, ...Object.values(t.sounds ?? {})].filter((f) => f != null);
    for (const f of files) {
      assert.ok(safe(f), `${id}: ${f}`);
      assert.ok(existsSync(join("themes", id, f)), `${id}: ${f} missing (node scripts/make-sample-themes.mjs)`);
      assert.ok(statSync(join("themes", id, f)).size < 3_000_000, `${id}: ${f} too big`);
    }
    if (t.unlock !== undefined) assert.ok(official.has(t.unlock), `${id}: unlock ${t.unlock} is not official`);
  }
  assert.equal(JSON.parse(readFileSync("themes/arena/theme.json", "utf8")).unlock, "story_prologue");
  assert.ok(existsSync("themes/noche/bg.webm") && existsSync("themes/noche/preview.png"));
});
```

  Run the scripts gate → this test FAILS (no `noche` / `arena`, default has no name).

- [ ] **Step 2: The generator.** Create `scripts/make-sample-themes.mjs` (tested on 2026-10-18 with FFmpeg 8.1 winget
  build: produces exactly the files listed, `bg.webm` 12 s ≈ 34 KB, each `.ogg` < 20 KB):

```js
// Regenerates the sample themes' media (no game art or audio): a looping gradient video and its preview
// frame, a short drum loop and three UI blips per theme, all synthesized by FFmpeg.
//   node scripts/make-sample-themes.mjs [--ffmpeg <path>]   (default: ffmpeg on PATH)
import { execFileSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const i = process.argv.indexOf("--ffmpeg");
const ffmpeg = i > 0 ? process.argv[i + 1] : "ffmpeg";
const run = (args) => execFileSync(ffmpeg, ["-hide_banner", "-loglevel", "error", "-y", ...args], { stdio: "inherit" });
const ogg = (expr, d, out) => run(["-f", "lavfi", "-i", `aevalsrc='${expr}':s=44100:d=${d}`, "-c:a", "libvorbis", "-q:a", "3", out]);
// "pata pata pata pon": a high drum on the first three quarters of each bar, a low one on the last.
const drums = (hi, lo, bar, beat) =>
  `0.5*sin(2*PI*if(lt(mod(t\\,${bar})\\,${+(bar * 0.75).toFixed(3)})\\,${hi}\\,${lo})*t)*exp(-12*mod(t\\,${beat}))+0.08*sin(2*PI*${lo / 2}*t)`;
const blip = (f) => `0.4*sin(2*PI*${f}*t)*exp(-40*t)`;
const two = (a, b) => `0.4*sin(2*PI*if(lt(t\\,0.07)\\,${a}\\,${b})*t)*exp(-15*t)`;
function sounds(dir, base) {
  ogg(blip(base * 2), 0.12, join(dir, "move.ogg"));
  ogg(two(base * 1.5, base * 2.25), 0.25, join(dir, "confirm.ogg"));
  ogg(two(base, base * 0.75), 0.25, join(dir, "back.ogg"));
}

const noche = join("themes", "noche");
mkdirSync(noche, { recursive: true });
// 6 s of drifting gradient played forward then backward: a seamless 12 s loop.
run(["-f", "lavfi", "-i", "gradients=s=960x540:r=24:d=6:c0=0x0b1030:c1=0x3a1060:c2=0x10304a:nb_colors=3:speed=0.08",
  "-vf", "format=yuv420p,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1",
  "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "42", "-row-mt", "1", "-an", join(noche, "bg.webm")]);
run(["-i", join(noche, "bg.webm"), "-frames:v", "1", "-vf", "scale=480:270", join(noche, "preview.png")]);
ogg(drums(220, 110, 2, 0.5), 8, join(noche, "music.ogg"));
sounds(noche, 440);

const arena = join("themes", "arena");
mkdirSync(arena, { recursive: true });
ogg(drums(247, 123.5, 1.6, 0.4), 6.4, join(arena, "music.ogg"));
sounds(arena, 392);
console.log("sample themes written");
```

- [ ] **Step 3: Theme files.** `themes/default/theme.json` becomes:

```json
{
  "name": "Patapon",
  "author": "Patapon Community",
  "colors": {
    "bg": "#0b0b0e",
    "text": "#ffffff",
    "accent": "#f5a400",
    "panel": "#e9e9e4",
    "panelText": "#111111",
    "muted": "#9a9a9a"
  },
  "font": null
}
```

  `themes/noche/theme.json`:

```json
{
  "name": "Noche",
  "author": "Patapon Community",
  "colors": { "bg": "#070912", "text": "#f2f0ff", "accent": "#8fd3ff", "panel": "#161a2e", "panelText": "#e8e8f5", "muted": "#8a8fb0" },
  "font": null,
  "background_video": "bg.webm",
  "music": "music.ogg",
  "music_volume": 80,
  "sounds": { "move": "move.ogg", "confirm": "confirm.ogg", "back": "back.ogg" }
}
```

  `themes/noche/theme.css`:

```css
/* Noche: a soft glow on the logo and the focused entry over the moving night gradient. */
.main-menu .logo { filter: drop-shadow(0 0 18px rgba(143, 211, 255, 0.5)); }
.menu li.selected { text-shadow: 0 0 12px rgba(143, 211, 255, 0.7); }
```

  `themes/arena/theme.json`:

```json
{
  "name": "Arena",
  "author": "Patapon Community",
  "colors": { "bg": "#2a1708", "text": "#fff4e0", "accent": "#ff7a1a", "panel": "#f3e2c0", "panelText": "#2a1708", "muted": "#c8a77a" },
  "font": null,
  "music": "music.ogg",
  "music_volume": 70,
  "sounds": { "move": "move.ogg", "confirm": "confirm.ogg", "back": "back.ogg" },
  "unlock": "story_prologue"
}
```

  `themes/arena/theme.css`:

```css
/* Arena: warm sand with a low sun behind the menu (no image, CSS only). */
.screen.main-menu { background: radial-gradient(ellipse at 50% 120%, rgba(255, 122, 26, 0.35), transparent 60%); }
```

- [ ] **Step 4: Generate the media.** Run: `cd /c/Users/torso/Documents/deco/launcher && node scripts/make-sample-themes.mjs
  && ls -la themes/noche themes/arena`. Expected: "sample themes written"; the 10 media files exist.

- [ ] **Step 5: Run the scripts gate.** Expected: every test passes (make-portable 2 + themes 1).

- [ ] **Step 6: README.** In `README.md`, insert before `## settings.ini` (Spanish part):

```markdown
## Temas y logros

Menú principal → **Temas**: una tarjeta por carpeta de `themes/`. Al enfocar una se ve y se oye (fondo, colores y
música); Confirmar la aplica (`settings.ini` `[launcher] theme`) y Esc vuelve al tema aplicado. Opciones → General →
**Volumen de la música** (0–100). Vienen `default`, `noche` (fondo animado) y `arena` (se desbloquea con el logro
«El despertar»). Formato, todo opcional: `themes/<id>/theme.json` con `name`, `author`, `colors` (`bg`, `text`,
`accent`, `panel`, `panelText`, `muted`), `font`, `background_video` (`.webm`), `music` (`.ogg`), `music_volume`,
`sounds` (`move`, `confirm`, `back`) y `unlock` (id de un logro oficial), más `background.png`, `logo.png`,
`preview.png` y `theme.css`. Los nombres de archivo son relativos a la carpeta del tema. Los medios de ejemplo se
regeneran con `node scripts/make-sample-themes.mjs` (FFmpeg); ningún tema incluye arte ni audio del juego.

Menú principal → **Logros**: la lista oficial (`game/<id>/achievements.json`, la misma para todos los jugadores) con
lo conseguido en este PC (`achievements/unlocked.json`, que escribe el juego; el launcher solo lo lee), Q/E para
filtrar y un enlace al tema que abre cada logro. Contrato: `psprecomp/docs/contracts/achievements-v1.md`.
```

  and before `**settings.ini:**` in the English part:

```markdown
**Themes and achievements:** main menu → **Themes**: one card per `themes/` folder; focusing one previews it
(background, colours, music), Confirm applies it (`settings.ini` `[launcher] theme`), Esc returns to the applied
one. Options → General → **Music volume** (0–100). Ships `default`, `noche` (animated background) and `arena`
(unlocked by the "The Awakening" achievement). `themes/<id>/theme.json`, every key optional: `name`, `author`,
`colors`, `font`, `background_video` (`.webm`), `music` (`.ogg`), `music_volume`, `sounds` (`move`, `confirm`,
`back`), `unlock` (an official achievement id), plus `background.png`, `logo.png`, `preview.png`, `theme.css`; file
names are relative to the theme folder. Sample media: `node scripts/make-sample-themes.mjs` (FFmpeg); no theme
contains game art or audio. Main menu → **Achievements**: the official list (`game/<id>/achievements.json`) with
what this PC earned (`achievements/unlocked.json`, written by the game, only read by the launcher), Q/E filters and
a link to the theme each one unlocks. Contract: `psprecomp/docs/contracts/achievements-v1.md`.
```

  Also replace the paragraph that starts `Volver a empaquetar solo reemplaza` (README lines ~110-112) with:

```markdown
Volver a empaquetar solo reemplaza `launcher.exe`, `game/`, `lang/`, `themes/` y `skins/`: `versions/`
(las versiones instaladas y las partidas), `achievements/`, `settings.ini` y `logs/` son del jugador y se
quedan como están.
```

  and the one that starts `Re-packaging only replaces` (~201-203) with:

```markdown
Re-packaging only replaces `launcher.exe`, `game/`, `lang/`, `themes/` and `skins/`: `versions/` (the
installed releases and their saves), `achievements/`, `settings.ini` and `logs/` belong to the player and are
left alone.
```

- [ ] **Step 7: All gates** (Rust, UI, scripts) — nothing else changed, counts as after Task 5 plus the new scripts test.

- [ ] **Step 8: Commit.**

```bash
cd /c/Users/torso/Documents/deco/launcher
git add scripts/make-sample-themes.mjs scripts/themes.test.mjs themes README.md
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "feat(launcher): sample themes Noche and Arena, generator and docs" -m "Verified: node --test scripts <N> pass" -m "Agent: <builder>"
```

---

### Task 7: Package, end to end, Findings (Claude)

- [ ] **Step 1: Package.** Copy the catalogue next to the other two exes (the CMake rule does it from their next
  build): `cd /c/Users/torso/Documents/deco && cp psprecomp/games/patapon3/achievements.json build/v115b/rtw/ &&
  cp psprecomp/games/patapon3/achievements.json build/vanilla/rtw/`.
  `cd launcher && npm run tauri build -- --no-bundle`, then make-portable into `build/e2e_portable3` and
  `build/e2e_portable` with the same three `--game` pairs as part 5's Findings. Check `game/*/achievements.json` and
  `themes/{default,noche,arena}` in both; `versions/`, `settings.ini`, `textures/` untouched.
- [ ] **Step 2: Drive (CDP).** Launcher from `e2e_portable3` with `WEBVIEW2_USER_DATA_FOLDER` = a scratch folder
  and `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222`, stdout/stderr to
  `build/e2e5b/launcher_stdout.log`. Add a `size <w> <h>` command to `build/e2e3/cdp.mjs`
  (`Emulation.setDeviceMetricsOverride` `{width, height, deviceScaleFactor: 1, mobile: false}`). Check and record:
  1. Main menu: Iniciar · Partidas · Logros · Temas · Opciones · Salir; default logo loads
     (`document.querySelector("img.logo").naturalWidth > 0` — proves the narrowed asset scope).
  2. Music: `document.getElementById("theme-music")` absent with `default` (no music).
  3. Logros: "0 / 24 · 0 %", every entry pending, two "???"; Q/E filters; screenshot.
  4. Temas: 3 cards, Arena "🔒 … Consigue: El despertar". Focus Noche → `video.backdrop` playing
     (`!paused`), `#theme-music` playing with `volume` 0.56 (0.8 × 0.7) — if `paused` is true before any key, record
     whether one key press starts it (autoplay flag vs the retry path; note it). Esc → default back, no video, no music.
  5. Enter on Noche → applied; `settings.ini` `[launcher] theme = noche`; close and relaunch → Noche still applied.
  6. Options › General › Volumen de la música 70 → 30, save → `#theme-music.volume` 0.24.
  7. Write `e2e_portable3/achievements/unlocked.json` with `story_prologue` (hero Edson, 2026-10-18T12:00:00Z),
     `missions_1`, `vs_1` → reopen Logros: "3 / 24 · 13 %", "18 oct 2026 · Edson", "🎨 Abre el tema Arena" → Enter →
     Temas on Arena, unlocked → apply it. Corrupt the file (`{`), note its SHA-256, reopen Logros/Temas: nothing earned,
     Arena locked, default shown, `settings.ini` still `theme = arena`, SHA-256 unchanged. Restore the good file.
  8. 1920×1080 and 1280×800 (`size`), Logros and Temas: screenshots; nothing outside the viewport
     (`[...document.querySelectorAll(".title, .theme-card, .achievement, .prompt-bar")].every(e => { const r =
     e.getBoundingClientRect(); return r.left >= 0 && r.right <= innerWidth; })`) and the Temas grid needs no scroll
     with 3 themes.
  9. Game: with Noche applied, Iniciar DxD 1.1.4 (game on port 9998 via `PSPRECOMP_DEBUG_PORT`), `e2e3.py hideout`;
     while it runs `#theme-music.paused` and `video.backdrop.paused` are true. CPU of `msedgewebview2` + `launcher`
     over 30 s in the hideout (`Get-Process … | Measure-Object CPU -Sum`, before/after): with Noche ≤ the same
     measurement with `default` + 0.5 s. F1 → Sí → back in the launcher: music and video resume.
- [ ] **Step 3: Gamepad.** With the DualSense (or the gilrs nav events): menu → Temas → move/apply → Logros → Q/E
  (L1/R1) → back; UI sounds play on move/confirm/back with Noche.
- [ ] **Step 4: Findings.** `## Findings` at the end of this plan (commits, gates, E2E table, plan defects, what is
  left for the user's check), commit `docs(plan): launcher part 5b plan 1 findings` in psprecomp; ask the user to try
  Temas and Logros on their PC (sound on, keyboard and DualSense) and later on the Steam Deck (WebKitGTK autoplay and
  `.webm` playback are part 7's checks); push launcher + psprecomp only on their OK. Next: Plan 2 (game) — writing-plans
  from spec §4 and §5.
