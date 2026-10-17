# Community launcher — Part 1 (settings contract + launcher shell) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a Tauri launcher with a main menu and Options that writes a versioned `settings.ini`,
launches the Patapon 3 port, and comes back when the player picks "back to menu" in the game.

**Architecture:** the game (`psprecomp`, C++) learns to read `settings.ini` (turning each key into
today's `PSPRECOMP_*` variable when that variable is not already set), a `--log` path, and a
Select+Start / F1 "back to menu" prompt that exits with code 10. The launcher (`DECO/launcher`,
Tauri 2 + React 19 + TypeScript 7.0.2) reads and writes the ini without losing comments, builds
its option rows from two catalogs (`core_options.json`, `game/game_options.json`), reads the
gamepad in Rust with `gilrs`, and launches / waits for the game.

**Tech Stack:** C++17 runtime (CMake, ctest), nlohmann/json 3.11.3 (vendored header); Rust 1.96
(MSVC), Tauri 2, gilrs 0.11, serde; Node 24 / npm 11, React 19, Vite, TypeScript 7.0.2, Vitest +
Testing Library (jsdom).

**Spec:** `docs/superpowers/specs/2026-10-17-launcher-part1-design.md` (approved 2026-10-17).

`DECO` = `C:/Users/torso/Documents/deco`. Port repo = `DECO/psprecomp` (branch
`patapon3-windows`). Launcher repo = `DECO/launcher` (new, local git, branch `main`).

## Global Constraints

- Distribution model: prebuilt game exe + the player's ISO; nothing is compiled on the player's PC.
- Launcher: Tauri 2 + React (TSX) with TypeScript **7.0.2**; React 19.
- Cross-platform design, Windows first: no Windows-only assumption in the contract or the
  launcher; the game executable is `game/psprecomp_runtime.exe` on Windows and
  `game/psprecomp_runtime` elsewhere.
- `settings.ini` v1 exactly as in the spec section 2 (keys, defaults, precedence: environment
  variable already set > `settings.ini` > runtime default). Invalid values / unknown keys are never
  fatal.
- Paths inside `settings.ini` are relative to the folder holding `settings.ini`.
- Exit codes: 0 = quit, 10 = back to the launcher menu, anything else = crash.
- Without a `settings.ini` (and without `--settings`) the game behaves exactly as today:
  `jugar.bat`, `jugar_60fps.bat`, tours and `rt_regress` unchanged.
- The generic runtime (`psprecomp/runtime`) has no Patapon 3 knowledge; Patapon 3 options live in
  `games/patapon3/game_options.json`.
- Launcher languages: `es` and `en` (`lang/<code>.json`); plain default theme in
  `themes/default/` (colors, free font, `background.png`, `logo.png`, optional CSS).
- Gamepad in the launcher is read in Rust (`gilrs`), never through the Web Gamepad API.
- Portable folder, nothing in AppData.
- Repo rules (port): never edit `build/p3_output/generated/*.cpp`; `runtime/` sources and docs are
  CRLF — use the Edit tool, not `sed -i`; test game instances only on port **9998**
  (`PSPRECOMP_DEBUG_PORT=9998`), never touch the user's game (port 9999) or `DECO/savedata`;
  before building check `netstat -ano | grep ":9999 .*LISTEN"` and ask if the user is playing.
- Commits: author "Patapon Community" (repo identity, do not touch git config), English, no
  `Co-Authored-By`, end with `Verified: ...` and `Agent: ...`. No push of either repo until the
  user says so (port pushes use the worktree cherry-pick recipe, next branch `push-p3-6`; never
  push `f639180`). The launcher repo is never pushed in part 1.
- Talk to the user in Spanish.

## Review Focus

1. **A hand-edited `settings.ini`** (BOM, CRLF, missing sections, `fullscreen = maybe`,
   `render_scale = 99`, duplicate keys): the game must start with defaults for the bad keys and
   log each one. Pinned by the `bom_crlf.ini` / `invalid.ini` fixtures in Task 1-2.
2. **The portable folder under a path with spaces** (`C:\Juegos\Patapon 3\`): launch and every
   path key must work. Pinned in Task 2 (`base dir with spaces`) and Task 7 (`spawn args`).
   Non-ASCII folders are a known limit of the runtime's narrow-string file APIs: the launcher
   warns (Task 7 `path_is_ascii`).
3. **The game exe or `disc0` missing, or the game dying at once:** the launcher must not stay
   hidden; it shows what is missing or the crash notice. Pinned in Task 7 (`check_install`,
   `outcome_from_code`) and Task 11 (crash screen test).
4. **Start pressed twice / keys mashed during launch:** only one game process. Pinned in Task 7
   (`LaunchGuard`) and Task 11 (button disabled while launching).
5. **Select+Start tapped during play** (the game reads both): no prompt unless both are held
   1 s, and the prompt defaults to "No". Pinned in Task 4 (`ComboHold`, `MenuPrompt` tests).

---

## File map

Port (`DECO/psprecomp`):
- `docs/contracts/settings-v1.md` — the contract (Task 1).
- `docs/contracts/fixtures/{defaults,custom,invalid,bom_crlf,future}.ini` — shared fixtures (Task 1).
- `games/patapon3/game_options.json` — Patapon 3 option catalog (Task 1).
- `runtime/third_party/nlohmann/json.hpp` (+ `LICENSE.MIT`) — vendored (Task 2).
- `runtime/include/psp_ini.h`, `runtime/src/psp_ini.cpp` — pure INI parser (Task 2).
- `runtime/include/psp_settings.h`, `runtime/src/psp_settings.cpp` — resolve + apply (Tasks 2-3).
- `runtime/tests/test_settings.cpp` — ctest `settings_tests` (Tasks 2-3).
- `runtime/include/psp_menu_prompt.h`, `runtime/src/psp_menu_prompt.cpp` — pure combo + prompt (Task 4).
- `runtime/tests/test_menu_prompt.cpp` — ctest `menu_prompt_tests` (Task 4).
- Modify: `runtime/src/main.cpp` (args, settings, exit code), `runtime/include/psp_exit.h`
  (requested exit code), `runtime/src/psp_event_loop.cpp` (F1 / combo / prompt input),
  `runtime/src/psp_osk_overlay.cpp` + `runtime/include/psp_osk_overlay.h` (prompt drawing),
  `runtime/src/psp_ge_draw.cpp` (call the prompt overlay), `runtime/CMakeLists.txt`,
  `docs/ENV_FLAGS.md`, `ARCHITECTURE.md`, `README.md`.

Launcher (`DECO/launcher`):
- `package.json`, `tsconfig.json`, `vite.config.ts`, `index.html`, `.gitignore`.
- `src-tauri/{Cargo.toml,build.rs,tauri.conf.json,capabilities/default.json}`.
- `src-tauri/src/main.rs` (entry), `lib.rs` (builder + commands), `ini.rs` (format-preserving
  INI), `options.rs` (catalog types + validation + defaults), `settings.rs` (root, read/write),
  `launch.rs` (exe lookup, checks, spawn, outcome, guard), `gamepad.rs` (gilrs -> nav events),
  `assets.rs` (lang + theme files).
- `src-tauri/core_options.json` — catalog of the core keys (Task 6).
- `src-tauri/tests/fixtures/*.ini` — copies of the port fixtures (Task 6).
- `src/main.tsx`, `src/App.tsx`, `src/api.ts`, `src/i18n.ts`, `src/theme.ts`,
  `src/nav/{events.ts,useNav.ts}`, `src/screens/{MainMenu,Options,Crash}.tsx`,
  `src/components/{PromptBar,OptionRow,Tabs}.tsx`, `src/styles.css`, `src/types.ts`.
- `src/**/*.test.tsx` — Vitest.
- `lang/es.json`, `lang/en.json`, `themes/default/{theme.json,background.png,logo.png,theme.css}`.
- `scripts/make-portable.mjs` — assembles `dist/Patapon3Recomp/` (Task 12).

---

### Task 1: The contract — spec doc, fixtures, Patapon 3 catalog

**Files:**
- Create: `psprecomp/docs/contracts/settings-v1.md`
- Create: `psprecomp/docs/contracts/fixtures/defaults.ini`, `custom.ini`, `invalid.ini`,
  `bom_crlf.ini`, `future.ini`
- Create: `psprecomp/games/patapon3/game_options.json`

**Interfaces:**
- Produces: the fixture files and the catalog schema used by Tasks 2, 3, 6, 10.
  Catalog schema (both `core_options.json` and `game_options.json`):
  `{ "version": 1, "options": [ { "section", "key", "type": "bool"|"enum"|"int"|"string"|"path",
  "values"?: [string], "min"?: int, "max"?: int, "default": string, "tab": "general"|"graphics"|
  "online", "label": {"es","en"}, "description"?: {"es","en"}, "display"?: {value: {"es","en"}},
  "env"?: string, "map"?: {value: string|null} } ] }`. All values are strings
  (`"true"`/`"false"` for bools). `map` turns a value into the env value (`null` = leave unset);
  without `map` the value is copied as is.

- [ ] **Step 1: Write `docs/contracts/settings-v1.md`.** Content: copy section 2 of the spec
  verbatim (the default ini and the rules), then add:
  - Parsing: UTF-8 with or without BOM, CRLF or LF; `;` and `#` start a comment only at the start
    of a line (after spaces); `key = value` splits at the first `=`; keys and section names are
    case-insensitive (lowercased); values are trimmed; a key repeated in a section keeps the last
    value; lines that are neither section, key nor comment are ignored with a log line.
  - Booleans: `true|false|1|0|yes|no|on|off`, case-insensitive. Ints: decimal, optional sign.
  - Core key table (section.key -> env, type, rule):

    | key | env | type | rule |
    |---|---|---|---|
    | general.language | PSPRECOMP_UI_LANG | enum es,en | copied |
    | paths.disc0 | PSPRECOMP_DISC0 | path | resolved against the ini folder |
    | paths.savedata | PSPRECOMP_SAVEDATA | path | resolved |
    | display.fullscreen | PSPRECOMP_FULLSCREEN | bool | true -> `1`, false -> unset |
    | display.window_scale | PSPRECOMP_WINDOW_SCALE | int 1..8 | copied |
    | display.render_scale | PSPRECOMP_RENDER_SCALE | int 1..8 | copied |
    | display.present_filter | PSPRECOMP_PRESENT_FILTER | enum linear,nearest | copied |
    | display.texture_filter | PSPRECOMP_TEX_FILTER | enum auto,nearest,linear | copied |
    | display.hd_textures + display.texture_pack | PSPRECOMP_TEXTURES | bool + path | resolved pack path when true, unset when false |
    | online.server | PSPRECOMP_ADHOC_SERVER | string (non-empty) | copied |
    | online.nickname | PSPRECOMP_ADHOC_NICK | string (non-empty) | copied |
    | online.relay | PSPRECOMP_ADHOC_RELAY | bool | true -> `1`, false -> unset |
    | online.port_offset | PSPRECOMP_ADHOC_PORT_OFFSET | int 0..65535 | copied |
    | online.mac_file | PSPRECOMP_ADHOC_MAC_FILE | path | resolved |
    | game.* | from `game_options.json` | per catalog | `map` or copied |

  - Command line: `--settings <path>`, `--log <path>` (stderr goes there), both optional.
  - Exit codes 0 / 10 / other.
- [ ] **Step 2: Fixtures.**
  `defaults.ini` = the spec's default ini, byte for byte (LF).
  `custom.ini`:
  ```ini
  [meta]
  version = 1
  [general]
  language = en
  [paths]
  disc0 = ../data/disc0
  savedata = Partidas Guardadas
  [display]
  fullscreen = no
  window_scale = 3
  render_scale = 2
  present_filter = nearest
  texture_filter = linear
  hd_textures = off
  texture_pack = textures/INFN00001
  [game]
  fps = 60
  smooth_animation = false
  fast_loading = TRUE
  [online]
  server = patapon.example
  nickname = Edson
  relay = yes
  port_offset = 0
  mac_file = adhoc_mac.txt
  ```
  `invalid.ini`:
  ```ini
  [meta]
  version = 1
  garbage line without equals
  [display]
  fullscreen = maybe
  render_scale = 99
  render_scale = 3
  window_scale = abc
  present_filter = cubic
  [game]
  fps = 45
  unknown_key = 1
  [nosuchsection]
  x = 1
  [online]
  nickname =
  port_offset = 70000
  ```
  `bom_crlf.ini`: UTF-8 BOM + CRLF line endings, content
  `[display]` / `render_scale = 5` / `[online]` / `nickname = Ñandú`. Write it with
  `printf '\xEF\xBB\xBF[display]\r\nrender_scale = 5\r\n[online]\r\nnickname = \xC3\x91and\xC3\xBA\r\n' > bom_crlf.ini`.
  `future.ini`: `[meta]` / `version = 2` / `[display]` / `render_scale = 6` / `[display2]` / `hdr = on`.
  Add `docs/contracts/fixtures/.gitattributes` with `* -text` so git never rewrites line endings.
- [ ] **Step 3: `games/patapon3/game_options.json`:**
  ```json
  {
    "version": 1,
    "game": "patapon3",
    "options": [
      {
        "section": "game", "key": "fps", "type": "enum", "values": ["30", "60"], "default": "30",
        "tab": "graphics", "env": "PSPRECOMP_PATCHES", "map": {"30": null, "60": "60fps"},
        "label": {"es": "Fotogramas por segundo", "en": "Frame rate"},
        "description": {"es": "60 usa el parche de 60 fps de la comunidad.", "en": "60 uses the community 60 fps patch."},
        "display": {"30": {"es": "30 fps", "en": "30 fps"}, "60": {"es": "60 fps", "en": "60 fps"}}
      },
      {
        "section": "game", "key": "smooth_animation", "type": "bool", "default": "true",
        "tab": "graphics", "env": "PSPRECOMP_P3_ANIM_SMOOTH", "map": {"true": "1", "false": "0"},
        "label": {"es": "Animación suave", "en": "Smooth animation"},
        "description": {"es": "Muestra la pose intermedia entre dos fotogramas del juego.", "en": "Shows the in-between pose of two game frames."}
      },
      {
        "section": "game", "key": "fast_loading", "type": "bool", "default": "true",
        "tab": "graphics", "env": "PSPRECOMP_P3_FAST_LOAD", "map": {"true": "1", "false": "0"},
        "label": {"es": "Cargas rápidas", "en": "Fast loading"},
        "description": {"es": "Las pantallas de CARGANDO van a la velocidad del PC (nunca en sesiones online).", "en": "Loading screens run at PC speed (never in online sessions)."}
      }
    ]
  }
  ```
- [ ] **Step 4: Check the JSON parses:** `python -I -c "import json;json.load(open('games/patapon3/game_options.json',encoding='utf-8'))"`.
  Expected: no output, exit 0.
- [ ] **Step 5: Commit** (port repo) `docs(contracts): settings.ini v1 contract, fixtures and the
  Patapon 3 option catalog` with `Verified: game_options.json parses; fixtures byte-checked
  (bom_crlf.ini starts EF BB BF, has CRLF)` and `Agent:`.

### Task 2: Game — INI parser and settings resolution (pure)

**Files:**
- Create: `runtime/third_party/nlohmann/json.hpp`, `runtime/third_party/nlohmann/LICENSE.MIT`
- Create: `runtime/include/psp_ini.h`, `runtime/src/psp_ini.cpp`
- Create: `runtime/include/psp_settings.h`, `runtime/src/psp_settings.cpp`
- Create: `runtime/tests/test_settings.cpp`
- Modify: `runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 fixtures and `games/patapon3/game_options.json`.
- Produces:
  ```cpp
  // psp_ini.h
  struct IniDoc {
      std::map<std::string, std::map<std::string, std::string>> values;  // lowercased names
      std::vector<std::string> warnings;                                  // per ignored line
      const std::string* get(const std::string& section, const std::string& key) const;
  };
  IniDoc psp_ini_parse(const std::string& text);
  // psp_settings.h
  struct SettingsResolution {
      std::vector<std::pair<std::string, std::string>> set;  // env name -> value, in order
      std::vector<std::string> log;                           // "[SETTINGS] ..." lines
  };
  using EnvGetter = std::function<const char*(const char*)>;
  SettingsResolution psp_settings_resolve(const std::string& ini_text,
                                          const std::string& catalog_json,  // "" = none
                                          const std::string& base_dir,      // ini folder, '/'
                                          const EnvGetter& getenv_fn);
  ```

- [ ] **Step 1: Vendor nlohmann/json 3.11.3.**
  `curl -L -o runtime/third_party/nlohmann/json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp`
  and `curl -L -o runtime/third_party/nlohmann/LICENSE.MIT https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT`.
  Check: `grep -c "NLOHMANN_JSON_VERSION_PATCH 3" runtime/third_party/nlohmann/json.hpp` -> 1.
- [ ] **Step 2: Failing tests** `runtime/tests/test_settings.cpp`:
  ```cpp
  // Unit tests for settings.ini v1 (docs/contracts/settings-v1.md): the INI parser and the
  // resolution of keys into PSPRECOMP_* environment variables.
  #include "psp_ini.h"
  #include "psp_settings.h"

  #include <cstdio>
  #include <fstream>
  #include <map>
  #include <sstream>
  #include <string>

  static int failures = 0;
  #define CHECK(cond, msg) \
      do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

  static std::string slurp(const std::string& path) {
      std::ifstream f(path, std::ios::binary);
      std::stringstream ss;
      ss << f.rdbuf();
      return ss.str();
  }
  static const std::string kFix = std::string(PSP_CONTRACT_DIR) + "/fixtures/";
  static const std::string kCatalog = slurp(PSP_GAME_OPTIONS_JSON);

  // env name -> value of a resolution; "<unset>" when not set.
  static std::string val(const SettingsResolution& r, const char* env) {
      std::string v = "<unset>";
      for (const auto& kv : r.set) if (kv.first == env) v = kv.second;
      return v;
  }
  static bool logged(const SettingsResolution& r, const char* needle) {
      for (const auto& l : r.log) if (l.find(needle) != std::string::npos) return true;
      return false;
  }
  static const EnvGetter kNoEnv = [](const char*) -> const char* { return nullptr; };

  static void parser_rules() {
      IniDoc d = psp_ini_parse("; c\n# c\n[Display]\n Render_Scale = 4 \nx=a=b\nbad line\n[online]\nnick=\n");
      CHECK(d.get("display", "render_scale") && *d.get("display", "render_scale") == "4",
            "sections and keys are lowercased, values trimmed");
      CHECK(d.get("display", "x") && *d.get("display", "x") == "a=b", "split at the first '='");
      CHECK(d.get("online", "nick") && d.get("online", "nick")->empty(), "empty value kept");
      CHECK(d.warnings.size() == 1, "one warning for the line without '='");
      d = psp_ini_parse("[a]\nk = 1\nk = 2\n");
      CHECK(*d.get("a", "k") == "2", "repeated key keeps the last value");
      d = psp_ini_parse(slurp(kFix + "bom_crlf.ini"));
      CHECK(d.get("display", "render_scale") && *d.get("display", "render_scale") == "5",
            "BOM and CRLF are accepted");
      CHECK(*d.get("online", "nickname") == "\xC3\x91" "and\xC3\xBA", "UTF-8 value kept");
  }

  static void defaults_fixture() {
      const SettingsResolution r = psp_settings_resolve(slurp(kFix + "defaults.ini"), kCatalog,
                                                        "C:/Juegos/Patapon 3", kNoEnv);
      CHECK(val(r, "PSPRECOMP_UI_LANG") == "es", "language");
      CHECK(val(r, "PSPRECOMP_DISC0") == "C:/Juegos/Patapon 3/disc0", "disc0 resolved (spaces)");
      CHECK(val(r, "PSPRECOMP_SAVEDATA") == "C:/Juegos/Patapon 3/savedata", "savedata resolved");
      CHECK(val(r, "PSPRECOMP_FULLSCREEN") == "1", "fullscreen true -> 1");
      CHECK(val(r, "PSPRECOMP_WINDOW_SCALE") == "2", "window scale");
      CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "4", "render scale");
      CHECK(val(r, "PSPRECOMP_PRESENT_FILTER") == "linear", "present filter");
      CHECK(val(r, "PSPRECOMP_TEX_FILTER") == "auto", "texture filter");
      CHECK(val(r, "PSPRECOMP_TEXTURES") == "C:/Juegos/Patapon 3/textures/INFN00001", "pack on");
      CHECK(val(r, "PSPRECOMP_PATCHES") == "<unset>", "fps 30 -> no patch");
      CHECK(val(r, "PSPRECOMP_P3_ANIM_SMOOTH") == "1", "smooth on");
      CHECK(val(r, "PSPRECOMP_P3_FAST_LOAD") == "1", "fast load on");
      CHECK(val(r, "PSPRECOMP_ADHOC_SERVER") == "127.0.0.1", "server");
      CHECK(val(r, "PSPRECOMP_ADHOC_NICK") == "Patapon", "nick");
      CHECK(val(r, "PSPRECOMP_ADHOC_RELAY") == "<unset>", "relay off -> unset");
      CHECK(val(r, "PSPRECOMP_ADHOC_PORT_OFFSET") == "10000", "port offset");
      CHECK(val(r, "PSPRECOMP_ADHOC_MAC_FILE") == "C:/Juegos/Patapon 3/adhoc_mac.txt", "mac file");
  }

  static void custom_fixture() {
      const SettingsResolution r = psp_settings_resolve(slurp(kFix + "custom.ini"), kCatalog,
                                                        "D:/P3", kNoEnv);
      CHECK(val(r, "PSPRECOMP_UI_LANG") == "en", "en");
      CHECK(val(r, "PSPRECOMP_DISC0") == "D:/P3/../data/disc0", "relative with ..");
      CHECK(val(r, "PSPRECOMP_SAVEDATA") == "D:/P3/Partidas Guardadas", "spaces in value");
      CHECK(val(r, "PSPRECOMP_FULLSCREEN") == "<unset>", "fullscreen no -> unset");
      CHECK(val(r, "PSPRECOMP_TEXTURES") == "<unset>", "hd off -> unset");
      CHECK(val(r, "PSPRECOMP_PATCHES") == "60fps", "fps 60 -> patch");
      CHECK(val(r, "PSPRECOMP_P3_ANIM_SMOOTH") == "0", "smooth false -> 0");
      CHECK(val(r, "PSPRECOMP_P3_FAST_LOAD") == "1", "TRUE accepted");
      CHECK(val(r, "PSPRECOMP_ADHOC_RELAY") == "1", "yes -> 1");
      CHECK(val(r, "PSPRECOMP_ADHOC_PORT_OFFSET") == "0", "offset 0");
  }

  static void invalid_fixture() {
      const SettingsResolution r = psp_settings_resolve(slurp(kFix + "invalid.ini"), kCatalog,
                                                        "C:/P", kNoEnv);
      CHECK(val(r, "PSPRECOMP_FULLSCREEN") == "1", "bad bool -> default (true)");
      CHECK(logged(r, "display.fullscreen"), "bad bool logged");
      CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "3", "last duplicate (3) wins");
      CHECK(val(r, "PSPRECOMP_WINDOW_SCALE") == "2", "bad int -> default");
      CHECK(val(r, "PSPRECOMP_PRESENT_FILTER") == "linear", "bad enum -> default");
      CHECK(val(r, "PSPRECOMP_PATCHES") == "<unset>", "fps 45 -> default 30 -> unset");
      CHECK(logged(r, "game.fps"), "bad fps logged");
      CHECK(logged(r, "game.unknown_key"), "unknown key logged");
      CHECK(logged(r, "nosuchsection"), "unknown section logged");
      CHECK(val(r, "PSPRECOMP_ADHOC_NICK") == "Patapon", "empty nick -> default");
      CHECK(val(r, "PSPRECOMP_ADHOC_PORT_OFFSET") == "10000", "offset out of range -> default");
      CHECK(logged(r, "garbage line"), "ignored line logged");
  }

  static void env_precedence() {
      std::map<std::string, std::string> env = {{"PSPRECOMP_RENDER_SCALE", "1"},
                                                {"PSPRECOMP_PATCHES", "60fps"}};
      const EnvGetter get = [&](const char* n) -> const char* {
          auto it = env.find(n);
          return it == env.end() ? nullptr : it->second.c_str();
      };
      const SettingsResolution r = psp_settings_resolve(slurp(kFix + "defaults.ini"), kCatalog,
                                                        "C:/P", get);
      CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "<unset>", "env already set: not touched");
      CHECK(val(r, "PSPRECOMP_PATCHES") == "<unset>", "env PATCHES already set: not touched");
      CHECK(logged(r, "PSPRECOMP_RENDER_SCALE already set"), "precedence logged");
  }

  static void future_and_empty() {
      SettingsResolution r = psp_settings_resolve(slurp(kFix + "future.ini"), kCatalog, "C:/P", kNoEnv);
      CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "6", "known key of a future version applied");
      CHECK(logged(r, "version 2"), "future version warned");
      r = psp_settings_resolve("", "", "C:/P", kNoEnv);
      CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "4", "empty ini -> defaults");
      CHECK(val(r, "PSPRECOMP_PATCHES") == "<unset>", "no catalog -> no game keys");
      r = psp_settings_resolve("[display]\nrender_scale=2\n", "{not json", "C:/P", kNoEnv);
      CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "2", "bad catalog does not stop core keys");
      CHECK(logged(r, "game_options.json"), "bad catalog logged");
  }

  int main() {
      parser_rules();
      defaults_fixture();
      custom_fixture();
      invalid_fixture();
      env_precedence();
      future_and_empty();
      if (failures == 0) std::printf("test_settings: all passed\n");
      return failures == 0 ? 0 : 1;
  }
  ```
  Headers with the interfaces above and stub sources: `psp_ini_parse` returns `IniDoc{}`,
  `IniDoc::get` returns `nullptr`, `psp_settings_resolve` returns `SettingsResolution{}`.
  CMake, after the `ge_rtt_tests` block:
  ```cmake
  # settings.ini v1 (docs/contracts/settings-v1.md): INI parser + key resolution. Pure.
  add_executable(test_settings tests/test_settings.cpp src/psp_ini.cpp src/psp_settings.cpp)
  target_include_directories(test_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                             ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  target_compile_definitions(test_settings PRIVATE
      PSP_CONTRACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../docs/contracts"
      PSP_GAME_OPTIONS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/game_options.json")
  target_compile_options(test_settings PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
  add_test(NAME settings_tests COMMAND test_settings)
  ```
  Add `src/psp_ini.cpp` and `src/psp_settings.cpp` to `RUNTIME_SOURCES` (after
  `src/psp_ge_rtt.cpp`) and `${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann` to the runtime's
  include directories (next to the other `third_party` includes; grep `third_party/stb`).
- [ ] **Step 3: Run:** `cmake --build DECO/build/rtw --target test_settings && DECO/build/rtw/test_settings.exe`.
  Expected: `FAIL:` lines for every check that expects a value or a log line, exit 1 (the
  `<unset>` checks pass against the stubs, which set nothing).
- [ ] **Step 4: Implement `psp_ini.cpp`:**
  ```cpp
  #include "psp_ini.h"

  #include <algorithm>
  #include <cctype>

  namespace {
  std::string trim(const std::string& s) {
      size_t a = 0, b = s.size();
      while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
      while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
      return s.substr(a, b - a);
  }
  std::string lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return s;
  }
  }  // namespace

  const std::string* IniDoc::get(const std::string& section, const std::string& key) const {
      auto s = values.find(section);
      if (s == values.end()) return nullptr;
      auto k = s->second.find(key);
      return k == s->second.end() ? nullptr : &k->second;
  }

  IniDoc psp_ini_parse(const std::string& text) {
      IniDoc d;
      std::string body = text;
      if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
          static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
          body.erase(0, 3);
      std::string section;
      size_t pos = 0;
      while (pos <= body.size()) {
          size_t nl = body.find('\n', pos);
          if (nl == std::string::npos) nl = body.size();
          const std::string line = trim(body.substr(pos, nl - pos));
          pos = nl + 1;
          if (line.empty() || line[0] == ';' || line[0] == '#') continue;
          if (line.front() == '[' && line.back() == ']') {
              section = lower(trim(line.substr(1, line.size() - 2)));
              d.values[section];
              continue;
          }
          const size_t eq = line.find('=');
          if (eq == std::string::npos) {
              d.warnings.push_back("ignored line \"" + line + "\"");
              continue;
          }
          d.values[section][lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
      }
      return d;
  }
  ```
- [ ] **Step 5: Implement `psp_settings.cpp` (`psp_settings_resolve`):**
  ```cpp
  #include "psp_settings.h"
  #include "psp_ini.h"

  #include "json.hpp"

  #include <algorithm>
  #include <cctype>
  #include <cstdlib>
  #include <set>

  namespace {

  enum class Kind { Bool, Int, Enum, String, Path };

  struct CoreKey {
      const char* section;
      const char* key;
      const char* env;
      Kind kind;
      const char* def;
      int min, max;          // Int
      const char* values;    // Enum: comma list
  };

  // docs/contracts/settings-v1.md, core key table. display.texture_pack is handled with
  // display.hd_textures.
  const CoreKey kCore[] = {
      {"general", "language", "PSPRECOMP_UI_LANG", Kind::Enum, "es", 0, 0, "es,en"},
      {"paths", "disc0", "PSPRECOMP_DISC0", Kind::Path, "disc0", 0, 0, nullptr},
      {"paths", "savedata", "PSPRECOMP_SAVEDATA", Kind::Path, "savedata", 0, 0, nullptr},
      {"display", "fullscreen", "PSPRECOMP_FULLSCREEN", Kind::Bool, "true", 0, 0, nullptr},
      {"display", "window_scale", "PSPRECOMP_WINDOW_SCALE", Kind::Int, "2", 1, 8, nullptr},
      {"display", "render_scale", "PSPRECOMP_RENDER_SCALE", Kind::Int, "4", 1, 8, nullptr},
      {"display", "present_filter", "PSPRECOMP_PRESENT_FILTER", Kind::Enum, "linear", 0, 0, "linear,nearest"},
      {"display", "texture_filter", "PSPRECOMP_TEX_FILTER", Kind::Enum, "auto", 0, 0, "auto,nearest,linear"},
      {"display", "hd_textures", "PSPRECOMP_TEXTURES", Kind::Bool, "true", 0, 0, nullptr},
      {"display", "texture_pack", nullptr, Kind::Path, "textures/INFN00001", 0, 0, nullptr},
      {"online", "server", "PSPRECOMP_ADHOC_SERVER", Kind::String, "127.0.0.1", 0, 0, nullptr},
      {"online", "nickname", "PSPRECOMP_ADHOC_NICK", Kind::String, "Patapon", 0, 0, nullptr},
      {"online", "relay", "PSPRECOMP_ADHOC_RELAY", Kind::Bool, "false", 0, 0, nullptr},
      {"online", "port_offset", "PSPRECOMP_ADHOC_PORT_OFFSET", Kind::Int, "10000", 0, 65535, nullptr},
      {"online", "mac_file", "PSPRECOMP_ADHOC_MAC_FILE", Kind::Path, "adhoc_mac.txt", 0, 0, nullptr},
  };

  std::string lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return s;
  }

  // "true"/"false" or "" when not a boolean.
  std::string parse_bool(const std::string& v) {
      const std::string l = lower(v);
      if (l == "true" || l == "1" || l == "yes" || l == "on") return "true";
      if (l == "false" || l == "0" || l == "no" || l == "off") return "false";
      return "";
  }

  bool parse_int(const std::string& v, int min, int max, std::string* out) {
      if (v.empty()) return false;
      size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
      if (i == v.size()) return false;
      for (size_t k = i; k < v.size(); k++) if (!std::isdigit(static_cast<unsigned char>(v[k]))) return false;
      if (v.size() > 7) return false;
      const long n = std::strtol(v.c_str(), nullptr, 10);
      if (n < min || n > max) return false;
      *out = std::to_string(n);
      return true;
  }

  bool in_list(const std::string& v, const std::string& list) {
      size_t a = 0;
      while (a <= list.size()) {
          size_t c = list.find(',', a);
          if (c == std::string::npos) c = list.size();
          if (list.compare(a, c - a, v) == 0) return true;
          a = c + 1;
      }
      return false;
  }

  bool is_absolute(const std::string& p) {
      return (!p.empty() && (p[0] == '/' || p[0] == '\\')) ||
             (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':');
  }

  std::string resolve_path(const std::string& base, const std::string& p) {
      if (is_absolute(p) || base.empty()) return p;
      return base + "/" + p;
  }

  // Validates `raw` for a key of kind/def/min/max/values; returns the normalized value or the
  // default, logging a line when it falls back.
  std::string validate(const std::string& where, const std::string* raw, Kind kind,
                       const std::string& def, int min, int max, const std::string& values,
                       std::vector<std::string>* log) {
      if (raw == nullptr) return def;
      std::string out;
      switch (kind) {
      case Kind::Bool:
          out = parse_bool(*raw);
          if (!out.empty()) return out;
          break;
      case Kind::Int:
          if (parse_int(*raw, min, max, &out)) return out;
          break;
      case Kind::Enum:
          if (in_list(*raw, values)) return *raw;
          break;
      case Kind::String:
      case Kind::Path:
          if (!raw->empty()) return *raw;
          break;
      }
      log->push_back("[SETTINGS] " + where + ": invalid value \"" + *raw + "\", using \"" + def + "\"");
      return def;
  }

  }  // namespace

  SettingsResolution psp_settings_resolve(const std::string& ini_text, const std::string& catalog_json,
                                          const std::string& base_dir, const EnvGetter& getenv_fn) {
      SettingsResolution r;
      const IniDoc ini = psp_ini_parse(ini_text);
      for (const std::string& w : ini.warnings) r.log.push_back("[SETTINGS] " + w);
      if (const std::string* v = ini.get("meta", "version")) {
          if (*v != "1") r.log.push_back("[SETTINGS] settings.ini version " + *v +
                                         " is newer than 1: applying the keys this build knows");
      }
      std::set<std::string> known = {"meta.version"};
      auto emit = [&](const char* env, const std::string& value, bool unset) {
          if (getenv_fn(env) != nullptr) {
              r.log.push_back(std::string("[SETTINGS] ") + env + " already set in the environment, kept");
              return;
          }
          if (!unset) r.set.emplace_back(env, value);
      };

      std::string hd, pack;
      for (const CoreKey& k : kCore) {
          known.insert(std::string(k.section) + "." + k.key);
          const std::string where = std::string(k.section) + "." + k.key;
          const std::string v = validate(where, ini.get(k.section, k.key), k.kind, k.def, k.min, k.max,
                                         k.values ? k.values : "", &r.log);
          if (std::string(k.key) == "hd_textures") { hd = v; continue; }
          if (std::string(k.key) == "texture_pack") { pack = v; continue; }
          if (k.kind == Kind::Bool) emit(k.env, "1", v != "true");
          else if (k.kind == Kind::Path) emit(k.env, resolve_path(base_dir, v), false);
          else emit(k.env, v, false);
      }
      emit("PSPRECOMP_TEXTURES", resolve_path(base_dir, pack), hd != "true");

      if (!catalog_json.empty()) {
          nlohmann::json cat = nlohmann::json::parse(catalog_json, nullptr, false);
          if (cat.is_discarded() || !cat.contains("options") || !cat["options"].is_array()) {
              r.log.push_back("[SETTINGS] game_options.json is not valid JSON: game options skipped");
          } else {
              for (const auto& o : cat["options"]) {
                  const std::string section = o.value("section", ""), key = o.value("key", "");
                  const std::string type = o.value("type", "string"), def = o.value("default", "");
                  known.insert(section + "." + key);
                  const Kind kind = type == "bool" ? Kind::Bool : type == "int" ? Kind::Int
                                  : type == "enum" ? Kind::Enum : type == "path" ? Kind::Path : Kind::String;
                  std::string values;
                  if (o.contains("values") && o["values"].is_array())
                      for (const auto& x : o["values"]) values += (values.empty() ? "" : ",") + x.get<std::string>();
                  const std::string v = validate(section + "." + key, ini.get(section, key), kind, def,
                                                 o.value("min", 0), o.value("max", 0), values, &r.log);
                  const std::string env = o.value("env", "");
                  if (env.empty()) continue;
                  if (o.contains("map") && o["map"].is_object()) {
                      const auto& m = o["map"];
                      if (!m.contains(v) || m[v].is_null()) { emit(env.c_str(), "", true); continue; }
                      emit(env.c_str(), m[v].get<std::string>(), false);
                  } else {
                      emit(env.c_str(), kind == Kind::Path ? resolve_path(base_dir, v) : v, false);
                  }
              }
          }
      }

      for (const auto& s : ini.values) {
          for (const auto& kv : s.second) {
              const std::string id = s.first + "." + kv.first;
              if (!known.count(id)) r.log.push_back("[SETTINGS] unknown key " + id + " ignored");
          }
          if (s.second.empty() && s.first != "controls")
              r.log.push_back("[SETTINGS] empty section [" + s.first + "]");
      }
      return r;
  }
  ```
  Note for the reviewer: `[controls]` is reserved and empty in v1; any key in it is logged as
  unknown, which is the contract.
- [ ] **Step 6: Run** the test again. Expected: `test_settings: all passed`. Then the whole build
  (no new warnings in `runtime/`) and `ctest --test-dir DECO/build/rtw`: 72/72.
- [ ] **Step 7: Commit** `feat(runtime): settings.ini v1 parser and key resolution (pure)` with
  `Verified: test_settings RED -> GREEN; ctest 72/72` and `Agent:`.

### Task 3: Game — apply settings at startup, `--settings`, `--log`, catalog next to the exe

**Files:**
- Modify: `runtime/include/psp_settings.h`, `runtime/src/psp_settings.cpp` (impure apply)
- Modify: `runtime/src/main.cpp` (first lines of `main`)
- Modify: `runtime/CMakeLists.txt` (copy `game_options.json` next to the exe)
- Modify: `runtime/tests/test_settings.cpp` (argument parsing)
- Modify: `docs/ENV_FLAGS.md`

**Interfaces:**
- Consumes: Task 2 `psp_settings_resolve`.
- Produces:
  ```cpp
  struct SettingsArgs { std::string settings_path, log_path; };   // "" = not given
  SettingsArgs psp_settings_parse_args(int argc, char** argv);      // pure
  // Redirects stderr to args.log_path when given, finds settings.ini (args path, else next to
  // the executable), resolves it with game_options.json next to the executable and sets the
  // environment. No ini and no --settings: does nothing.
  void psp_settings_apply(const SettingsArgs& args, const char* exe_dir);
  ```

- [ ] **Step 1: Failing test** (append to `test_settings.cpp` before `main`, call it from `main`):
  ```cpp
  static void args_rules() {
      char a0[] = "game.exe", a1[] = "--settings", a2[] = "C:/P 3/settings.ini", a3[] = "--log",
           a4[] = "C:/P 3/logs/game.log", a5[] = "--unknown";
      char* argv[] = {a0, a1, a2, a3, a4, a5};
      SettingsArgs s = psp_settings_parse_args(6, argv);
      CHECK(s.settings_path == "C:/P 3/settings.ini", "--settings path (spaces)");
      CHECK(s.log_path == "C:/P 3/logs/game.log", "--log path");
      char* only[] = {a0, a1};
      s = psp_settings_parse_args(2, only);
      CHECK(s.settings_path.empty(), "--settings without a value is ignored");
  }
  ```
  Add the declarations, a stub returning `SettingsArgs{}`, run: expected the two path FAILs.
- [ ] **Step 2: Implement** in `psp_settings.cpp`:
  ```cpp
  SettingsArgs psp_settings_parse_args(int argc, char** argv) {
      SettingsArgs a;
      for (int i = 1; i + 1 < argc; i++) {
          const std::string k = argv[i];
          if (k == "--settings") a.settings_path = argv[++i];
          else if (k == "--log") a.log_path = argv[++i];
      }
      return a;
  }

  namespace {
  std::string read_file(const std::string& path, bool* ok) {
      FILE* f = std::fopen(path.c_str(), "rb");
      *ok = f != nullptr;
      if (!f) return "";
      std::string s;
      char buf[4096];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
      std::fclose(f);
      return s;
  }
  std::string dir_of(const std::string& path) {
      const size_t p = path.find_last_of("/\\");
      return p == std::string::npos ? "." : path.substr(0, p);
  }
  std::string slashes(std::string s) {
      std::replace(s.begin(), s.end(), '\\', '/');
      return s;
  }
  void set_env(const std::string& name, const std::string& value) {
  #ifdef _WIN32
      _putenv_s(name.c_str(), value.c_str());
  #else
      setenv(name.c_str(), value.c_str(), 1);
  #endif
  }
  }  // namespace

  void psp_settings_apply(const SettingsArgs& args, const char* exe_dir) {
      if (!args.log_path.empty()) {
          if (std::freopen(args.log_path.c_str(), "w", stderr) == nullptr)
              std::fprintf(stdout, "[SETTINGS] cannot open log %s\n", args.log_path.c_str());
      }
      const std::string exe = slashes(exe_dir ? exe_dir : "");
      std::string ini_path = slashes(args.settings_path);
      bool ok = false;
      std::string text;
      if (!ini_path.empty()) {
          text = read_file(ini_path, &ok);
          if (!ok) {
              std::fprintf(stderr, "[SETTINGS] %s not found: using the runtime defaults\n", ini_path.c_str());
              return;
          }
      } else {
          ini_path = exe + "/settings.ini";
          text = read_file(ini_path, &ok);
          if (!ok) return;  // no settings.ini: everything as before
      }
      bool cat_ok = false;
      const std::string catalog = read_file(exe + "/game_options.json", &cat_ok);
      std::fprintf(stderr, "[SETTINGS] reading %s (game options: %s)\n", ini_path.c_str(),
                   cat_ok ? "game_options.json" : "none");
      const SettingsResolution r = psp_settings_resolve(text, catalog, dir_of(ini_path),
                                                        [](const char* n) { return std::getenv(n); });
      for (const std::string& l : r.log) std::fprintf(stderr, "%s\n", l.c_str());
      for (const auto& kv : r.set) {
          set_env(kv.first, kv.second);
          std::fprintf(stderr, "[SETTINGS] %s=%s\n", kv.first.c_str(), kv.second.c_str());
      }
  }
  ```
  (add `#include <cstdio>`, `<cstdlib>`.) Run the test: expected `test_settings: all passed`.
- [ ] **Step 3: Call it first in `main`.** In `main.cpp` replace `(void)argc; (void)argv;` with:
  ```cpp
      // settings.ini (docs/contracts/settings-v1.md): before anything reads a PSPRECOMP_* variable.
      {
          char* base = SDL_GetBasePath();
          psp_settings_apply(psp_settings_parse_args(argc, argv), base);
          SDL_free(base);
      }
  ```
  and add `#include "psp_settings.h"` (and `<SDL.h>` if not already included; grep first).
- [ ] **Step 4: Catalog next to the exe.** In `runtime/CMakeLists.txt`, after
  `add_executable(psprecomp_runtime ...)`:
  ```cmake
  # Game option catalog for settings.ini [game] (docs/contracts/settings-v1.md).
  set(_game_options "${CMAKE_CURRENT_SOURCE_DIR}/../games/${PSPRECOMP_GAME}/game_options.json")
  if(EXISTS "${_game_options}")
      add_custom_command(TARGET psprecomp_runtime POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_game_options}"
                  "$<TARGET_FILE_DIR:psprecomp_runtime>/game_options.json")
  endif()
  ```
- [ ] **Step 5: Build and gates.** Port 9999 check; `cmake --build DECO/build/rtw` (no new
  warnings); `ctest`: 72/72; `ls DECO/build/rtw/game_options.json` exists.
  Manual check with a test instance (no game needed past boot):
  `mkdir -p "DECO/build/settings_check/a b" && cp psprecomp/docs/contracts/fixtures/custom.ini "DECO/build/settings_check/a b/settings.ini"`,
  then `cd DECO/build/rtw && PSPRECOMP_DEBUG_PORT=9998 timeout 20 ./psprecomp_runtime.exe --settings "../settings_check/a b/settings.ini" --log "../settings_check/a b/game.log"`
  (it stops at the missing disc0 of `custom.ini` or after 20 s); then
  `grep "\[SETTINGS\]" "DECO/build/settings_check/a b/game.log"`. Expected: the `reading` line,
  `PSPRECOMP_PATCHES=60fps`, `PSPRECOMP_SAVEDATA=.../a b/Partidas Guardadas`, no crash before the
  lines. Then confirm `jugar.bat`'s path is untouched: `ls DECO/build/rtw/settings.ini` must not
  exist.
- [ ] **Step 6: Regression.** `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/settings_t3`
  then `python -I build/tools/rt_regress.py build/tour/fastload2 build/tour/settings_t3`.
  Expected `RT_REGRESS PASS` (no settings.ini next to the exe: nothing changes).
- [ ] **Step 7: Docs.** `docs/ENV_FLAGS.md`: a section "Command line and settings.ini" describing
  `--settings`, `--log`, the precedence and pointing to `docs/contracts/settings-v1.md`; a
  `PSPRECOMP_UI_LANG` entry (es|en, language of in-game runtime dialogs, default es).
- [ ] **Step 8: Commit** `feat(runtime): read settings.ini at startup (--settings, --log)` with
  `Verified:` (test RED -> GREEN, ctest 72/72, manual [SETTINGS] lines, rt_regress PASS) and `Agent:`.

### Task 4: Game — back to the menu (F1 / Select+Start 1 s, Yes/No prompt, exit code 10)

**Files:**
- Create: `runtime/include/psp_menu_prompt.h`, `runtime/src/psp_menu_prompt.cpp`
- Create: `runtime/tests/test_menu_prompt.cpp`
- Modify: `runtime/include/psp_exit.h`, `runtime/src/main.cpp` (quick_exit code)
- Modify: `runtime/src/psp_event_loop.cpp`, `runtime/src/psp_osk_overlay.cpp`,
  `runtime/include/psp_osk_overlay.h`, `runtime/src/psp_ge_draw.cpp`, `runtime/CMakeLists.txt`
- Modify: `docs/ENV_FLAGS.md`, `README.md`, `ARCHITECTURE.md`

**Interfaces:**
- Produces:
  ```cpp
  // psp_menu_prompt.h (pure)
  class ComboHold {            // true once when held >= hold_ms; re-arms on release
  public:
      explicit ComboHold(int64_t hold_ms) : hold_ms_(hold_ms) {}
      bool update(bool held, int64_t now_ms);
  private:
      int64_t hold_ms_, since_ = -1;
      bool fired_ = false;
  };
  enum class PromptAction { Left, Right, Up, Down, Confirm, Cancel };
  enum class PromptResult { None, Confirmed, Cancelled };
  class MenuPrompt {
  public:
      void open();                       // selection starts on "No"
      bool active() const;
      bool yes_selected() const;
      PromptResult handle(PromptAction a);
  private:
      bool active_ = false, yes_ = false;
  };
  struct PromptText { const char* question; const char* yes; const char* no; const char* hint; };
  PromptText menu_prompt_text(const char* lang);   // "en" -> English, anything else Spanish
  // psp_exit.h
  inline std::atomic<int>& psp_requested_exit_code();  // 0 by default
  // psp_osk_overlay.h
  bool psp_menu_prompt_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh);
  MenuPrompt& psp_menu_prompt();     // process-wide, main thread only
  ```

- [ ] **Step 1: Failing tests** `runtime/tests/test_menu_prompt.cpp`:
  ```cpp
  // Back-to-menu combo and prompt (psp_menu_prompt.h): Select+Start held 1 s opens a Yes/No
  // prompt that starts on "No".
  #include "psp_menu_prompt.h"

  #include <cstdio>
  #include <cstring>

  static int failures = 0;
  #define CHECK(cond, msg) \
      do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

  static void combo() {
      ComboHold c(1000);
      CHECK(!c.update(true, 0), "press starts the clock");
      CHECK(!c.update(true, 999), "999 ms: not yet");
      CHECK(c.update(true, 1000), "1000 ms: fires");
      CHECK(!c.update(true, 3000), "fires once while held");
      CHECK(!c.update(false, 3100), "release");
      CHECK(!c.update(true, 3200), "press again");
      CHECK(!c.update(false, 3800), "tap released at 600 ms: never fires");
      CHECK(!c.update(true, 4000) && c.update(true, 5000), "re-armed after release");
  }

  static void prompt() {
      MenuPrompt p;
      CHECK(!p.active(), "closed by default");
      CHECK(p.handle(PromptAction::Confirm) == PromptResult::None, "input while closed ignored");
      p.open();
      CHECK(p.active() && !p.yes_selected(), "opens on No");
      CHECK(p.handle(PromptAction::Confirm) == PromptResult::Cancelled && !p.active(),
            "confirm on No = cancel, closes");
      p.open();
      p.handle(PromptAction::Left);
      CHECK(p.yes_selected(), "left moves to Yes");
      p.handle(PromptAction::Right);
      CHECK(!p.yes_selected(), "right moves to No");
      p.handle(PromptAction::Up);
      CHECK(p.yes_selected(), "up toggles too");
      CHECK(p.handle(PromptAction::Confirm) == PromptResult::Confirmed && !p.active(), "confirm on Yes");
      p.open();
      p.handle(PromptAction::Left);
      CHECK(p.handle(PromptAction::Cancel) == PromptResult::Cancelled, "cancel always cancels");
  }

  static void texts() {
      CHECK(std::strcmp(menu_prompt_text("en").yes, "Yes") == 0, "English");
      CHECK(std::strcmp(menu_prompt_text("es").yes, "Sí") == 0, "Spanish");
      CHECK(std::strcmp(menu_prompt_text(nullptr).yes, "Sí") == 0, "default Spanish");
  }

  int main() {
      combo();
      prompt();
      texts();
      if (failures == 0) std::printf("test_menu_prompt: all passed\n");
      return failures == 0 ? 0 : 1;
  }
  ```
  Header as above, stubs (`update` returns false, `handle` returns None, `open` does nothing,
  `menu_prompt_text` returns `{"", "", "", ""}`). CMake:
  ```cmake
  # Back-to-menu combo + prompt (pure).
  add_executable(test_menu_prompt tests/test_menu_prompt.cpp src/psp_menu_prompt.cpp)
  target_include_directories(test_menu_prompt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(test_menu_prompt PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
  add_test(NAME menu_prompt_tests COMMAND test_menu_prompt)
  ```
  and `src/psp_menu_prompt.cpp` in `RUNTIME_SOURCES`. Run: expected FAIL lines, exit 1.
- [ ] **Step 2: Implement `psp_menu_prompt.cpp`:**
  ```cpp
  #include "psp_menu_prompt.h"

  #include <cstring>

  bool ComboHold::update(bool held, int64_t now_ms) {
      if (!held) { since_ = -1; fired_ = false; return false; }
      if (since_ < 0) since_ = now_ms;
      if (!fired_ && now_ms - since_ >= hold_ms_) { fired_ = true; return true; }
      return false;
  }

  void MenuPrompt::open() { active_ = true; yes_ = false; }
  bool MenuPrompt::active() const { return active_; }
  bool MenuPrompt::yes_selected() const { return yes_; }

  PromptResult MenuPrompt::handle(PromptAction a) {
      if (!active_) return PromptResult::None;
      switch (a) {
      case PromptAction::Left: yes_ = true; return PromptResult::None;
      case PromptAction::Right: yes_ = false; return PromptResult::None;
      case PromptAction::Up:
      case PromptAction::Down: yes_ = !yes_; return PromptResult::None;
      case PromptAction::Confirm:
          active_ = false;
          return yes_ ? PromptResult::Confirmed : PromptResult::Cancelled;
      case PromptAction::Cancel:
          active_ = false;
          return PromptResult::Cancelled;
      }
      return PromptResult::None;
  }

  PromptText menu_prompt_text(const char* lang) {
      if (lang && std::strcmp(lang, "en") == 0)
          return {"Back to the menu? Unsaved progress is lost.", "Yes", "No",
                  "Arrows: choose    Enter / A: confirm    Esc / B: back"};
      return {"¿Volver al menú? Se perderá lo no guardado.", "Sí", "No",
              "Flechas: elegir    Enter / A: confirmar    Esc / B: volver"};
  }
  ```
  Run: expected `test_menu_prompt: all passed`.
- [ ] **Step 3: Exit code.** In `psp_exit.h` add `#include <atomic>` and
  ```cpp
  // Process exit code main() ends with: 0 = quit, 10 = back to the launcher menu
  // (docs/contracts/settings-v1.md).
  inline std::atomic<int>& psp_requested_exit_code() {
      static std::atomic<int> code{0};
      return code;
  }
  ```
  In `main.cpp` replace `std::fprintf(stderr, "[RT] Exit 0\n");` / `std::quick_exit(0);` with
  ```cpp
      const int exit_code = psp_requested_exit_code().load();
      std::fprintf(stderr, "[RT] Exit %d\n", exit_code);
      std::fflush(nullptr);
      std::quick_exit(exit_code);  // runs the psp_on_exit diagnostics (psp_exit.h), then _Exit
  ```
- [ ] **Step 4: Prompt drawing.** In `psp_osk_overlay.cpp` add, after `psp_osk_overlay_draw`
  (reusing `update_text`, `rect`, `gl()` from the same file):
  ```cpp
  MenuPrompt& psp_menu_prompt() {
      static MenuPrompt p;
      return p;
  }

  bool psp_menu_prompt_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh) {
      MenuPrompt& p = psp_menu_prompt();
      if (!p.active() || win_w <= 0 || win_h <= 0 || rw <= 0 || rh <= 0) return false;
      static TextTex question, yes, no, hint;
      const PromptText t = menu_prompt_text(std::getenv("PSPRECOMP_UI_LANG"));
      const float gx = static_cast<float>(rx), gy = static_cast<float>(win_h - (ry + rh));
      const float pw = rw * 0.70f, ph = rh * 0.34f;
      const float px = gx + (rw - pw) * 0.5f, py = gy + (rh - ph) * 0.5f, pad = rh * 0.035f;
      update_text(question, t.question, static_cast<int>(rh * 0.060f));
      update_text(yes, t.yes, static_cast<int>(rh * 0.070f));
      update_text(no, t.no, static_cast<int>(rh * 0.070f));
      update_text(hint, t.hint, static_cast<int>(rh * 0.040f));
      Gl& g = gl();
      glUseProgram(g.prog);
      glBindVertexArray(g.vao);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_STENCIL_TEST);
      glDisable(GL_CULL_FACE);
      glDisable(GL_SCISSOR_TEST);
      glEnable(GL_BLEND);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      glBlendEquation(GL_FUNC_ADD);
      const float shade[4] = {0.0f, 0.0f, 0.0f, 0.55f}, panel[4] = {0.07f, 0.06f, 0.10f, 0.95f};
      const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f}, grey[4] = {0.75f, 0.75f, 0.80f, 1.0f};
      const float accent[4] = {1.0f, 0.82f, 0.30f, 1.0f}, sel[4] = {1.0f, 0.82f, 0.30f, 0.25f};
      rect(win_w, win_h, gx, gy, static_cast<float>(rw), static_cast<float>(rh), shade, 0);
      rect(win_w, win_h, px, py, pw, ph, panel, 0);
      rect(win_w, win_h, px, py, pw, rh * 0.008f, accent, 0);
      float y = py + pad;
      rect(win_w, win_h, px + (pw - question.w) * 0.5f, y, static_cast<float>(question.w),
           static_cast<float>(question.h), white, question.tex);
      y += question.h + pad;
      const float bw = pw * 0.30f, bh = yes.h + pad * 0.6f;
      const float yx = px + pw * 0.5f - bw - pad, nx = px + pw * 0.5f + pad;
      rect(win_w, win_h, p.yes_selected() ? yx : nx, y, bw, bh, sel, 0);
      rect(win_w, win_h, yx + (bw - yes.w) * 0.5f, y + pad * 0.3f, static_cast<float>(yes.w),
           static_cast<float>(yes.h), p.yes_selected() ? accent : white, yes.tex);
      rect(win_w, win_h, nx + (bw - no.w) * 0.5f, y + pad * 0.3f, static_cast<float>(no.w),
           static_cast<float>(no.h), p.yes_selected() ? white : accent, no.tex);
      y += bh + pad;
      rect(win_w, win_h, px + (pw - hint.w) * 0.5f, y, static_cast<float>(hint.w),
           static_cast<float>(hint.h), grey, hint.tex);
      glDisable(GL_BLEND);
      glBindVertexArray(0);
      glUseProgram(0);
      return true;
  }
  ```
  Declare both in `psp_osk_overlay.h` (include `psp_menu_prompt.h`). In `psp_ge_draw.cpp`
  `present_blit`, change the overlay call to
  `if (psp_osk_overlay_draw(...) | psp_menu_prompt_overlay_draw(win_w, win_h, r.x, r.y, r.w, r.h)) {`
  (bitwise `|` so both run).
- [ ] **Step 5: Input.** In `psp_event_loop.cpp`:
  ```cpp
  // Back to the launcher menu: F1, or Select+Start held 1 s on a gamepad (the Guide button
  // too, when SDL delivers it) opens a Yes/No prompt; while it is open the game gets no input.
  // Yes ends the process with exit code 10 (docs/contracts/settings-v1.md).
  static ComboHold g_menu_combo(1000);

  static void open_menu_prompt() {
      if (osk_session().active() || psp_menu_prompt().active()) return;
      psp_menu_prompt().open();
      g_host_buttons.store(0, std::memory_order_relaxed);
  }

  static void finish_menu_prompt(PromptResult r) {
      if (r != PromptResult::Confirmed) return;
      psp_requested_exit_code().store(10);
      g_should_exit.store(true);
  }

  // Returns true when the event was consumed by the prompt (or opened it).
  static bool handle_menu_prompt_event(const SDL_Event& ev) {
      MenuPrompt& p = psp_menu_prompt();
      if (!p.active()) {
          if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym == SDLK_F1) {
              open_menu_prompt();
              return true;
          }
          if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
              open_menu_prompt();
              return true;
          }
          return false;
      }
      if (ev.type == SDL_KEYDOWN) {
          switch (ev.key.keysym.sym) {
          case SDLK_LEFT: finish_menu_prompt(p.handle(PromptAction::Left)); break;
          case SDLK_RIGHT: finish_menu_prompt(p.handle(PromptAction::Right)); break;
          case SDLK_UP: finish_menu_prompt(p.handle(PromptAction::Up)); break;
          case SDLK_DOWN: finish_menu_prompt(p.handle(PromptAction::Down)); break;
          case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x:
              finish_menu_prompt(p.handle(PromptAction::Confirm)); break;
          case SDLK_ESCAPE: case SDLK_F1: case SDLK_z:
              finish_menu_prompt(p.handle(PromptAction::Cancel)); break;
          default: break;
          }
          return true;
      }
      if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
          switch (ev.cbutton.button) {
          case SDL_CONTROLLER_BUTTON_DPAD_LEFT: finish_menu_prompt(p.handle(PromptAction::Left)); break;
          case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: finish_menu_prompt(p.handle(PromptAction::Right)); break;
          case SDL_CONTROLLER_BUTTON_DPAD_UP: finish_menu_prompt(p.handle(PromptAction::Up)); break;
          case SDL_CONTROLLER_BUTTON_DPAD_DOWN: finish_menu_prompt(p.handle(PromptAction::Down)); break;
          case SDL_CONTROLLER_BUTTON_A: finish_menu_prompt(p.handle(PromptAction::Confirm)); break;
          case SDL_CONTROLLER_BUTTON_B: finish_menu_prompt(p.handle(PromptAction::Cancel)); break;
          default: break;
          }
          return true;
      }
      return ev.type == SDL_KEYUP || ev.type == SDL_CONTROLLERBUTTONUP || ev.type == SDL_TEXTINPUT;
  }
  ```
  In `handle_sdl_event`, first line: `if (handle_menu_prompt_event(ev)) return;`. In
  `publish_pads`, store `0` buttons while `psp_menu_prompt().active()`:
  `g_pad_buttons.store(psp_menu_prompt().active() ? 0u : g_pads.buttons(), ...)`. Also call
  `publish_pads()` right after `psp_menu_prompt().open()` in `open_menu_prompt` and after a
  prompt closes (in `finish_menu_prompt`, before the early return). In `psp_event_loop`, after
  `publish_wheel();`:
  ```cpp
          // Select (0x0001) + Start (0x0008) held on any pad for 1 s.
          if (g_menu_combo.update((g_pads.buttons() & 0x0009u) == 0x0009u, steady_ms()))
              open_menu_prompt();
  ```
  Includes: `psp_menu_prompt.h`, `psp_osk_overlay.h`, `psp_exit.h`.
- [ ] **Step 6: Build and gates.** Port 9999 check, build (no new warnings), `ctest`: 73/73.
  The prompt itself (F1, the gamepad combo, Yes/No, exit 10) needs real keyboard and gamepad
  input, which the debug socket does not inject (its `B` command sets PSP buttons, not SDL
  events); it is checked by hand in Task 12 Step 5. Here verify the rest:
  `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/menu_t4`,
  then `python -I build/tools/rt_regress.py build/tour/settings_t3 build/tour/menu_t4`: PASS, and
  `grep "\[RT\] Exit 0" build/logs/tour_rt.log` finds the line (a normal close still exits 0).
- [ ] **Step 7: Docs.** `docs/ENV_FLAGS.md` "Command line and settings.ini" section: F1 /
  Select+Start 1 s / Guide, exit codes 0/10. `README.md`: one paragraph "Launcher (in progress)"
  pointing to the spec. `ARCHITECTURE.md`: a row "Settings / launcher contract" (`psp_settings`,
  `psp_ini`, `psp_menu_prompt`, exit code 10, `docs/contracts/settings-v1.md`).
- [ ] **Step 8: Commit** `feat(runtime): back to the launcher menu (F1, Select+Start 1 s, exit 10)`
  with `Verified:` (test_menu_prompt RED -> GREEN, ctest 73/73, tour + rt_regress PASS, Exit 0 on
  a normal close) and `Agent:`.

### Task 5: Launcher — scaffold (Tauri 2 + React 19 + TypeScript 7.0.2 + Vitest)

**Files:** (all under `DECO/launcher`) `package.json`, `tsconfig.json`, `vite.config.ts`,
`index.html`, `.gitignore`, `src/main.tsx`, `src/App.tsx`, `src/App.test.tsx`,
`src/setupTests.ts`, `src-tauri/{Cargo.toml,build.rs,tauri.conf.json,capabilities/default.json,src/main.rs,src/lib.rs}`.

**Interfaces:**
- Produces: `npm test` (Vitest), `npm run typecheck` (tsc --noEmit), `npm run tauri dev`,
  `cargo test --manifest-path src-tauri/Cargo.toml`; Rust crate name `p3_launcher` (lib) with
  `pub fn run()`; window label `main`.

- [ ] **Step 1: Pin versions.** Run `npm view @tauri-apps/cli version`, `npm view @tauri-apps/api version`,
  `npm view react version`, `npm view react-dom version`, `npm view vite version`,
  `npm view @vitejs/plugin-react version`, `npm view vitest version`, `npm view jsdom version`,
  `npm view @testing-library/react version`, `npm view @testing-library/user-event version`,
  `npm view @types/react version`, `npm view @types/react-dom version`, `npm view typescript@7.0.2 version`.
  Record the printed versions; they are the exact pins below (`"x.y.z"`, no `^`). If
  `typescript@7.0.2` does not exist, stop and tell the user (spec requires it).
- [ ] **Step 2: Files.** `package.json`:
  ```json
  {
    "name": "p3-launcher",
    "private": true,
    "version": "0.1.0",
    "type": "module",
    "scripts": {
      "dev": "vite",
      "build": "vite build",
      "typecheck": "tsc --noEmit",
      "test": "vitest run",
      "tauri": "tauri"
    },
    "dependencies": { "@tauri-apps/api": "<pinned>", "react": "<pinned>", "react-dom": "<pinned>" },
    "devDependencies": {
      "@tauri-apps/cli": "<pinned>", "@types/react": "<pinned>", "@types/react-dom": "<pinned>",
      "@vitejs/plugin-react": "<pinned>", "@testing-library/react": "<pinned>",
      "@testing-library/user-event": "<pinned>", "jsdom": "<pinned>", "typescript": "7.0.2",
      "vite": "<pinned>", "vitest": "<pinned>"
    }
  }
  ```
  (`<pinned>` = the exact version from Step 1 for that package.)
  `tsconfig.json`:
  ```json
  {
    "compilerOptions": {
      "target": "ES2022", "lib": ["ES2022", "DOM", "DOM.Iterable"], "module": "ESNext",
      "moduleResolution": "bundler", "jsx": "react-jsx", "strict": true, "noEmit": true,
      "skipLibCheck": true, "resolveJsonModule": true, "types": ["vitest/globals"]
    },
    "include": ["src"]
  }
  ```
  `vite.config.ts`:
  ```ts
  import { defineConfig } from "vite";
  import react from "@vitejs/plugin-react";

  export default defineConfig({
    plugins: [react()],
    clearScreen: false,
    server: { port: 1420, strictPort: true },
    test: { environment: "jsdom", globals: true, setupFiles: ["./src/setupTests.ts"] },
  });
  ```
  (add `/// <reference types="vitest/config" />` as the first line).
  `index.html`: standard Vite root with `<div id="root"></div>` and
  `<script type="module" src="/src/main.tsx"></script>`, `<title>Patapon 3</title>`.
  `src/setupTests.ts`: `import "@testing-library/jest-dom/vitest";` — add
  `@testing-library/jest-dom` to the Step 1 pins and devDependencies.
  `src/main.tsx`:
  ```tsx
  import { StrictMode } from "react";
  import { createRoot } from "react-dom/client";
  import { App } from "./App";
  import "./styles.css";

  createRoot(document.getElementById("root")!).render(<StrictMode><App /></StrictMode>);
  ```
  `src/App.tsx`: `export function App() { return <main data-testid="app">Patapon 3</main>; }`
  `src/styles.css`: `html, body, #root { margin: 0; height: 100%; background: #000; color: #fff; }`
  `src/App.test.tsx`:
  ```tsx
  import { render, screen } from "@testing-library/react";
  import { App } from "./App";

  test("renders the app root", () => {
    render(<App />);
    expect(screen.getByTestId("app")).toBeInTheDocument();
  });
  ```
  `.gitignore`: `node_modules/`, `dist/`, `src-tauri/target/`, `src-tauri/gen/`.
  `src-tauri/Cargo.toml`:
  ```toml
  [package]
  name = "p3-launcher"
  version = "0.1.0"
  edition = "2021"

  [lib]
  name = "p3_launcher"
  crate-type = ["staticlib", "cdylib", "rlib"]

  [build-dependencies]
  tauri-build = { version = "2", features = [] }

  [dependencies]
  tauri = { version = "2", features = ["protocol-asset"] }
  serde = { version = "1", features = ["derive"] }
  serde_json = "1"
  gilrs = "0.11"

  [dev-dependencies]
  tempfile = "3"
  ```
  `src-tauri/build.rs`: `fn main() { tauri_build::build() }`.
  `src-tauri/src/main.rs`:
  ```rust
  #![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
  fn main() { p3_launcher::run() }
  ```
  `src-tauri/src/lib.rs`:
  ```rust
  pub fn run() {
      tauri::Builder::default()
          .run(tauri::generate_context!())
          .expect("error while running the launcher");
  }
  ```
  `src-tauri/tauri.conf.json`:
  ```json
  {
    "$schema": "https://schema.tauri.app/config/2",
    "productName": "Patapon 3",
    "version": "0.1.0",
    "identifier": "community.patapon3.launcher",
    "build": {
      "beforeDevCommand": "npm run dev", "devUrl": "http://localhost:1420",
      "beforeBuildCommand": "npm run build", "frontendDist": "../dist"
    },
    "app": {
      "windows": [{ "label": "main", "title": "Patapon 3", "width": 1280, "height": 720,
                    "resizable": true, "visible": false }],
      "security": { "csp": null, "assetProtocol": { "enable": true, "scope": ["**"] } }
    },
    "bundle": { "active": false }
  }
  ```
  (`visible: false`: Task 9 shows the window after applying fullscreen.)
  `src-tauri/capabilities/default.json`:
  ```json
  { "identifier": "default", "windows": ["main"], "permissions": ["core:default"] }
  ```
  Then `git init -b main` in `DECO/launcher` and `npm install`.
- [ ] **Step 3: Toolchain check** (this task has no behaviour to drive by a failing test; it
  proves the four toolchains work): `npm test` -> 1 passed; `npm run typecheck` -> no output;
  `cargo test --manifest-path src-tauri/Cargo.toml` -> compiles, 0 tests. Then
  `npm run tauri build -- --debug --no-bundle` -> `src-tauri/target/debug/p3-launcher.exe` exists.
  (Window visibility is handled in Task 9; the debug exe starts hidden — kill it by PID if run.)
- [ ] **Step 4: Commit** (launcher repo, first commit) `chore: scaffold the Patapon 3 launcher
  (Tauri 2, React 19, TypeScript 7.0.2, Vitest)` with `Verified:` (npm test 1/1, typecheck clean,
  cargo test builds, debug exe built) and `Agent:`. Configure nothing in git config: if git asks
  for an identity, use `git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit ...`.

### Task 6: Launcher — format-preserving INI and option catalogs (Rust)

**Files:**
- Create: `src-tauri/src/ini.rs`, `src-tauri/src/options.rs`, `src-tauri/core_options.json`
- Create: `src-tauri/tests/fixtures/` (copies of the port's five fixtures)
- Modify: `src-tauri/src/lib.rs` (`mod ini; mod options;`)

**Interfaces:**
- Consumes: Task 1 fixtures (copied) and catalog schema.
- Produces:
  ```rust
  // ini.rs
  pub struct IniFile { /* lines */ }
  impl IniFile {
      pub fn parse(text: &str) -> IniFile;
      pub fn get(&self, section: &str, key: &str) -> Option<String>;   // last value wins
      pub fn set(&mut self, section: &str, key: &str, value: &str);     // in place, or appended
      pub fn push_line(&mut self, line: &str);                          // raw line at the end
      pub fn to_string(&self) -> String;                                // comments/unknown kept
  }
  // options.rs
  #[derive(Deserialize, Serialize, Clone)] pub struct OptionDef { pub section: String, pub key: String,
      #[serde(rename = "type")] pub kind: String, pub values: Option<Vec<String>>, pub min: Option<i64>,
      pub max: Option<i64>, pub default: String, pub tab: String, pub label: Localized,
      pub description: Option<Localized>, pub display: Option<BTreeMap<String, Localized>> }
  #[derive(Deserialize, Serialize, Clone)] pub struct Localized { pub es: String, pub en: String }
  #[derive(Deserialize, Serialize, Clone)] pub struct Catalog { pub version: u32, pub options: Vec<OptionDef> }
  pub fn parse_catalog(json: &str) -> Result<Catalog, String>;
  pub fn core_catalog() -> Catalog;                                // include_str!("../core_options.json")
  pub fn normalize(def: &OptionDef, value: &str) -> Option<String>; // validated canonical value
  pub fn effective(file: &IniFile, defs: &[OptionDef]) -> BTreeMap<String, String>; // "section.key" -> value
  pub fn default_ini(defs: &[OptionDef]) -> String;                // the spec's default file text
  ```

- [ ] **Step 1: `core_options.json`** — one entry per core key of the contract table (Task 1),
  same schema as `game_options.json`, `section`/`key`/`type`/`values`/`min`/`max`/`default` exactly
  as the C++ `kCore` table (Task 2 Step 5), `tab` = `general` (language), `graphics` (display.*),
  `online` (online.*); `paths.*` entries get `"tab": "hidden"`. Labels (es/en):
  language "Idioma"/"Language", fullscreen "Pantalla completa"/"Fullscreen", window_scale
  "Tamaño de ventana"/"Window size", render_scale "Resolución interna"/"Internal resolution",
  present_filter "Filtro de escalado"/"Scaling filter", texture_filter "Filtro de texturas"/
  "Texture filter", hd_textures "Texturas HD"/"HD textures", texture_pack "Paquete de texturas"/
  "Texture pack" (tab hidden), server "Servidor"/"Server", nickname "Apodo"/"Nickname", relay
  "Relevo del servidor"/"Server relay", port_offset "Desplazamiento de puertos"/"Port offset",
  mac_file tab hidden. `display` for render_scale: `"1": "480×272"` … `"8": "3840×2176"`
  (480·n × 272·n), for window_scale the same, for filters `linear` "Suave"/"Smooth", `nearest`
  "Píxel nítido"/"Sharp pixels", `auto` "Como el juego"/"As the game".
- [ ] **Step 2: Failing tests** at the bottom of `ini.rs` and `options.rs`:
  ```rust
  #[cfg(test)]
  mod tests {
      use super::*;
      fn fixture(name: &str) -> String {
          std::fs::read_to_string(format!("{}/tests/fixtures/{}", env!("CARGO_MANIFEST_DIR"), name)).unwrap()
      }
      #[test] fn keeps_comments_and_unknown_keys() {
          let text = "; head\n[display]\n# c\nrender_scale = 4 ; tail stays\nweird = 1\n[x]\ny=2\n";
          let mut f = IniFile::parse(text);
          f.set("display", "render_scale", "6");
          let out = f.to_string();
          assert!(out.contains("; head\n[display]\n# c\nrender_scale = 6\n"));
          assert!(out.contains("weird = 1") && out.contains("[x]\ny=2"));
      }
      #[test] fn set_appends_missing_key_and_section() {
          let mut f = IniFile::parse("[display]\nfullscreen = true\n");
          f.set("display", "render_scale", "2");
          f.set("online", "relay", "true");
          assert_eq!(f.get("display", "render_scale").as_deref(), Some("2"));
          assert!(f.to_string().ends_with("[online]\nrelay = true\n"));
      }
      #[test] fn bom_crlf_and_case() {
          let f = IniFile::parse(&fixture("bom_crlf.ini"));
          assert_eq!(f.get("DISPLAY", "Render_Scale").as_deref(), Some("5"));
          assert_eq!(f.get("online", "nickname").as_deref(), Some("Ñandú"));
      }
      #[test] fn last_duplicate_wins() {
          let f = IniFile::parse(&fixture("invalid.ini"));
          assert_eq!(f.get("display", "render_scale").as_deref(), Some("3"));
      }
      #[test] fn crlf_round_trip_keeps_crlf() {
          let mut f = IniFile::parse("[a]\r\nk = 1\r\n");
          f.set("a", "k", "2");
          assert_eq!(f.to_string(), "[a]\r\nk = 2\r\n");
      }
  }
  ```
  and in `options.rs`:
  ```rust
  #[cfg(test)]
  mod tests {
      use super::*;
      use crate::ini::IniFile;
      fn fixture(name: &str) -> String {
          std::fs::read_to_string(format!("{}/tests/fixtures/{}", env!("CARGO_MANIFEST_DIR"), name)).unwrap()
      }
      fn game() -> Catalog {
          parse_catalog(&std::fs::read_to_string(
              format!("{}/../../psprecomp/games/patapon3/game_options.json", env!("CARGO_MANIFEST_DIR"))).unwrap()).unwrap()
      }
      fn all() -> Vec<OptionDef> { let mut v = core_catalog().options; v.extend(game().options); v }
      #[test] fn normalize_rules() {
          let defs = all();
          let by = |k: &str| defs.iter().find(|d| format!("{}.{}", d.section, d.key) == k).unwrap().clone();
          assert_eq!(normalize(&by("display.fullscreen"), "YES").as_deref(), Some("true"));
          assert_eq!(normalize(&by("display.fullscreen"), "maybe"), None);
          assert_eq!(normalize(&by("display.render_scale"), "8").as_deref(), Some("8"));
          assert_eq!(normalize(&by("display.render_scale"), "9"), None);
          assert_eq!(normalize(&by("game.fps"), "60").as_deref(), Some("60"));
          assert_eq!(normalize(&by("game.fps"), "45"), None);
          assert_eq!(normalize(&by("online.nickname"), ""), None);
          assert_eq!(normalize(&by("online.port_offset"), "70000"), None);
      }
      #[test] fn effective_values_match_the_game() {
          let defs = all();
          let e = effective(&IniFile::parse(&fixture("invalid.ini")), &defs);
          assert_eq!(e["display.fullscreen"], "true");     // bad bool -> default
          assert_eq!(e["display.render_scale"], "3");      // last duplicate
          assert_eq!(e["display.window_scale"], "2");      // bad int -> default
          assert_eq!(e["game.fps"], "30");
          assert_eq!(e["online.nickname"], "Patapon");
          let c = effective(&IniFile::parse(&fixture("custom.ini")), &defs);
          assert_eq!(c["game.fps"], "60");
          assert_eq!(c["online.relay"], "true");
          assert_eq!(c["paths.savedata"], "Partidas Guardadas");
      }
      #[test] fn default_ini_equals_the_contract_fixture() {
          let defs = all();
          let ours = effective(&IniFile::parse(&default_ini(&defs)), &defs);
          let contract = effective(&IniFile::parse(&fixture("defaults.ini")), &defs);
          assert_eq!(ours, contract);
      }
  }
  ```
  Copy the five fixtures from `psprecomp/docs/contracts/fixtures/` into
  `src-tauri/tests/fixtures/` (keep bytes: `cp`). Stubs: `IniFile` with an empty `Vec<String>`,
  every function `unimplemented!()` is NOT allowed — return empty / `None` so tests fail on
  values. Run `cargo test --manifest-path src-tauri/Cargo.toml`. Expected: the tests fail.
- [ ] **Step 3: Implement `ini.rs`:**
  ```rust
  //! Format-preserving INI for settings.ini v1 (psprecomp docs/contracts/settings-v1.md):
  //! comments, unknown keys and line endings survive a rewrite.
  pub struct IniFile { lines: Vec<String>, crlf: bool }

  fn norm(s: &str) -> String { s.trim().to_lowercase() }
  fn section_of(line: &str) -> Option<String> {
      let t = line.trim();
      (t.starts_with('[') && t.ends_with(']') && t.len() >= 2).then(|| norm(&t[1..t.len() - 1]))
  }
  fn key_value(line: &str) -> Option<(String, String)> {
      let t = line.trim();
      if t.is_empty() || t.starts_with(';') || t.starts_with('#') || section_of(t).is_some() { return None; }
      let eq = t.find('=')?;
      Some((norm(&t[..eq]), t[eq + 1..].trim().to_string()))
  }

  impl IniFile {
      pub fn parse(text: &str) -> IniFile {
          let text = text.strip_prefix('\u{feff}').unwrap_or(text);
          let crlf = text.contains("\r\n");
          let mut lines: Vec<String> = text.split('\n').map(|l| l.trim_end_matches('\r').to_string()).collect();
          if lines.last().map(|l| l.is_empty()).unwrap_or(false) { lines.pop(); }
          IniFile { lines, crlf }
      }
      pub fn get(&self, section: &str, key: &str) -> Option<String> {
          let (s, k) = (norm(section), norm(key));
          let mut cur = String::new();
          let mut found = None;
          for l in &self.lines {
              if let Some(sec) = section_of(l) { cur = sec; continue; }
              if let Some((kk, v)) = key_value(l) { if cur == s && kk == k { found = Some(v); } }
          }
          found
      }
      pub fn set(&mut self, section: &str, key: &str, value: &str) {
          let (s, k) = (norm(section), norm(key));
          let mut cur = String::new();
          let mut last_match = None;
          let mut section_end = None;
          for (i, l) in self.lines.iter().enumerate() {
              if let Some(sec) = section_of(l) { cur = sec; continue; }
              if cur == s {
                  section_end = Some(i);
                  if let Some((kk, _)) = key_value(l) { if kk == k { last_match = Some(i); } }
              }
          }
          let new_line = format!("{} = {}", k, value);
          if let Some(i) = last_match { self.lines[i] = new_line; return; }
          if let Some(i) = section_end { self.lines.insert(i + 1, new_line); return; }
          if self.lines.iter().any(|l| section_of(l).as_deref() == Some(&s)) {
              let pos = self.lines.iter().position(|l| section_of(l).as_deref() == Some(&s)).unwrap();
              self.lines.insert(pos + 1, new_line);
              return;
          }
          self.lines.push(format!("[{}]", s));
          self.lines.push(new_line);
      }
      pub fn to_string(&self) -> String {
          let nl = if self.crlf { "\r\n" } else { "\n" };
          let mut out = self.lines.join(nl);
          out.push_str(nl);
          out
      }
  }
  ```
  Note: the test `keeps_comments_and_unknown_keys` replaces `render_scale = 4 ; tail stays` by
  `render_scale = 6` (inline comments are not part of the contract; values are whole lines).
- [ ] **Step 4: Implement `options.rs`:**
  ```rust
  //! Option catalogs (core_options.json, game/game_options.json) and value rules shared with
  //! the game's reader (psprecomp runtime/src/psp_settings.cpp).
  use crate::ini::IniFile;
  use serde::{Deserialize, Serialize};
  use std::collections::BTreeMap;

  #[derive(Deserialize, Serialize, Clone, Debug, PartialEq)]
  pub struct Localized { pub es: String, pub en: String }

  #[derive(Deserialize, Serialize, Clone, Debug)]
  pub struct OptionDef {
      pub section: String, pub key: String,
      #[serde(rename = "type")] pub kind: String,
      #[serde(default)] pub values: Option<Vec<String>>,
      #[serde(default)] pub min: Option<i64>, #[serde(default)] pub max: Option<i64>,
      pub default: String, pub tab: String, pub label: Localized,
      #[serde(default)] pub description: Option<Localized>,
      #[serde(default)] pub display: Option<BTreeMap<String, Localized>>,
  }

  #[derive(Deserialize, Serialize, Clone, Debug)]
  pub struct Catalog { pub version: u32, pub options: Vec<OptionDef> }

  pub fn parse_catalog(json: &str) -> Result<Catalog, String> {
      serde_json::from_str(json).map_err(|e| e.to_string())
  }

  pub fn core_catalog() -> Catalog {
      parse_catalog(include_str!("../core_options.json")).expect("core_options.json is valid")
  }

  pub fn normalize(def: &OptionDef, value: &str) -> Option<String> {
      let v = value.trim();
      match def.kind.as_str() {
          "bool" => match v.to_lowercase().as_str() {
              "true" | "1" | "yes" | "on" => Some("true".into()),
              "false" | "0" | "no" | "off" => Some("false".into()),
              _ => None,
          },
          "int" => {
              let digits = v.trim_start_matches(['+', '-']);
              if digits.is_empty() || !digits.chars().all(|c| c.is_ascii_digit()) || v.len() > 7 { return None; }
              let n: i64 = v.parse().ok()?;
              let ok = def.min.map_or(true, |m| n >= m) && def.max.map_or(true, |m| n <= m);
              ok.then(|| n.to_string())
          }
          "enum" => def.values.as_ref()?.iter().any(|x| x == v).then(|| v.to_string()),
          _ => (!v.is_empty()).then(|| v.to_string()),
      }
  }

  pub fn effective(file: &IniFile, defs: &[OptionDef]) -> BTreeMap<String, String> {
      defs.iter().map(|d| {
          let v = file.get(&d.section, &d.key).and_then(|raw| normalize(d, &raw)).unwrap_or_else(|| d.default.clone());
          (format!("{}.{}", d.section, d.key), v)
      }).collect()
  }

  pub fn default_ini(defs: &[OptionDef]) -> String {
      let mut f = IniFile::parse("; settings.ini v1 — written by the Patapon 3 launcher\n[meta]\nversion = 1\n");
      for d in defs { f.set(&d.section, &d.key, &d.default); }
      f.push_line("[controls]");
      f.to_string()
  }
  ```
  and in `ini.rs` (`IniFile`): `pub fn push_line(&mut self, line: &str) { self.lines.push(line.to_string()); }`
  (add it to the Interfaces list of this task).
- [ ] **Step 5: Run** `cargo test --manifest-path src-tauri/Cargo.toml`. Expected: all pass.
- [ ] **Step 6: Commit** `feat: settings.ini v1 (format-preserving) and option catalogs` with
  `Verified: cargo test RED -> GREEN (ini 5, options 3)` and `Agent:`.

### Task 7: Launcher — settings root, install checks, launching and exit codes (Rust)

**Files:**
- Create: `src-tauri/src/settings.rs`, `src-tauri/src/launch.rs`
- Modify: `src-tauri/src/lib.rs` (commands)

**Interfaces:**
- Consumes: Task 6 `IniFile`, `effective`, `normalize`, `default_ini`, `core_catalog`, `parse_catalog`.
- Produces (Rust):
  ```rust
  // settings.rs
  pub fn root_dir() -> PathBuf;     // P3L_ROOT env, else the launcher exe's folder
  pub fn catalogs(root: &Path) -> (Vec<OptionDef>, Vec<String>);  // core + game/game_options.json, warnings
  pub fn load(root: &Path) -> Result<BTreeMap<String, String>, String>;  // creates defaults if missing
  pub fn save(root: &Path, changes: &BTreeMap<String, String>) -> Result<(), String>; // validates all first
  // launch.rs
  pub fn game_exe(root: &Path) -> PathBuf;  // game/psprecomp_runtime(.exe)
  #[derive(Serialize, Debug, PartialEq)] pub struct InstallCheck { pub exe: bool, pub disc0: bool, pub ascii_path: bool }
  pub fn check_install(root: &Path, disc0: &str) -> InstallCheck;
  pub fn path_is_ascii(p: &Path) -> bool;
  #[derive(Serialize, Debug, PartialEq)] #[serde(tag = "kind")] pub enum Outcome { Menu, Quit, Crash { code: Option<i32> } }
  pub fn outcome_from_code(code: Option<i32>) -> Outcome;   // 10 Menu, 0 Quit, else Crash
  pub fn game_args(root: &Path) -> Vec<String>;            // ["--settings", <abs ini>, "--log", <abs log>]
  pub fn rotate_logs(root: &Path) -> std::io::Result<()>;  // logs/game.log -> logs/game.prev.log
  pub struct LaunchGuard(AtomicBool);  impl LaunchGuard { pub fn try_begin(&self) -> bool; pub fn end(&self); }
  ```
  Tauri commands (JS names): `get_options() -> { options: OptionDef[], values: Record<string,string>, warnings: string[] }`,
  `save_settings(changes: Record<string,string>) -> void`, `check_install() -> InstallCheck`,
  `start_game() -> Outcome` (hides `main`, runs the game, shows `main`, `Quit` also exits the app),
  `open_log() -> void`.

- [ ] **Step 1: Failing tests** (bottom of `launch.rs` and `settings.rs`):
  ```rust
  #[cfg(test)]
  mod tests {
      use super::*;
      #[test] fn outcomes() {
          assert_eq!(outcome_from_code(Some(10)), Outcome::Menu);
          assert_eq!(outcome_from_code(Some(0)), Outcome::Quit);
          assert_eq!(outcome_from_code(Some(3)), Outcome::Crash { code: Some(3) });
          assert_eq!(outcome_from_code(None), Outcome::Crash { code: None });
      }
      #[test] fn exe_name_per_os() {
          let p = game_exe(Path::new("R"));
          if cfg!(windows) { assert!(p.ends_with("game/psprecomp_runtime.exe")); }
          else { assert!(p.ends_with("game/psprecomp_runtime")); }
      }
      #[test] fn spawn_args_with_spaces() {
          let root = Path::new("C:/Juegos/Patapon 3");
          let a = game_args(root);
          assert_eq!(a[0], "--settings");
          assert!(a[1].ends_with("Patapon 3/settings.ini") || a[1].ends_with("Patapon 3\\settings.ini"));
          assert_eq!(a[2], "--log");
          assert!(a[3].contains("logs"));
      }
      #[test] fn install_checks() {
          let d = tempfile::tempdir().unwrap();
          assert_eq!(check_install(d.path(), "disc0"), InstallCheck { exe: false, disc0: false, ascii_path: path_is_ascii(d.path()) });
          std::fs::create_dir_all(d.path().join("game")).unwrap();
          std::fs::write(game_exe(d.path()), b"x").unwrap();
          std::fs::create_dir_all(d.path().join("disc0")).unwrap();
          let c = check_install(d.path(), "disc0");
          assert!(c.exe && c.disc0);
          assert!(!path_is_ascii(Path::new("C:/Juegos/Ñandú")));
      }
      #[test] fn rotate() {
          let d = tempfile::tempdir().unwrap();
          std::fs::create_dir_all(d.path().join("logs")).unwrap();
          std::fs::write(d.path().join("logs/game.log"), b"old").unwrap();
          rotate_logs(d.path()).unwrap();
          assert_eq!(std::fs::read(d.path().join("logs/game.prev.log")).unwrap(), b"old");
          assert!(!d.path().join("logs/game.log").exists());
          rotate_logs(d.path()).unwrap();   // nothing to rotate: still Ok
      }
      #[test] fn guard_allows_one_launch() {
          let g = LaunchGuard::default();
          assert!(g.try_begin());
          assert!(!g.try_begin());
          g.end();
          assert!(g.try_begin());
      }
  }
  ```
  `settings.rs`:
  ```rust
  #[cfg(test)]
  mod tests {
      use super::*;
      fn root() -> tempfile::TempDir {
          let d = tempfile::tempdir().unwrap();
          std::fs::create_dir_all(d.path().join("game")).unwrap();
          std::fs::copy(format!("{}/../../psprecomp/games/patapon3/game_options.json", env!("CARGO_MANIFEST_DIR")),
                        d.path().join("game/game_options.json")).unwrap();
          d
      }
      #[test] fn load_creates_defaults() {
          let d = root();
          let v = load(d.path()).unwrap();
          assert_eq!(v["display.render_scale"], "4");
          assert_eq!(v["game.fps"], "30");
          assert!(d.path().join("settings.ini").exists());
      }
      #[test] fn save_validates_everything_first() {
          let d = root();
          load(d.path()).unwrap();
          let before = std::fs::read_to_string(d.path().join("settings.ini")).unwrap();
          let mut bad = BTreeMap::new();
          bad.insert("display.render_scale".to_string(), "2".to_string());
          bad.insert("game.fps".to_string(), "45".to_string());
          assert!(save(d.path(), &bad).is_err());
          assert_eq!(std::fs::read_to_string(d.path().join("settings.ini")).unwrap(), before);
          let mut ok = BTreeMap::new();
          ok.insert("game.fps".to_string(), "60".to_string());
          save(d.path(), &ok).unwrap();
          assert_eq!(load(d.path()).unwrap()["game.fps"], "60");
      }
      #[test] fn save_keeps_user_comments() {
          let d = root();
          std::fs::write(d.path().join("settings.ini"), "; mine\n[display]\nrender_scale = 3\n[mods]\nx = 1\n").unwrap();
          let mut ok = BTreeMap::new();
          ok.insert("display.render_scale".to_string(), "5".to_string());
          save(d.path(), &ok).unwrap();
          let t = std::fs::read_to_string(d.path().join("settings.ini")).unwrap();
          assert!(t.starts_with("; mine\n") && t.contains("render_scale = 5") && t.contains("[mods]\nx = 1"));
      }
      #[test] fn missing_game_catalog_is_a_warning() {
          let d = tempfile::tempdir().unwrap();
          let (defs, warnings) = catalogs(d.path());
          assert!(defs.iter().all(|o| o.section != "game"));
          assert_eq!(warnings.len(), 1);
      }
  }
  ```
  Stubs return empty values / `Err("todo".into())` so tests fail; `mod settings; mod launch;` in
  `lib.rs`. Run `cargo test`: expected failures.
- [ ] **Step 2: Implement `launch.rs`:**
  ```rust
  //! Launching the game (psprecomp runtime) and reading its exit code
  //! (docs/contracts/settings-v1.md: 0 quit, 10 back to menu, other crash).
  use serde::Serialize;
  use std::path::{Path, PathBuf};
  use std::sync::atomic::{AtomicBool, Ordering};

  pub fn game_exe(root: &Path) -> PathBuf {
      root.join("game").join(if cfg!(windows) { "psprecomp_runtime.exe" } else { "psprecomp_runtime" })
  }

  #[derive(Serialize, Debug, PartialEq)]
  pub struct InstallCheck { pub exe: bool, pub disc0: bool, pub ascii_path: bool }

  pub fn path_is_ascii(p: &Path) -> bool { p.to_string_lossy().is_ascii() }

  pub fn check_install(root: &Path, disc0: &str) -> InstallCheck {
      let d = Path::new(disc0);
      let disc = if d.is_absolute() { d.to_path_buf() } else { root.join(d) };
      InstallCheck { exe: game_exe(root).is_file(), disc0: disc.is_dir(), ascii_path: path_is_ascii(root) }
  }

  #[derive(Serialize, Debug, PartialEq)]
  #[serde(tag = "kind")]
  pub enum Outcome { Menu, Quit, Crash { code: Option<i32> } }

  pub fn outcome_from_code(code: Option<i32>) -> Outcome {
      match code { Some(10) => Outcome::Menu, Some(0) => Outcome::Quit, c => Outcome::Crash { code: c } }
  }

  pub fn game_args(root: &Path) -> Vec<String> {
      vec!["--settings".into(), root.join("settings.ini").to_string_lossy().into_owned(),
           "--log".into(), root.join("logs").join("game.log").to_string_lossy().into_owned()]
  }

  pub fn rotate_logs(root: &Path) -> std::io::Result<()> {
      let logs = root.join("logs");
      std::fs::create_dir_all(&logs)?;
      let cur = logs.join("game.log");
      if cur.exists() { std::fs::rename(&cur, logs.join("game.prev.log"))?; }
      Ok(())
  }

  #[derive(Default)]
  pub struct LaunchGuard(AtomicBool);
  impl LaunchGuard {
      pub fn try_begin(&self) -> bool { self.0.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst).is_ok() }
      pub fn end(&self) { self.0.store(false, Ordering::SeqCst); }
  }

  /// Runs the game and waits; returns its exit code (None when killed by a signal).
  pub fn run_game(root: &Path) -> std::io::Result<Option<i32>> {
      rotate_logs(root)?;
      let status = std::process::Command::new(game_exe(root))
          .args(game_args(root))
          .current_dir(root.join("game"))
          .status()?;
      Ok(status.code())
  }
  ```
  (`rotate_logs` on Windows: `rename` fails when the target exists — delete
  `game.prev.log` first: `let prev = logs.join("game.prev.log"); if prev.exists() { std::fs::remove_file(&prev)?; }`.)
- [ ] **Step 3: Implement `settings.rs`:**
  ```rust
  //! settings.ini in the portable root (psprecomp docs/contracts/settings-v1.md).
  use crate::ini::IniFile;
  use crate::options::{core_catalog, default_ini, effective, normalize, parse_catalog, OptionDef};
  use std::collections::BTreeMap;
  use std::path::{Path, PathBuf};

  pub fn root_dir() -> PathBuf {
      if let Ok(r) = std::env::var("P3L_ROOT") { return PathBuf::from(r); }
      std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_else(|| PathBuf::from("."))
  }

  pub fn catalogs(root: &Path) -> (Vec<OptionDef>, Vec<String>) {
      let mut defs = core_catalog().options;
      let mut warnings = Vec::new();
      match std::fs::read_to_string(root.join("game").join("game_options.json")) {
          Ok(t) => match parse_catalog(&t) {
              Ok(c) => defs.extend(c.options),
              Err(e) => warnings.push(format!("game_options.json: {e}")),
          },
          Err(e) => warnings.push(format!("game/game_options.json: {e}")),
      }
      (defs, warnings)
  }

  fn ini_path(root: &Path) -> PathBuf { root.join("settings.ini") }

  pub fn load(root: &Path) -> Result<BTreeMap<String, String>, String> {
      let (defs, _) = catalogs(root);
      let p = ini_path(root);
      if !p.exists() { std::fs::write(&p, default_ini(&defs)).map_err(|e| e.to_string())?; }
      let text = std::fs::read_to_string(&p).map_err(|e| e.to_string())?;
      Ok(effective(&IniFile::parse(&text), &defs))
  }

  pub fn save(root: &Path, changes: &BTreeMap<String, String>) -> Result<(), String> {
      let (defs, _) = catalogs(root);
      let mut norm = Vec::new();
      for (id, v) in changes {
          let d = defs.iter().find(|d| format!("{}.{}", d.section, d.key) == *id)
              .ok_or_else(|| format!("unknown option {id}"))?;
          let n = normalize(d, v).ok_or_else(|| format!("invalid value \"{v}\" for {id}"))?;
          norm.push((d.section.clone(), d.key.clone(), n));
      }
      let p = ini_path(root);
      let text = std::fs::read_to_string(&p).unwrap_or_else(|_| default_ini(&defs));
      let mut f = IniFile::parse(&text);
      for (s, k, v) in norm { f.set(&s, &k, &v); }
      let tmp = p.with_extension("ini.tmp");
      std::fs::write(&tmp, f.to_string()).map_err(|e| e.to_string())?;
      std::fs::rename(&tmp, &p).or_else(|_| { std::fs::remove_file(&p).ok(); std::fs::rename(&tmp, &p) })
          .map_err(|e| e.to_string())
  }
  ```
- [ ] **Step 4: Commands in `lib.rs`:**
  ```rust
  mod ini;
  mod launch;
  mod options;
  mod settings;

  use serde::Serialize;
  use std::collections::BTreeMap;
  use tauri::{AppHandle, Manager, State};

  #[derive(Serialize)]
  struct OptionsPayload { options: Vec<options::OptionDef>, values: BTreeMap<String, String>, warnings: Vec<String> }

  #[tauri::command]
  fn get_options() -> Result<OptionsPayload, String> {
      let root = settings::root_dir();
      let (options, warnings) = settings::catalogs(&root);
      Ok(OptionsPayload { options, values: settings::load(&root)?, warnings })
  }

  #[tauri::command]
  fn save_settings(changes: BTreeMap<String, String>) -> Result<(), String> {
      settings::save(&settings::root_dir(), &changes)
  }

  #[tauri::command]
  fn check_install() -> Result<launch::InstallCheck, String> {
      let root = settings::root_dir();
      let v = settings::load(&root)?;
      Ok(launch::check_install(&root, v.get("paths.disc0").map(String::as_str).unwrap_or("disc0")))
  }

  #[tauri::command]
  async fn start_game(app: AppHandle, guard: State<'_, launch::LaunchGuard>) -> Result<launch::Outcome, String> {
      if !guard.try_begin() { return Err("already running".into()); }
      let win = app.get_webview_window("main").ok_or("no window")?;
      win.hide().map_err(|e| e.to_string())?;
      let root = settings::root_dir();
      let code = tauri::async_runtime::spawn_blocking(move || launch::run_game(&root))
          .await.map_err(|e| e.to_string())?;
      guard.end();
      let outcome = match code { Ok(c) => launch::outcome_from_code(c), Err(_) => launch::Outcome::Crash { code: None } };
      if outcome == launch::Outcome::Quit { app.exit(0); }
      win.show().map_err(|e| e.to_string())?;
      win.set_focus().ok();
      Ok(outcome)
  }

  #[tauri::command]
  fn open_log() -> Result<(), String> {
      let log = settings::root_dir().join("logs").join("game.log");
      let opener = if cfg!(windows) { ("explorer", vec![log.to_string_lossy().into_owned()]) }
                   else { ("xdg-open", vec![log.to_string_lossy().into_owned()]) };
      std::process::Command::new(opener.0).args(opener.1).spawn().map(|_| ()).map_err(|e| e.to_string())
  }

  pub fn run() {
      tauri::Builder::default()
          .manage(launch::LaunchGuard::default())
          .invoke_handler(tauri::generate_handler![get_options, save_settings, check_install, start_game, open_log])
          .run(tauri::generate_context!())
          .expect("error while running the launcher");
  }
  ```
- [ ] **Step 5: Run** `cargo test --manifest-path src-tauri/Cargo.toml`. Expected: all pass
  (Task 6 + 6 launch + 4 settings).
- [ ] **Step 6: Commit** `feat: settings root, install checks and game launch with exit codes` with
  `Verified: cargo test RED -> GREEN` and `Agent:`.

### Task 8: Launcher — gamepad navigation in Rust (gilrs)

**Files:**
- Create: `src-tauri/src/gamepad.rs`
- Modify: `src-tauri/src/lib.rs` (spawn the thread in `setup`)

**Interfaces:**
- Produces: Tauri event `"nav"` with payload `{ "action": "up"|"down"|"left"|"right"|"confirm"|
  "back"|"tab_prev"|"tab_next"|"reset", "device": "xbox"|"playstation" }`;
  pure functions
  ```rust
  pub fn map_button(b: gilrs::Button) -> Option<&'static str>;
  pub fn device_kind(name: &str) -> &'static str;
  pub struct StickRepeat { /* per axis direction state */ }
  impl StickRepeat { pub fn update(&mut self, x: f32, y: f32, now_ms: u64) -> Option<&'static str>; }
  ```
  (stick: threshold 0.6, first repeat after 400 ms, then every 120 ms; d-pad buttons are
  single presses.)

- [ ] **Step 1: Failing tests:**
  ```rust
  #[cfg(test)]
  mod tests {
      use super::*;
      use gilrs::Button;
      #[test] fn buttons() {
          assert_eq!(map_button(Button::DPadUp), Some("up"));
          assert_eq!(map_button(Button::South), Some("confirm"));
          assert_eq!(map_button(Button::East), Some("back"));
          assert_eq!(map_button(Button::LeftTrigger), Some("tab_prev"));
          assert_eq!(map_button(Button::RightTrigger), Some("tab_next"));
          assert_eq!(map_button(Button::North), Some("reset"));
          assert_eq!(map_button(Button::Mode), None);
      }
      #[test] fn devices() {
          assert_eq!(device_kind("DualSense Wireless Controller"), "playstation");
          assert_eq!(device_kind("PS4 Controller"), "playstation");
          assert_eq!(device_kind("Xbox Series X Controller"), "xbox");
          assert_eq!(device_kind("Generic"), "xbox");
      }
      #[test] fn stick_repeat() {
          let mut s = StickRepeat::default();
          assert_eq!(s.update(0.0, 0.9, 0), Some("up"));     // gilrs: +y is up
          assert_eq!(s.update(0.0, 0.9, 300), None);
          assert_eq!(s.update(0.0, 0.9, 400), Some("up"));
          assert_eq!(s.update(0.0, 0.9, 450), None);
          assert_eq!(s.update(0.0, 0.9, 520), Some("up"));
          assert_eq!(s.update(0.0, 0.1, 530), None);          // back to centre
          assert_eq!(s.update(-0.8, 0.0, 600), Some("left"));
      }
  }
  ```
  Stubs return `None` / `"xbox"`. Run `cargo test`: FAIL.
- [ ] **Step 2: Implement:**
  ```rust
  //! Gamepad -> navigation events for the web UI (the Web Gamepad API is unreliable on Linux).
  use gilrs::{Axis, Button, EventType, Gilrs};
  use serde::Serialize;
  use tauri::{AppHandle, Emitter};

  pub fn map_button(b: Button) -> Option<&'static str> {
      Some(match b {
          Button::DPadUp => "up", Button::DPadDown => "down", Button::DPadLeft => "left",
          Button::DPadRight => "right", Button::South => "confirm", Button::East => "back",
          Button::LeftTrigger | Button::LeftTrigger2 => "tab_prev",
          Button::RightTrigger | Button::RightTrigger2 => "tab_next", Button::North => "reset",
          _ => return None,
      })
  }

  pub fn device_kind(name: &str) -> &'static str {
      let n = name.to_lowercase();
      if n.contains("dualsense") || n.contains("dualshock") || n.contains("ps4") || n.contains("ps5")
          || n.contains("playstation") || n.contains("sony") { "playstation" } else { "xbox" }
  }

  #[derive(Default)]
  pub struct StickRepeat { dir: Option<&'static str>, next_ms: u64 }
  impl StickRepeat {
      pub fn update(&mut self, x: f32, y: f32, now_ms: u64) -> Option<&'static str> {
          let dir = if y > 0.6 { Some("up") } else if y < -0.6 { Some("down") }
                    else if x < -0.6 { Some("left") } else if x > 0.6 { Some("right") } else { None };
          match dir {
              None => { self.dir = None; None }
              Some(d) if self.dir != Some(d) => { self.dir = Some(d); self.next_ms = now_ms + 400; Some(d) }
              Some(d) if now_ms >= self.next_ms => { self.next_ms = now_ms + 120; Some(d) }
              _ => None,
          }
      }
  }

  #[derive(Serialize, Clone)]
  struct Nav { action: &'static str, device: &'static str }

  pub fn spawn(app: AppHandle) {
      std::thread::spawn(move || {
          let Ok(mut gilrs) = Gilrs::new() else { return };
          let start = std::time::Instant::now();
          let mut stick = StickRepeat::default();
          loop {
              while let Some(ev) = gilrs.next_event() {
                  let device = device_kind(gilrs.gamepad(ev.id).name());
                  if let EventType::ButtonPressed(b, _) = ev.event {
                      if let Some(action) = map_button(b) { let _ = app.emit("nav", Nav { action, device }); }
                  }
              }
              for (_, pad) in gilrs.gamepads() {
                  let x = pad.value(Axis::LeftStickX);
                  let y = pad.value(Axis::LeftStickY);
                  if let Some(action) = stick.update(x, y, start.elapsed().as_millis() as u64) {
                      let _ = app.emit("nav", Nav { action, device: device_kind(pad.name()) });
                  }
                  break;  // first connected pad drives the stick
              }
              std::thread::sleep(std::time::Duration::from_millis(16));
          }
      });
  }
  ```
  In `lib.rs` `run()`: `.setup(|app| { gamepad::spawn(app.handle().clone()); Ok(()) })` and `mod gamepad;`.
- [ ] **Step 3: Run** `cargo test`. Expected pass.
- [ ] **Step 4: Commit** `feat: gamepad navigation events from gilrs` with `Verified:` and `Agent:`.

### Task 9: Launcher UI — i18n, theme, navigation, main menu, prompt bar

**Files:**
- Create: `src-tauri/src/assets.rs` (commands `get_lang(code)`, `get_theme()`), `lang/es.json`,
  `lang/en.json`, `themes/default/theme.json`, `themes/default/background.png`,
  `themes/default/logo.png`, `themes/default/theme.css`
- Create: `src/types.ts`, `src/api.ts`, `src/i18n.ts`, `src/theme.ts`, `src/nav/events.ts`,
  `src/nav/useNav.ts`, `src/components/PromptBar.tsx`, `src/screens/MainMenu.tsx`,
  `src/screens/MainMenu.test.tsx`, `src/nav/useNav.test.tsx`
- Modify: `src/App.tsx`, `src/styles.css`, `src-tauri/src/lib.rs` (setup: fullscreen + show)

**Interfaces:**
- Consumes: Task 7 commands, Task 8 `"nav"` event.
- Produces (TS):
  ```ts
  // types.ts
  export type Localized = { es: string; en: string };
  export type OptionDef = { section: string; key: string; type: "bool"|"enum"|"int"|"string"|"path";
    values?: string[]; min?: number; max?: number; default: string; tab: string; label: Localized;
    description?: Localized; display?: Record<string, Localized> };
  export type Outcome = { kind: "Menu" } | { kind: "Quit" } | { kind: "Crash"; code: number | null };
  export type InstallCheck = { exe: boolean; disc0: boolean; ascii_path: boolean };
  export type NavAction = "up"|"down"|"left"|"right"|"confirm"|"back"|"tab_prev"|"tab_next"|"reset";
  export type Device = "keyboard"|"xbox"|"playstation";
  // api.ts — thin invoke wrappers (mocked in tests)
  export const api: { getOptions(): Promise<{options: OptionDef[]; values: Record<string,string>; warnings: string[]}>;
    saveSettings(c: Record<string,string>): Promise<void>; checkInstall(): Promise<InstallCheck>;
    startGame(): Promise<Outcome>; openLog(): Promise<void>; getLang(code: string): Promise<Record<string,string>>;
    getTheme(): Promise<{ colors: Record<string,string>; font?: string; background: string; logo: string; css?: string }> };
  // i18n.ts
  export function t(strings: Record<string,string>, key: string, vars?: Record<string,string>): string;
  // nav/events.ts
  export function keyToAction(e: KeyboardEvent): NavAction | null;
  export function onNav(cb: (a: NavAction, device: Device) => void): () => void; // keyboard + Tauri "nav"
  // nav/useNav.ts
  export function useListNav(count: number, onConfirm: (i: number) => void, onBack?: () => void): { index: number; setIndex(i: number): void; device: Device };
  ```
  Language file keys (both files): `menu.start`, `menu.options`, `menu.exit`, `prompt.select`,
  `prompt.confirm`, `prompt.back`, `prompt.reset`, `prompt.category`, `options.title`,
  `tab.general`, `tab.graphics`, `tab.controls`, `tab.online`, `options.unsaved`,
  `options.unsaved.save`, `options.unsaved.discard`, `controls.readonly`, `start.missing_exe`,
  `start.missing_disc0`, `start.non_ascii`, `start.launching`, `crash.title`, `crash.body`,
  `crash.open_log`, `crash.back`, `warn.catalog`.

- [ ] **Step 1: Failing tests** `src/nav/useNav.test.tsx`:
  ```tsx
  import { act, renderHook } from "@testing-library/react";
  import { keyToAction } from "./events";
  import { useListNav } from "./useNav";

  vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));

  test("keyboard keys map to actions", () => {
    const k = (key: string) => keyToAction(new KeyboardEvent("keydown", { key }));
    expect(k("ArrowUp")).toBe("up");
    expect(k("Enter")).toBe("confirm");
    expect(k("Escape")).toBe("back");
    expect(k("q")).toBe("tab_prev");
    expect(k("e")).toBe("tab_next");
    expect(k("r")).toBe("reset");
    expect(k("x")).toBeNull();
  });

  test("list navigation wraps and confirms", () => {
    const onConfirm = vi.fn();
    const { result } = renderHook(() => useListNav(3, onConfirm));
    const press = (key: string) => act(() => { window.dispatchEvent(new KeyboardEvent("keydown", { key })); });
    press("ArrowUp");
    expect(result.current.index).toBe(2);
    press("ArrowDown");
    expect(result.current.index).toBe(0);
    press("Enter");
    expect(onConfirm).toHaveBeenCalledWith(0);
  });
  ```
  `src/screens/MainMenu.test.tsx`:
  ```tsx
  import { render, screen } from "@testing-library/react";
  import userEvent from "@testing-library/user-event";
  import { MainMenu } from "./MainMenu";

  vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
  const strings = { "menu.start": "Iniciar", "menu.options": "Opciones", "menu.exit": "Salir",
    "prompt.select": "Seleccionar", "prompt.confirm": "Confirmar", "prompt.back": "Volver" };

  test("shows the three entries and starts with Enter", async () => {
    const onStart = vi.fn(), onOptions = vi.fn(), onExit = vi.fn();
    render(<MainMenu strings={strings} logo="" background="" onStart={onStart} onOptions={onOptions} onExit={onExit} busy={false} />);
    expect(screen.getByText("Iniciar")).toHaveAttribute("aria-selected", "true");
    await userEvent.keyboard("{Enter}");
    expect(onStart).toHaveBeenCalledOnce();
  });

  test("mouse click picks an entry; busy disables start", async () => {
    const onStart = vi.fn(), onOptions = vi.fn();
    const { rerender } = render(<MainMenu strings={strings} logo="" background="" onStart={onStart} onOptions={onOptions} onExit={vi.fn()} busy={false} />);
    await userEvent.click(screen.getByText("Opciones"));
    expect(onOptions).toHaveBeenCalledOnce();
    rerender(<MainMenu strings={strings} logo="" background="" onStart={onStart} onOptions={onOptions} onExit={vi.fn()} busy={true} />);
    await userEvent.click(screen.getByText("Iniciar"));
    expect(onStart).not.toHaveBeenCalled();
  });

  test("prompt bar starts with keyboard glyphs", async () => {
    render(<MainMenu strings={strings} logo="" background="" onStart={vi.fn()} onOptions={vi.fn()} onExit={vi.fn()} busy={false} />);
    expect(screen.getByTestId("prompt-bar")).toHaveAttribute("data-device", "keyboard");
  });
  ```
  Create empty exports so imports resolve; run `npm test`: expected failures.
- [ ] **Step 2: Implement** `src/nav/events.ts`:
  ```ts
  import { listen } from "@tauri-apps/api/event";
  import type { Device, NavAction } from "../types";

  const KEYS: Record<string, NavAction> = {
    ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right",
    Enter: "confirm", " ": "confirm", Escape: "back", Backspace: "back",
    q: "tab_prev", Q: "tab_prev", e: "tab_next", E: "tab_next", r: "reset", R: "reset",
  };

  export function keyToAction(e: KeyboardEvent): NavAction | null {
    return KEYS[e.key] ?? null;
  }

  export function onNav(cb: (a: NavAction, device: Device) => void): () => void {
    const onKey = (e: KeyboardEvent) => {
      const a = keyToAction(e);
      if (a && !(e.target instanceof HTMLInputElement)) { e.preventDefault(); cb(a, "keyboard"); }
    };
    window.addEventListener("keydown", onKey);
    let unlisten: (() => void) | null = null;
    let closed = false;
    listen<{ action: NavAction; device: Device }>("nav", (ev) => cb(ev.payload.action, ev.payload.device))
      .then((u) => { if (closed) u(); else unlisten = u; })
      .catch(() => {});
    return () => { closed = true; window.removeEventListener("keydown", onKey); unlisten?.(); };
  }
  ```
  `src/nav/useNav.ts`:
  ```ts
  import { useEffect, useRef, useState } from "react";
  import { onNav } from "./events";
  import type { Device } from "../types";

  export function useListNav(count: number, onConfirm: (i: number) => void, onBack?: () => void) {
    const [index, setIndex] = useState(0);
    const [device, setDevice] = useState<Device>("keyboard");
    const ref = useRef({ index, count, onConfirm, onBack });
    ref.current = { index, count, onConfirm, onBack };
    useEffect(() => onNav((a, d) => {
      setDevice(d);
      const { index: i, count: n } = ref.current;
      if (a === "up") setIndex((i - 1 + n) % n);
      else if (a === "down") setIndex((i + 1) % n);
      else if (a === "confirm") ref.current.onConfirm(i);
      else if (a === "back") ref.current.onBack?.();
    }), []);
    return { index, setIndex, device };
  }
  ```
  `src/components/PromptBar.tsx`:
  ```tsx
  import type { Device } from "../types";

  const GLYPHS: Record<Device, Record<string, string>> = {
    keyboard: { select: "↑↓", confirm: "Enter", back: "Esc", reset: "R", category: "Q E" },
    xbox: { select: "✚", confirm: "A", back: "B", reset: "Y", category: "LB RB" },
    playstation: { select: "✚", confirm: "✕", back: "○", reset: "△", category: "L1 R1" },
  };

  export function PromptBar({ device, items }: { device: Device; items: { id: string; label: string }[] }) {
    return (
      <footer className="prompt-bar" data-testid="prompt-bar" data-device={device}>
        {items.map((it) => (
          <span key={it.id} className="prompt"><kbd>{GLYPHS[device][it.id] ?? it.id}</kbd> : {it.label}</span>
        ))}
      </footer>
    );
  }
  ```
  `src/screens/MainMenu.tsx`:
  ```tsx
  import { PromptBar } from "../components/PromptBar";
  import { t } from "../i18n";
  import { useListNav } from "../nav/useNav";

  type Props = { strings: Record<string, string>; logo: string; background: string; busy: boolean;
    onStart(): void; onOptions(): void; onExit(): void };

  export function MainMenu(p: Props) {
    const entries = [
      { id: "start", label: t(p.strings, "menu.start"), run: () => { if (!p.busy) p.onStart(); } },
      { id: "options", label: t(p.strings, "menu.options"), run: p.onOptions },
      { id: "exit", label: t(p.strings, "menu.exit"), run: p.onExit },
    ];
    const nav = useListNav(entries.length, (i) => entries[i].run());
    return (
      <div className="screen main-menu" style={p.background ? { backgroundImage: `url(${p.background})` } : undefined}>
        {p.logo && <img className="logo" src={p.logo} alt="" />}
        <ul className="menu" role="listbox">
          {entries.map((e, i) => (
            <li key={e.id} role="option" aria-selected={nav.index === i} aria-disabled={e.id === "start" && p.busy}
                className={nav.index === i ? "selected" : ""}
                onMouseEnter={() => nav.setIndex(i)} onClick={() => e.run()}>{e.label}</li>
          ))}
        </ul>
        <PromptBar device={nav.device} items={[
          { id: "select", label: t(p.strings, "prompt.select") },
          { id: "confirm", label: t(p.strings, "prompt.confirm") },
        ]} />
      </div>
    );
  }
  ```
  `src/i18n.ts`:
  ```ts
  export function t(strings: Record<string, string>, key: string, vars?: Record<string, string>): string {
    let s = strings[key] ?? key;
    for (const [k, v] of Object.entries(vars ?? {})) s = s.replaceAll(`{${k}}`, v);
    return s;
  }
  ```
  `src/api.ts`:
  ```ts
  import { invoke } from "@tauri-apps/api/core";
  import type { InstallCheck, OptionDef, Outcome } from "./types";

  export const api = {
    getOptions: () => invoke<{ options: OptionDef[]; values: Record<string, string>; warnings: string[] }>("get_options"),
    saveSettings: (changes: Record<string, string>) => invoke<void>("save_settings", { changes }),
    checkInstall: () => invoke<InstallCheck>("check_install"),
    startGame: () => invoke<Outcome>("start_game"),
    openLog: () => invoke<void>("open_log"),
    getLang: (code: string) => invoke<Record<string, string>>("get_lang", { code }),
    getTheme: () => invoke<{ colors: Record<string, string>; font?: string; background: string; logo: string; css?: string }>("get_theme"),
  };
  ```
  `src/theme.ts`: `applyTheme(theme)` sets CSS variables `--c-<name>` on `document.documentElement`
  for each color, `--font` when `font` is given (via an injected `@font-face` named `ThemeFont`
  with `src: url(<font>)`), and appends `theme.css` text in a `<style id="theme-css">`.
  `src-tauri/src/assets.rs`:
  ```rust
  //! lang/<code>.json and themes/default/ from the portable root, as data for the web UI.
  use crate::settings::root_dir;
  use serde::Serialize;
  use std::collections::BTreeMap;

  #[tauri::command]
  pub fn get_lang(code: String) -> Result<BTreeMap<String, String>, String> {
      let safe: String = code.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-').collect();
      let read = |c: &str| std::fs::read_to_string(root_dir().join("lang").join(format!("{c}.json")));
      let text = read(&safe).or_else(|_| read("en")).map_err(|e| e.to_string())?;
      serde_json::from_str(&text).map_err(|e| e.to_string())
  }

  #[derive(Serialize)]
  pub struct Theme { colors: BTreeMap<String, String>, font: Option<String>, background: String, logo: String, css: Option<String> }

  #[tauri::command]
  pub fn get_theme() -> Result<Theme, String> {
      let dir = root_dir().join("themes").join("default");
      #[derive(serde::Deserialize)] struct Raw { colors: BTreeMap<String, String>, font: Option<String> }
      let raw: Raw = serde_json::from_str(&std::fs::read_to_string(dir.join("theme.json")).map_err(|e| e.to_string())?)
          .map_err(|e| e.to_string())?;
      let p = |f: &str| dir.join(f).to_string_lossy().into_owned();
      Ok(Theme { colors: raw.colors, font: raw.font.map(|f| p(&f)), background: p("background.png"), logo: p("logo.png"),
                 css: std::fs::read_to_string(dir.join("theme.css")).ok() })
  }
  ```
  (the web side turns `background`, `logo`, `font` file paths into URLs with
  `convertFileSrc` from `@tauri-apps/api/core` — do it in `App.tsx` when loading the theme.)
  `themes/default/theme.json`: `{ "colors": { "bg": "#0b0b0e", "text": "#ffffff", "accent": "#f5a400",
  "panel": "#e9e9e4", "panelText": "#111111", "muted": "#9a9a9a" }, "font": null }` (the default
  font is the system UI font). `background.png`: 1920x1080 solid `#0b0b0e` with a soft radial
  `#1d1a10` centre, generated with Python + Pillow (`python -I -c "..."` in the step; Pillow is
  installed). `logo.png`: 1200x300 transparent with the text "PATAPON 3" in white, generated the
  same way with the default PIL font scaled (it is a placeholder for part 5).
  `lang/es.json` and `lang/en.json`: every key listed in Interfaces; Spanish values:
  "Iniciar", "Opciones", "Salir", "Seleccionar", "Confirmar", "Volver", "Restablecer ajustes",
  "Categoría", "OPCIONES", "General", "Gráficos", "Controles", "Online",
  "Hay cambios sin guardar.", "Guardar", "Descartar", "Edición de controles: próximamente.",
  "No se encontró el juego (game/psprecomp_runtime).", "No se encontraron los datos del juego (disc0). Pronto el launcher los sacará de tu ISO.",
  "La carpeta tiene caracteres no ASCII ({path}); el juego podría no encontrar sus archivos. Muévela a una ruta sin acentos.",
  "Iniciando…", "El juego se cerró inesperadamente", "Código de salida: {code}.", "Abrir el log",
  "Volver al menú", "No se pudo leer game_options.json; faltan las opciones del juego.";
  English the same meaning.
  `App.tsx` (this task): loads `getOptions` (for `general.language`), `getLang`, `getTheme`, applies
  the theme, renders `MainMenu`; `onExit` calls `getCurrentWindow().close()` from
  `@tauri-apps/api/window`; `onOptions` and `onStart` are wired in Tasks 10-11 (here
  `onOptions={() => {}}`, `onStart={() => {}}`).
  `lib.rs` `setup`: read `display.fullscreen` via `settings::load(&root_dir())`; on the `main`
  window call `set_fullscreen(true)` when it is `"true"`, then `show()`. Register
  `assets::get_lang`, `assets::get_theme` in `generate_handler!`.
  `styles.css`: full-screen `.screen` (flex column, centred, `background: var(--c-bg) center/cover`),
  `.menu li` large text, `.selected` with `color: var(--c-accent)`, `.prompt-bar` fixed bottom row
  with spaced items, `kbd` boxed.
- [ ] **Step 3: Run** `npm test` (expected: all pass), `npm run typecheck` (clean),
  `cargo test` (pass).
- [ ] **Step 4: Manual look.** `P3L_ROOT=C:/Users/torso/Documents/deco/launcher npm run tauri dev`
  (the root holds `lang/` and `themes/`; it has no `game/` yet, so the catalog warning is
  expected). Check: window shows the menu, arrows / mouse move the selection, Esc does nothing on
  the main menu, Salir closes. Take a screenshot (`Win+Shift+S` is not scriptable — use the
  `mcp__claude-in-chrome` tools only if the user asks; otherwise describe it in the commit).
- [ ] **Step 5: Commit** `feat: main menu, prompt bar, languages and the default theme` with
  `Verified:` (npm test N/N, typecheck clean, cargo test, manual dev window) and `Agent:`.

### Task 10: Launcher UI — Options (tabs, rows from the catalogs, save / restore / unsaved)

**Files:**
- Create: `src/components/Tabs.tsx`, `src/components/OptionRow.tsx`, `src/screens/Options.tsx`,
  `src/screens/Options.test.tsx`
- Modify: `src/App.tsx`, `src/styles.css`

**Interfaces:**
- Consumes: Task 9 types, `t`, `onNav`, `PromptBar`; Task 7 `getOptions`/`saveSettings`.
- Produces: `<Options strings lang defs values onSaved onBack />` where
  `onSaved(values: Record<string,string>)` and `onBack()`; rows: tab `general` -> language,
  `graphics` -> display.* then game.* in catalog order, `online` -> online.*, `controls` -> the
  read-only mapping list; `tab: "hidden"` options never shown.

- [ ] **Step 1: Failing tests** `src/screens/Options.test.tsx`:
  ```tsx
  import { render, screen, within } from "@testing-library/react";
  import userEvent from "@testing-library/user-event";
  import { Options } from "./Options";
  import type { OptionDef } from "../types";

  vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
  const L = (es: string) => ({ es, en: es });
  const defs: OptionDef[] = [
    { section: "general", key: "language", type: "enum", values: ["es", "en"], default: "es", tab: "general", label: L("Idioma") },
    { section: "display", key: "fullscreen", type: "bool", default: "true", tab: "graphics", label: L("Pantalla completa") },
    { section: "display", key: "render_scale", type: "int", min: 1, max: 8, default: "4", tab: "graphics", label: L("Resolución interna"),
      display: { "4": L("1920×1088"), "5": L("2400×1360") } },
    { section: "game", key: "fps", type: "enum", values: ["30", "60"], default: "30", tab: "graphics", label: L("Fotogramas por segundo") },
    { section: "paths", key: "disc0", type: "path", default: "disc0", tab: "hidden", label: L("disc0") },
    { section: "online", key: "nickname", type: "string", default: "Patapon", tab: "online", label: L("Apodo") },
  ];
  const values = { "general.language": "es", "display.fullscreen": "true", "display.render_scale": "4",
    "game.fps": "30", "paths.disc0": "disc0", "online.nickname": "Patapon" };
  const strings = { "tab.general": "General", "tab.graphics": "Gráficos", "tab.controls": "Controles", "tab.online": "Online",
    "options.unsaved": "Hay cambios sin guardar.", "options.unsaved.save": "Guardar", "options.unsaved.discard": "Descartar" };

  function setup(onSaved = vi.fn(), onBack = vi.fn()) {
    render(<Options strings={strings} lang="es" defs={defs} values={values} onSaved={onSaved} onBack={onBack} />);
    return { onSaved, onBack };
  }

  test("tabs switch with Q/E and hide hidden options", async () => {
    setup();
    expect(screen.getByText("Idioma")).toBeInTheDocument();
    await userEvent.keyboard("e");
    expect(screen.getByText("Resolución interna")).toBeInTheDocument();
    expect(screen.getByText("Fotogramas por segundo")).toBeInTheDocument();
    expect(screen.queryByText("disc0")).toBeNull();
  });

  test("left/right change the selected value; enter saves only changes", async () => {
    const { onSaved } = setup();
    await userEvent.keyboard("e");                 // Gráficos: row 0 fullscreen
    await userEvent.keyboard("{ArrowDown}");       // render_scale
    await userEvent.keyboard("{ArrowRight}");      // 4 -> 5
    expect(within(screen.getByTestId("row-display.render_scale")).getByText("2400×1360")).toBeInTheDocument();
    await userEvent.keyboard("{Enter}");
    expect(onSaved).toHaveBeenCalledWith({ "display.render_scale": "5" });
  });

  test("int never leaves its range; bool toggles", async () => {
    setup();
    await userEvent.keyboard("e");
    await userEvent.keyboard("{ArrowLeft}");       // fullscreen true -> false
    expect(within(screen.getByTestId("row-display.fullscreen")).getByRole("switch")).toHaveAttribute("aria-checked", "false");
  });

  test("back with unsaved changes asks first; discard leaves", async () => {
    const { onBack } = setup();
    await userEvent.keyboard("e{ArrowLeft}{Escape}");
    expect(screen.getByText("Hay cambios sin guardar.")).toBeInTheDocument();
    expect(onBack).not.toHaveBeenCalled();
    await userEvent.click(screen.getByText("Descartar"));
    expect(onBack).toHaveBeenCalledOnce();
  });

  test("R restores the defaults of the current tab", async () => {
    const { onSaved } = setup();
    await userEvent.keyboard("e{ArrowLeft}");       // fullscreen true -> false (unsaved)
    await userEvent.keyboard("r{Enter}");           // defaults again, then confirm
    expect(onSaved).toHaveBeenCalledWith({});       // nothing differs from the saved values
  });

  test("text option edits with the keyboard", async () => {
    const { onSaved } = setup();
    await userEvent.keyboard("eee");                // General -> Gráficos -> Controles -> Online
    const input = within(screen.getByTestId("row-online.nickname")).getByRole("textbox");
    await userEvent.clear(input);
    await userEvent.type(input, "Edson");
    input.blur();
    await userEvent.keyboard("{Enter}");
    expect(onSaved).toHaveBeenCalledWith({ "online.nickname": "Edson" });
  });
  ```
  Empty `Options` export; run: FAIL.
- [ ] **Step 2: Implement.** Rules (the tests pin them):
  - State: `draft` (copy of `values`), `tab` index, `row` index, `confirmLeave` flag.
  - `onNav`: `tab_prev`/`tab_next` change tab (wrap), reset `row` to 0; `up`/`down` move the row
    (wrap); `left`/`right` step the value: bool toggles; enum moves through `values`; int moves by
    1 clamped to `min..max`; string / path ignore left/right; `reset` sets every row of the current
    tab to its `default`; `confirm` on a text row focuses its input, otherwise saves:
    `onSaved(diff)` where `diff` = keys whose `draft` differs from `values`; `back` -> if `diff` is
    empty `onBack()` else show the unsaved box (Guardar -> save then `onBack`, Descartar ->
    `onBack`, `back` again closes the box).
  - While an `<input>` has focus, navigation ignores keys (events.ts already skips inputs);
    Enter in the input blurs it.
  - `OptionRow` renders: label (in `lang`), the value text (`display[value][lang]` or the value),
    `role="switch" aria-checked` for bools, `<input type="text">` for string/path, `data-testid`
    `row-<section>.<key>`, `aria-selected` when it is the current row; click selects the row, a
    click on the value steps it right.
  - Controls tab: a read-only two-column table (PSP button, keyboard key, gamepad button) built
    from a constant matching `key_to_psp_button` and `pad_button_to_psp` in the runtime
    (Cross: X / Enter, A; Circle: Z, B; Square: A, X; Triangle: S, Y; L: Q, LB/LT; R: W, RB/RT;
    Start: Space, Start; Select: V / Tab, Back; D-pad: arrows, d-pad) and the line
    `controls.readonly`.
  - `PromptBar` items: reset, category, select, confirm, back.
  Wire in `App.tsx`: `screen` state `"menu" | "options"`; `onOptions` -> options; `onSaved(diff)`
  -> `api.saveSettings(diff)` then reload values, stay in Options; `onBack` -> menu. When
  `general.language` changes after a save, reload the strings.
- [ ] **Step 3: Run** `npm test` (all pass), `npm run typecheck` (clean).
- [ ] **Step 4: Manual.** `npm run tauri dev` with `P3L_ROOT` pointing at a temp root that has
  `game/game_options.json` (copy it from the port): change render scale and fps, confirm, check
  the root's `settings.ini` with `cat`: comment header kept, `render_scale` / `fps` updated.
- [ ] **Step 5: Commit** `feat: Options screen built from the catalogs (save, restore, unsaved)` with
  `Verified:` (npm test, typecheck, manual settings.ini diff) and `Agent:`.

### Task 11: Launcher UI — Start, return from the game, crash notice

**Files:**
- Create: `src/screens/Crash.tsx`, `src/screens/Start.test.tsx`
- Modify: `src/App.tsx`

**Interfaces:**
- Consumes: `api.checkInstall`, `api.startGame`, `api.openLog`; `Outcome`, `InstallCheck`.
- Produces: `startFlow(api, setScreen, strings)` behaviour in `App`: checks -> message screen for
  missing pieces (with Back), otherwise `busy = true` -> `startGame()` -> `Menu` back to the menu,
  `Crash` -> `<Crash code onOpenLog onBack />`; errors from `startGame` (e.g. "already running")
  are ignored while busy.

- [ ] **Step 1: Failing tests** `src/screens/Start.test.tsx` (render `App` with `api` mocked):
  ```tsx
  import { render, screen, waitFor } from "@testing-library/react";
  import userEvent from "@testing-library/user-event";

  vi.mock("@tauri-apps/api/event", () => ({ listen: vi.fn(async () => () => {}) }));
  vi.mock("@tauri-apps/api/window", () => ({ getCurrentWindow: () => ({ close: vi.fn() }) }));
  vi.mock("@tauri-apps/api/core", () => ({ convertFileSrc: (p: string) => p, invoke: vi.fn() }));
  const api = vi.hoisted(() => ({
    getOptions: vi.fn(async () => ({ options: [], values: { "general.language": "es", "display.fullscreen": "true" }, warnings: [] })),
    getLang: vi.fn(async () => ({ "menu.start": "Iniciar", "menu.options": "Opciones", "menu.exit": "Salir",
      "start.missing_disc0": "Faltan datos", "start.missing_exe": "Falta el juego", "crash.title": "Se cerró",
      "crash.body": "Código {code}", "crash.open_log": "Abrir el log", "crash.back": "Volver al menú", "start.launching": "Iniciando…" })),
    getTheme: vi.fn(async () => ({ colors: {}, background: "", logo: "" })),
    saveSettings: vi.fn(), openLog: vi.fn(), checkInstall: vi.fn(), startGame: vi.fn(),
  }));
  vi.mock("../api", () => ({ api }));
  import { App } from "../App";

  test("missing disc0 is explained and the game is not started", async () => {
    api.checkInstall.mockResolvedValue({ exe: true, disc0: false, ascii_path: true });
    render(<App />);
    await userEvent.click(await screen.findByText("Iniciar"));
    expect(await screen.findByText("Faltan datos")).toBeInTheDocument();
    expect(api.startGame).not.toHaveBeenCalled();
  });

  test("back to menu returns to the main menu; double start launches once", async () => {
    api.checkInstall.mockResolvedValue({ exe: true, disc0: true, ascii_path: true });
    let finish: (v: unknown) => void = () => {};
    api.startGame.mockImplementation(() => new Promise((r) => { finish = r; }));
    render(<App />);
    await userEvent.click(await screen.findByText("Iniciar"));
    await userEvent.click(screen.getByText("Iniciar"));
    await waitFor(() => expect(api.startGame).toHaveBeenCalledOnce());
    finish({ kind: "Menu" });
    expect(await screen.findByText("Opciones")).toBeInTheDocument();
  });

  test("a crash shows the notice with its code and opens the log", async () => {
    api.checkInstall.mockResolvedValue({ exe: true, disc0: true, ascii_path: true });
    api.startGame.mockResolvedValue({ kind: "Crash", code: 3 });
    render(<App />);
    await userEvent.click(await screen.findByText("Iniciar"));
    expect(await screen.findByText("Se cerró")).toBeInTheDocument();
    expect(screen.getByText("Código 3")).toBeInTheDocument();
    await userEvent.click(screen.getByText("Abrir el log"));
    expect(api.openLog).toHaveBeenCalledOnce();
  });
  ```
  Run: FAIL.
- [ ] **Step 2: Implement** in `App.tsx`: `busy` state; `onStart`:
  ```ts
  async function start() {
    if (busy) return;
    const c = await api.checkInstall();
    if (!c.exe || !c.disc0) { setNotice(t(strings, c.exe ? "start.missing_disc0" : "start.missing_exe")); return; }
    if (!c.ascii_path) setWarning(t(strings, "start.non_ascii", { path: "" }));
    setBusy(true);
    try {
      const o = await api.startGame();
      if (o.kind === "Crash") setCrash({ code: o.code });
      else setScreen("menu");
    } catch { /* already running: ignore */ } finally { setBusy(false); }
  }
  ```
  `notice` renders a panel with the text and a Back entry (navigable with the same nav);
  `Crash.tsx` renders `crash.title`, `crash.body` with `{code}` (or `-` when null), buttons
  `crash.open_log` (`api.openLog()`) and `crash.back` (back to the menu).
- [ ] **Step 3: Run** `npm test`, `npm run typecheck`: all pass / clean.
- [ ] **Step 4: Commit** `feat: start the game, return to the menu, crash notice` with `Verified:` and `Agent:`.

### Task 12: Portable folder, end-to-end run, regression

**Files:**
- Create: `scripts/make-portable.mjs` (launcher repo), `README.md` (launcher repo, Spanish + English short)
- Modify (port): `docs/superpowers/plans/2026-10-17-launcher-part1.md` (Findings section)

**Interfaces:**
- Consumes: everything above; the port build in `DECO/build/rtw`.
- Produces: `DECO/launcher/dist/Patapon3Recomp/` (portable folder) via
  `node scripts/make-portable.mjs --game C:/Users/torso/Documents/deco/build/rtw --data C:/Users/torso/Documents/deco --out dist/Patapon3Recomp`.

- [ ] **Step 1: Script** `scripts/make-portable.mjs`:
  ```js
  // Assembles the portable folder: launcher, game exe + DLLs + game_options.json, lang, themes.
  // --data links disc0/ and textures/ (junctions on Windows, symlinks elsewhere) instead of copying.
  import { cpSync, existsSync, mkdirSync, readdirSync, rmSync, symlinkSync } from "node:fs";
  import { join, resolve } from "node:path";

  const arg = (name) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : undefined; };
  const game = resolve(arg("--game") ?? "");
  const data = arg("--data") ? resolve(arg("--data")) : undefined;
  const out = resolve(arg("--out") ?? "dist/Patapon3Recomp");
  const win = process.platform === "win32";
  const exe = win ? "psprecomp_runtime.exe" : "psprecomp_runtime";
  if (!existsSync(join(game, exe))) { console.error(`no ${exe} in ${game}`); process.exit(1); }
  const launcher = join("src-tauri", "target", "release", win ? "p3-launcher.exe" : "p3-launcher");
  if (!existsSync(launcher)) { console.error(`build the launcher first: npm run tauri build -- --no-bundle`); process.exit(1); }

  rmSync(out, { recursive: true, force: true });
  mkdirSync(join(out, "game"), { recursive: true });
  mkdirSync(join(out, "logs"), { recursive: true });
  cpSync(launcher, join(out, win ? "launcher.exe" : "launcher"));
  for (const f of readdirSync(game)) {
    if (f === exe || f.endsWith(".dll") || f === "game_options.json") cpSync(join(game, f), join(out, "game", f));
  }
  cpSync("lang", join(out, "lang"), { recursive: true });
  cpSync("themes", join(out, "themes"), { recursive: true });
  if (data) {
    for (const [name, target] of [["disc0", join(data, "disc0")], ["textures", join(data, "PPSSPP", "memstick", "PSP", "TEXTURES")]]) {
      if (existsSync(target)) symlinkSync(target, join(out, name), win ? "junction" : "dir");
    }
  }
  console.log(`portable folder: ${out}`);
  ```
  (`textures/INFN00001` then resolves inside the linked `TEXTURES` folder, matching the default
  `texture_pack`.)
- [ ] **Step 2: Build and assemble.** Port 9999 check; port `cmake --build DECO/build/rtw`;
  launcher `npm run tauri build -- --no-bundle`; then the Step 1 command. Expected: the folder
  with `launcher.exe`, `game/psprecomp_runtime.exe` + DLLs + `game_options.json`, `lang/`,
  `themes/`, junctions `disc0`, `textures`.
- [ ] **Step 3: Savedata for the run.** Copy the tour save (never the user's):
  `cp -r C:/Users/torso/Documents/deco/build/tour_save dist/Patapon3Recomp/savedata`.
- [ ] **Step 4: End-to-end, automated part.** With `PSPRECOMP_DEBUG_PORT=9998` in the environment
  (inherited by the game), start `dist/Patapon3Recomp/launcher.exe` in the background. Expect
  `settings.ini` created with the defaults. Then set `game.fps = 60` by editing the file with the
  launcher closed (the UI path was tested in Task 10), start again, and pick Iniciar by hand
  (ask the user to do Steps 5-6 if they are at the PC; otherwise do them yourself on the visible
  window). Check `dist/Patapon3Recomp/logs/game.log`: `[SETTINGS] reading ...settings.ini`,
  `PSPRECOMP_PATCHES=60fps`, `[P3] 60fps: input update paced to 30 Hz`.
- [ ] **Step 5: End-to-end, by hand (the user, in Spanish):** open `launcher.exe`; check full
  screen; move with keyboard, mouse and a gamepad; change two options in Gráficos and Online and
  confirm; Iniciar; in the game press F1 -> No (keeps playing) -> F1 -> Sí: the launcher comes
  back on the main menu; then Select+Start held 1 s on the gamepad -> Sí: back again; Iniciar and
  close the game window: the launcher closes too. Crash path: while the game runs,
  `taskkill /F /PID <game pid>` (a killed process exits with code 1): the launcher shows the
  crash notice with code 1 and "Abrir el log" opens `logs/game.log`.
- [ ] **Step 6: Regression.** Without `settings.ini` next to `build/rtw/psprecomp_runtime.exe`:
  `TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/launcher_t12`
  and `rt_regress build/tour/settings_t3 build/tour/launcher_t12`: PASS. `jugar.bat` unchanged
  (`git -C DECO status` is not a repo — check the file's content is the same as before part 1).
- [ ] **Step 7: Launcher README** (Spanish first, then English): what it is, folder layout,
  `settings.ini` pointer to the contract, how to build (`npm install`, `npm run tauri build --
  --no-bundle`, `node scripts/make-portable.mjs ...`), F1 / Select+Start.
- [ ] **Step 8: Findings + commits.** Write `## Findings` at the end of this plan (E2E results,
  anything the user reported). Commit the launcher repo `feat: portable folder script and README`
  and the port repo `docs(plan): launcher part 1 findings`, both with `Verified:` and `Agent:`.

## Finish

- Whole-branch review over both repos (fresh reviewer on the most capable model, or a self-review
  recorded as such if the user asks for "nativo").
- Tell the user (Spanish) what works, where the portable folder is, and ask for their OK.
  No push of either repo until they say so.

## Findings

Executed inline overnight 2026-10-17 ("nativo"), Tasks 1-12 done; final review was a
self-review (no fresh reviewer). Launcher repo `DECO/launcher` (local, never pushed):
4badf57..1824af1. Port: f132c5f..8b67760 on `patapon3-windows` (not pushed).

- **E2E (automated, real windows and keys, port 9998, from `launcher/portable/Patapon3Recomp`):**
  first launch writes the default `settings.ini` and opens fullscreen; with `fps = 60` the game
  log shows `[SETTINGS] reading <root>/settings.ini`, `PSPRECOMP_PATCHES=60fps` and
  `[P3] 60fps: input update paced to 30 Hz`; F1 opens the box (Spanish, starts on No), No keeps
  playing, F1 -> Sí -> `[RT] Exit 10` -> the launcher shows its menu again; closing the game
  window -> `Exit 0` -> the launcher exits 0; `taskkill /F` of the game -> crash notice with code 1.
  Not done: a real gamepad (Select+Start 1 s, launcher navigation with gilrs) and "Abrir el log"
  by hand — they need the user.
- **Key injection pitfall:** `keybd_event` with scan code 0 never reaches SDL2 as F1 (SDL maps
  Windows keys by scan code; arrows have a VK fallback). Send `MapVirtualKeyW(vk, 0)` as the scan
  code. The runtime now logs `[RT] back-to-menu prompt open`.
- **Tours were flaky tonight:** three runs after Task 4 failed on driver desyncs ("no beat
  found", one 336 s boot) while another Claude session was active; an A/B in the same
  conditions showed the Task 3 binary also failing against `settings_t3`, and the Task 4 binary
  vs that run: RT_REGRESS PASS. Final tour (`launcher_t12`): PASS vs `ab_t4` and vs `settings_t3`.
- **Plan defects fixed on the way (rulings in the executor's ledger):** `defaults.ini` cannot
  carry trailing `;` comments (comments only at line start); Salir needs
  `core:window:allow-close`; tauri-build needs `icons/icon.ico`; the gilrs thread must drop
  events while the game runs; the prompt must let key/button releases through; the portable
  folder must not live inside Vite's `dist/` (every build empties it) and must copy
  `libc++.dll` / `libunwind.dll`; Iniciar must explain errors instead of swallowing them.
- **Open (minor):** once, the first Enter after the launcher appeared was not received (focus of
  the WebView right after `show()`); a second Enter worked. The asset protocol scope is `**`;
  narrow it to the root before community themes (part 5).
