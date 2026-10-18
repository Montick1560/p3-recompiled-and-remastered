# Launcher part 4 — remappable controls + Steam Deck style keyboard — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Execution chosen by the user:** a NEW session runs this plan. Each task is a brief for **DeepSeek v4.1
> flash at max effort** (`python ~/.claude/skills/albanil-grok/grok-build.py <brief> --repo <repo> --model
> "deepseek/deepseek-flash#max" --msg "<no-commit message>"`); Claude reads every diff, re-runs every gate,
> commits and decides. **One OpenCode session at a time** (two in parallel both die with "Failed to execute
> statement"; a cut session resumes with `--session <id>`). DeepSeek cannot edit `psprecomp/crates/` (the
> DECO `opencode.json` still denies it) — this plan does not touch `crates/`. Briefs live in
> `DECO/build/briefs_p4/` (same template as `DECO/build/briefs_p3/T1.md`: plan path, line range, "verbatim",
> no commit, gates, report).

**Goal:** The player remaps keyboard keys and gamepad buttons in the launcher, and types in the game with a
gamepad only, on a Steam Deck style on-screen keyboard with selectable skins.

**Architecture:** `settings.ini` `[controls]` (`<action>_keys` up to two SDL key names, `<action>_pad` one SDL
GameController button name) reaches the game as `PSPRECOMP_CONTROLS`; a pure `ControlMap` replaces the
runtime's two fixed `switch`es. A pure `OskKeyboard` model (grid, layers, shift / caps, accents) edits the
existing `OskSession` (whose `TextEdit` gains a cursor); the overlay draws the grid with a skin parsed from
JSON (`[display] osk_skin` → `PSPRECOMP_OSK_SKIN`). The launcher stores the bindings as catalog options of two
new kinds (`keys`, `pad`), edits them in the Controls tab (keyboard capture in the web UI, gamepad capture in
Rust via gilrs) and lists the skins.

**Tech Stack:** C++17 runtime (CMake + ctest, clang 22 llvm-mingw, SDL2, GLAD, stb_truetype, nlohmann json),
Bash (Git Bash), Rust 1.96 + Tauri 2 + gilrs 0.11 (launcher), React 19 + TSX + Vitest.

**Spec:** `docs/superpowers/specs/2026-10-18-launcher-part4-design.md` (read it first).

## Global Constraints

- Actions, in this order (it decides who keeps a duplicate): `up down left right cross circle square triangle
  l r start select`. PSP masks: up 0x0010, down 0x0040, left 0x0080, right 0x0020, cross 0x4000, circle
  0x2000, square 0x8000, triangle 0x1000, l 0x0100, r 0x0200, start 0x0008, select 0x0001.
- Defaults (= today's mapping): up `Up`/`dpup`, down `Down`/`dpdown`, left `Left`/`dpleft`, right
  `Right`/`dpright`, cross `X, Return`/`a`, circle `Z`/`b`, square `A`/`x`, triangle `S`/`y`, l `Q`/
  `leftshoulder`, r `W`/`rightshoulder`, start `Space`/`start`, select `V, Tab`/`back`.
- Fixed, never remapped: left stick = analog; LT / RT also press L / R; mouse wheel = D-pad up / down;
  F1, F11, Alt+Return, pad `guide` are reserved (refused as bindings). The back-to-menu combo holds the
  PSP buttons select + start (so it follows the bindings).
- `PSPRECOMP_CONTROLS` canonical form: `action=key1,key2|pad;action=…` (keys may be empty; pad may be
  empty = unbound). Environment variable already set wins over `settings.ini` (contract rule).
- `[display] osk_skin = <name>` → `PSPRECOMP_OSK_SKIN=<ini folder>/skins/osk/<name>.json`; names match
  `[a-z0-9_-]+`; shipped skins `deck` (default), `light`, `patapon`.
- On-screen keyboard gamepad buttons (positional SDL names, fixed while the keyboard is open): D-pad /
  left stick move, `a` press, `b` backspace, `x` shift, `y` space, `leftshoulder` / `rightshoulder` move
  the text cursor, `back` switches letters / symbols, `start` accepts. Cancel is a grid key and Esc.
- In the launcher Controls tab: Confirm on a cell captures; **reset (R / △) on a cell clears it**; the last
  row "Restore defaults" restores every binding (Backspace already means Back in the launcher; this replaces
  the spec's "Delete / Backspace clears").
- Nothing is pushed without the user's explicit OK. psprecomp commits on `patapon3-windows`, launcher on
  `main`. Author `Patapon Community` (launcher: `git -c user.name="Patapon Community"
  -c user.email=rexgamer1560@gmail.com commit ...`). **No `Co-Authored-By` trailer.** Commit bodies end with
  `Verified: …` and `Agent: …`.
- Test runs of the game use debug port **9998**, never 9999. Code, comments and commits in English; strings
  shown to the player in `launcher/lang/{es,en}.json` and, in the game overlay, es / en by `PSPRECOMP_UI_LANG`.

## Review Focus

1. Without `[controls]` (and without `PSPRECOMP_CONTROLS`) the game must map exactly as today: the
   `ControlMap` default test compares every key / pad button with the old tables (Task 1).
2. A key bound to two actions (hand-edited ini) must not press two PSP buttons: the first action keeps it,
   the log says so (Task 1 test); the launcher refuses to save duplicates (Task 4 test).
3. Typing at the text cursor when the field is full, and moving the cursor at both ends, must never
   corrupt the text or exceed the PSP limit (Task 2 tests).
4. While the keyboard is open no PSP button may stay pressed or reach the game, from the keyboard or the
   pad (Task 3: buttons are cleared on open, pad events are consumed).
5. While a launcher capture is waiting, a gamepad press must not also navigate the menu, and Esc must
   cancel instead of leaving Options (Task 4 / Task 5 tests).

## File map

psprecomp (`DECO/psprecomp`):
- `runtime/include/psp_controls.h`, `runtime/src/psp_controls.cpp`, `runtime/tests/test_controls.cpp` (T1).
- `runtime/src/psp_settings.cpp`, `runtime/tests/test_settings.cpp` (T1).
- `runtime/src/psp_event_loop.cpp` (T1 bindings, T3 keyboard routing / demo).
- `runtime/include/psp_text_edit.h`, `runtime/src/psp_text_edit.cpp`, `runtime/tests/test_text_edit.cpp`,
  `runtime/include/psp_osk_session.h`, `runtime/src/psp_osk_session.cpp` (T2 cursor).
- `runtime/include/psp_osk_keyboard.h`, `runtime/src/psp_osk_keyboard.cpp`, `runtime/tests/test_osk_keyboard.cpp` (T2).
- `runtime/include/psp_osk_skin.h`, `runtime/src/psp_osk_skin.cpp`, `runtime/assets/osk_skins/{deck,light,patapon}.json`,
  `runtime/src/psp_osk_overlay.cpp` (T3).
- `runtime/CMakeLists.txt` (T1–T3), `games/patapon3/scripts/p3.sh` (T3 copies skins),
  `docs/contracts/settings-v1.md`, `docs/ENV_FLAGS.md`, `ARCHITECTURE.md`, `README.md` (T1, T3).

launcher (`DECO/launcher`):
- `src-tauri/core_options.json`, `src-tauri/src/options.rs`, `src-tauri/src/settings.rs`,
  `src-tauri/src/controls.rs` (new), `src-tauri/src/gamepad.rs`, `src-tauri/src/lib.rs`,
  `scripts/make-portable.mjs` (T4).
- `src/types.ts`, `src/api.ts`, `src/controls.ts` (new), `src/controls.test.ts` (new),
  `src/components/ControlsEditor.tsx` (new), `src/screens/Options.tsx`, `src/screens/Options.test.tsx`,
  `src/styles.css`, `lang/{es,en}.json`, `README.md` (T5).

Gates (exact commands):
- runtime: `cmake --build C:/Users/torso/Documents/deco/build/rtw 2>&1 | tail -2 && ctest --test-dir C:/Users/torso/Documents/deco/build/rtw 2>&1 | tail -3`
- test_p3: `cd /c/Users/torso/Documents/deco/psprecomp && bash games/patapon3/scripts/test_p3.sh 2>&1 | tail -3`
- launcher Rust: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q 2>&1 | grep -E "^test result|FAILED|panicked"`
- launcher UI: `cd /c/Users/torso/Documents/deco/launcher && npx vitest run 2>&1 | tail -5` and `npx tsc --noEmit`
- 1.1.4 tour: `cd /c/Users/torso/Documents/deco && TOUR_RT_PORT=9998 PSPRECOMP_RTC_FIXED=2026-10-11T18:32:00 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/<name>` then `python -I build/tools/rt_regress.py build/tour/rebuild114_t1 build/tour/<name>` → `RT_REGRESS PASS` (the hideout checkpoints 00/14/15 are flaky: rerun once before blaming a change).

Baseline (2026-10-18, record the real numbers in the T1 report): ctest 75/75, test_p3 12/12, launcher
cargo 66 passed + 1 ignored, Vitest 35/35.

---

### Task 1: `ControlMap`, `[controls]` → `PSPRECOMP_CONTROLS`, runtime wiring (psprecomp)

**Files:**
- Create: `runtime/include/psp_controls.h`, `runtime/src/psp_controls.cpp`, `runtime/tests/test_controls.cpp`
- Modify: `runtime/src/psp_settings.cpp`, `runtime/tests/test_settings.cpp`, `runtime/src/psp_event_loop.cpp`,
  `runtime/CMakeLists.txt`, `docs/contracts/settings-v1.md`, `docs/ENV_FLAGS.md`

**Interfaces:**
- Produces: `struct ControlAction { const char* name; uint32_t mask; const char* keys; const char* pad; };`
  `const std::vector<ControlAction>& control_actions();`
  `using KeyLookup = std::function<int32_t(const std::string&)>;` (0 = unknown)
  `using PadLookup = std::function<int(const std::string&)>;` (-1 = unknown)
  `class ControlMap { static ControlMap build(const std::string& spec, const KeyLookup&, const PadLookup&, std::vector<std::string>* log); uint32_t key_mask(int32_t) const; uint32_t pad_mask(int) const; std::string describe() const; };`
  env `PSPRECOMP_CONTROLS`, `PSPRECOMP_OSK_SKIN` (resolved by the settings reader; the skin is used in T3).

- [ ] **Step 1: Failing test.** Create `runtime/tests/test_controls.cpp`:

```cpp
// Unit tests for psp_controls (docs/contracts/settings-v1.md [controls]): defaults equal the old fixed
// tables, PSPRECOMP_CONTROLS parsing, invalid / reserved / duplicate entries.
#include "psp_controls.h"
#include "psp_pad.h"

#include <cstdio>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// A tiny stand-in for SDL's name tables (keycodes are arbitrary but distinct).
static const std::map<std::string, int32_t> kKeys = {
    {"up", 1}, {"down", 2}, {"left", 3}, {"right", 4}, {"x", 5}, {"return", 6}, {"z", 7}, {"a", 8},
    {"s", 9}, {"q", 10}, {"w", 11}, {"space", 12}, {"v", 13}, {"tab", 14}, {"k", 15}, {"f1", 16},
    {"f11", 17}, {"left shift", 18}};
static const std::map<std::string, int> kPads = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"guide", 5}, {"start", 6},
    {"leftstick", 7}, {"rightstick", 8}, {"leftshoulder", 9}, {"rightshoulder", 10},
    {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14}};
static std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
static const KeyLookup kKey = [](const std::string& n) { auto it = kKeys.find(lower(n)); return it == kKeys.end() ? 0 : it->second; };
static const PadLookup kPad = [](const std::string& n) { auto it = kPads.find(lower(n)); return it == kPads.end() ? -1 : it->second; };

static bool logged(const std::vector<std::string>& log, const char* needle) {
    for (const auto& l : log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void defaults_are_todays_mapping() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("", kKey, kPad, &log);
    CHECK(log.empty(), "no log for the defaults");
    CHECK(m.key_mask(1) == 0x0010 && m.key_mask(2) == 0x0040 && m.key_mask(3) == 0x0080 && m.key_mask(4) == 0x0020, "arrows");
    CHECK(m.key_mask(5) == 0x4000 && m.key_mask(6) == 0x4000, "X and Return = cross");
    CHECK(m.key_mask(7) == 0x2000 && m.key_mask(8) == 0x8000 && m.key_mask(9) == 0x1000, "Z A S");
    CHECK(m.key_mask(10) == 0x0100 && m.key_mask(11) == 0x0200, "Q W");
    CHECK(m.key_mask(12) == 0x0008 && m.key_mask(13) == 0x0001 && m.key_mask(14) == 0x0001, "Space V Tab");
    CHECK(m.key_mask(15) == 0, "K unbound");
    for (int b = 0; b <= 20; b++) CHECK(m.pad_mask(b) == pad_button_to_psp(b), "pad default equals pad_button_to_psp");
    CHECK(m.describe().rfind("up=Up|dpup;down=Down|dpdown;", 0) == 0, "describe starts in contract order");
}

static void spec_replaces_named_actions_only() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("cross=K|y;triangle=S|", kKey, kPad, &log);
    CHECK(log.empty(), "valid spec, no log");
    CHECK(m.key_mask(15) == 0x4000, "K is cross");
    CHECK(m.key_mask(5) == 0 && m.key_mask(6) == 0, "X / Return no longer cross");
    CHECK(m.pad_mask(3) == 0x4000, "pad y is cross");
    CHECK(m.pad_mask(0) == 0, "pad a unbound now");
    CHECK(m.key_mask(9) == 0x1000, "triangle keeps S");
    CHECK(m.key_mask(7) == 0x2000, "circle untouched");
}

static void invalid_entries_fall_back() {
    std::vector<std::string> log;
    ControlMap m = ControlMap::build("cross=Nope|a", kKey, kPad, &log);
    CHECK(m.key_mask(5) == 0x4000 && m.key_mask(6) == 0x4000, "unknown key name -> default keys");
    CHECK(logged(log, "[CONTROLS] cross"), "logged");
    log.clear();
    m = ControlMap::build("cross=X,Return,K|a", kKey, kPad, &log);
    CHECK(m.key_mask(15) == 0 && m.key_mask(5) == 0x4000, "third key -> defaults");
    CHECK(logged(log, "more than two keys"), "too many keys logged");
    log.clear();
    m = ControlMap::build("start=F1|guide", kKey, kPad, &log);
    CHECK(m.key_mask(16) == 0 && m.key_mask(12) == 0x0008, "F1 is reserved");
    CHECK(m.pad_mask(5) == 0 && m.pad_mask(6) == 0x0008, "guide is reserved");
    CHECK(logged(log, "reserved"), "reserved logged");
    log.clear();
    m = ControlMap::build("cross=X|nope;garbage;bogus=K|a", kKey, kPad, &log);
    CHECK(m.pad_mask(0) == 0x4000, "unknown pad -> default pad");
    CHECK(logged(log, "bogus") && logged(log, "garbage"), "unknown action and malformed entry logged");
}

static void duplicates_keep_the_first_action() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("circle=X|a", kKey, kPad, &log);
    CHECK(m.key_mask(5) == 0x4000, "X stays cross (cross comes first)");
    CHECK(m.pad_mask(0) == 0x4000, "a stays cross");
    CHECK(logged(log, "already used by cross"), "duplicate logged");
    CHECK(m.key_mask(7) == 0, "Z no longer circle (circle was replaced by X, which was dropped)");
}

static void empty_unbinds() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("select=|", kKey, kPad, &log);
    CHECK(log.empty(), "empty is valid");
    CHECK(m.key_mask(13) == 0 && m.key_mask(14) == 0 && m.pad_mask(4) == 0, "select fully unbound");
}

int main() {
    defaults_are_todays_mapping();
    spec_replaces_named_actions_only();
    invalid_entries_fall_back();
    duplicates_keep_the_first_action();
    empty_unbinds();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
```
  CMake (after the `pad_tests` block, line ~728):

```cmake
add_executable(test_controls tests/test_controls.cpp src/psp_controls.cpp src/psp_pad.cpp)
target_include_directories(test_controls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_controls PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME controls_tests COMMAND test_controls)
```
  Run: `cmake --build C:/Users/torso/Documents/deco/build/rtw --target test_controls 2>&1 | tail -3` → FAIL (no `psp_controls.h`).

- [ ] **Step 2: Implementation.** `runtime/include/psp_controls.h`:

```cpp
#pragma once
// PSP button bindings (pure): up to two keyboard keys and one gamepad button per PSP button, from
// PSPRECOMP_CONTROLS (docs/contracts/settings-v1.md, [controls]). Key and pad names are resolved by
// functions the caller passes in (SDL in the runtime, a small table in the tests).
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct ControlAction {
    const char* name;  // contract key prefix ("cross" -> cross_keys / cross_pad)
    uint32_t mask;     // PSP sceCtrl button
    const char* keys;  // default keys, comma separated SDL key names
    const char* pad;   // default SDL GameController button name
};

// The 12 actions in contract order (the order decides who keeps a duplicate), with today's defaults.
const std::vector<ControlAction>& control_actions();

using KeyLookup = std::function<int32_t(const std::string&)>;  // name -> keycode, 0 = unknown
using PadLookup = std::function<int(const std::string&)>;      // name -> button, -1 = unknown

class ControlMap {
public:
    // `spec` is PSPRECOMP_CONTROLS ("action=key1,key2|pad;..."); actions it does not name keep their
    // defaults. An invalid part falls back to that action's default and adds a "[CONTROLS] ..." line.
    static ControlMap build(const std::string& spec, const KeyLookup& key, const PadLookup& pad,
                            std::vector<std::string>* log);
    uint32_t key_mask(int32_t keycode) const;  // 0 when unbound
    uint32_t pad_mask(int button) const;       // 0 when unbound
    std::string describe() const;              // the effective map, canonical form, for the log

private:
    std::map<int32_t, uint32_t> keys_;
    std::map<int, uint32_t> pads_;
    std::vector<std::string> desc_;
};
```
  `runtime/src/psp_controls.cpp`:

```cpp
#include "psp_controls.h"

#include <algorithm>
#include <cctype>

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t c = s.find(sep, a);
        if (c == std::string::npos) c = s.size();
        out.push_back(trim(s.substr(a, c - a)));
        a = c + 1;
    }
    return out;
}

std::vector<std::string> names(const std::string& keys) {
    std::vector<std::string> out;
    for (const std::string& n : split(keys, ',')) if (!n.empty()) out.push_back(n);
    return out;
}

bool reserved_key(const std::string& n) {
    const std::string l = lower(n);
    return l == "f1" || l == "f11";
}

}  // namespace

const std::vector<ControlAction>& control_actions() {
    static const std::vector<ControlAction> a = {
        {"up", 0x0010, "Up", "dpup"},          {"down", 0x0040, "Down", "dpdown"},
        {"left", 0x0080, "Left", "dpleft"},    {"right", 0x0020, "Right", "dpright"},
        {"cross", 0x4000, "X, Return", "a"},   {"circle", 0x2000, "Z", "b"},
        {"square", 0x8000, "A", "x"},          {"triangle", 0x1000, "S", "y"},
        {"l", 0x0100, "Q", "leftshoulder"},    {"r", 0x0200, "W", "rightshoulder"},
        {"start", 0x0008, "Space", "start"},   {"select", 0x0001, "V, Tab", "back"},
    };
    return a;
}

ControlMap ControlMap::build(const std::string& spec, const KeyLookup& key, const PadLookup& pad,
                             std::vector<std::string>* log) {
    std::map<std::string, std::pair<std::string, std::string>> given;
    for (const std::string& entry : split(spec, ';')) {
        if (entry.empty()) continue;
        const size_t eq = entry.find('=');
        const size_t bar = entry.find('|');
        if (eq == std::string::npos || bar == std::string::npos || bar < eq) {
            log->push_back("[CONTROLS] malformed entry \"" + entry + "\" ignored");
            continue;
        }
        const std::string name = lower(trim(entry.substr(0, eq)));
        const bool known = std::any_of(control_actions().begin(), control_actions().end(),
                                       [&](const ControlAction& a) { return name == a.name; });
        if (!known) {
            log->push_back("[CONTROLS] unknown action \"" + name + "\" ignored");
            continue;
        }
        given[name] = {entry.substr(eq + 1, bar - eq - 1), trim(entry.substr(bar + 1))};
    }

    ControlMap m;
    std::map<int32_t, std::string> key_owner;
    std::map<int, std::string> pad_owner;
    for (const ControlAction& a : control_actions()) {
        std::string keys = a.keys, padname = a.pad;
        if (auto it = given.find(a.name); it != given.end()) {
            keys = it->second.first;
            padname = it->second.second;
        }
        // Keys: up to two, all known and not reserved, else this action's defaults.
        std::vector<std::string> ks = names(keys);
        std::string problem;
        if (ks.size() > 2) problem = "more than two keys";
        for (const std::string& n : ks) {
            if (!problem.empty()) break;
            if (reserved_key(n)) problem = "key " + n + " is reserved";
            else if (key(n) == 0) problem = "unknown key " + n;
        }
        if (!problem.empty()) {
            log->push_back(std::string("[CONTROLS] ") + a.name + ": " + problem + ", using \"" + a.keys + "\"");
            ks = names(a.keys);
        }
        std::vector<std::string> kept;
        for (const std::string& n : ks) {
            const int32_t code = key(n);
            if (code == 0) continue;
            if (auto o = key_owner.find(code); o != key_owner.end()) {
                log->push_back(std::string("[CONTROLS] ") + a.name + ": key " + n + " already used by " +
                               o->second + ", dropped");
                continue;
            }
            key_owner[code] = a.name;
            m.keys_[code] = a.mask;
            kept.push_back(n);
        }
        // Pad: one button (or none), known and not guide, else the default.
        if (!padname.empty() && (pad(padname) < 0 || lower(padname) == "guide")) {
            log->push_back(std::string("[CONTROLS] ") + a.name + ": " +
                           (lower(padname) == "guide" ? "pad button guide is reserved" : "unknown pad button " + padname) +
                           ", using \"" + a.pad + "\"");
            padname = a.pad;
        }
        if (!padname.empty()) {
            const int b = pad(padname);
            if (auto o = pad_owner.find(b); o != pad_owner.end()) {
                log->push_back(std::string("[CONTROLS] ") + a.name + ": pad button " + padname +
                               " already used by " + o->second + ", dropped");
                padname.clear();
            } else if (b >= 0) {
                pad_owner[b] = a.name;
                m.pads_[b] = a.mask;
            }
        }
        std::string joined;
        for (const std::string& n : kept) joined += (joined.empty() ? "" : ",") + n;
        m.desc_.push_back(std::string(a.name) + "=" + joined + "|" + padname);
    }
    return m;
}

uint32_t ControlMap::key_mask(int32_t keycode) const {
    auto it = keys_.find(keycode);
    return it == keys_.end() ? 0 : it->second;
}

uint32_t ControlMap::pad_mask(int button) const {
    auto it = pads_.find(button);
    return it == pads_.end() ? 0 : it->second;
}

std::string ControlMap::describe() const {
    std::string s;
    for (const std::string& d : desc_) s += (s.empty() ? "" : ";") + d;
    return s;
}
```
  Note for the describe test: names are kept as written (`Up`, `dpup`), defaults written as in
  `control_actions()` with `", "` → the joined form drops the space (`X,Return`).

- [ ] **Step 3: Settings reader.** In `runtime/src/psp_settings.cpp` add `#include "psp_controls.h"`, and in
  `psp_settings_resolve`, right before the `for (const auto& s : ini.values)` unknown-key loop:

```cpp
    // [controls] -> PSPRECOMP_CONTROLS, validated by the runtime's ControlMap (psp_controls.h): only
    // the actions the file names, the missing half of an action keeps its default.
    {
        std::string spec;
        for (const ControlAction& a : control_actions()) {
            const std::string kk = std::string(a.name) + "_keys", pk = std::string(a.name) + "_pad";
            known.insert("controls." + kk);
            known.insert("controls." + pk);
            const std::string* keys = ini.get("controls", kk);
            const std::string* pad = ini.get("controls", pk);
            if (keys == nullptr && pad == nullptr) continue;
            spec += (spec.empty() ? "" : ";") + std::string(a.name) + "=" + (keys ? *keys : a.keys) + "|" +
                    (pad ? *pad : a.pad);
        }
        if (!spec.empty()) emit("PSPRECOMP_CONTROLS", spec, false);
    }
    // [display] osk_skin -> PSPRECOMP_OSK_SKIN (<ini folder>/skins/osk/<name>.json).
    known.insert("display.osk_skin");
    if (const std::string* skin = ini.get("display", "osk_skin")) {
        const bool ok = !skin->empty() && std::all_of(skin->begin(), skin->end(), [](unsigned char c) {
            return std::islower(c) || std::isdigit(c) || c == '_' || c == '-';
        });
        if (ok) emit("PSPRECOMP_OSK_SKIN", resolve_path(base_dir, "skins/osk/" + *skin + ".json"), false);
        else r.log.push_back("[SETTINGS] display.osk_skin: invalid name \"" + *skin + "\", using the built-in skin");
    }
```
  CMake: `test_settings` gains `src/psp_controls.cpp` in its sources (line ~652).
  Add to `runtime/tests/test_settings.cpp` before `int main()` and call it from `main`:

```cpp
static void controls_and_skin() {
    SettingsResolution r = psp_settings_resolve(
        "[controls]\ncross_keys = K, Return\nsquare_pad = y\nbogus_keys = J\n[display]\nosk_skin = patapon\n",
        "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_CONTROLS") == "cross=K, Return|a;square=A|y", "only named actions, other half default");
    CHECK(val(r, "PSPRECOMP_OSK_SKIN") == "C:/P/skins/osk/patapon.json", "skin path resolved");
    CHECK(logged(r, "unknown key controls.bogus_keys"), "unknown controls key logged");
    r = psp_settings_resolve("[display]\nosk_skin = ../x\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_OSK_SKIN") == "<unset>" && logged(r, "display.osk_skin"), "bad skin name refused");
    r = psp_settings_resolve("[display]\nrender_scale = 4\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_CONTROLS") == "<unset>", "no [controls] -> no variable");
    const EnvGetter env = [](const char* n) -> const char* {
        return std::string(n) == "PSPRECOMP_CONTROLS" ? "cross=X|a" : nullptr;
    };
    r = psp_settings_resolve("[controls]\ncross_keys = K\n", "", "C:/P", env);
    CHECK(val(r, "PSPRECOMP_CONTROLS") == "<unset>" && logged(r, "PSPRECOMP_CONTROLS already set"), "env wins");
}
```

- [ ] **Step 4: Runtime wiring** (`runtime/src/psp_event_loop.cpp`):
  - add `#include "psp_controls.h"`; replace the whole `key_to_psp_button` function and its comment block
    with:

```cpp
// ---------------------------------------------------------------------------
// Keyboard and gamepad -> PSP buttons: PSPRECOMP_CONTROLS (settings.ini [controls], psp_controls.h),
// today's mapping by default. Built once SDL is up (it resolves SDL key / button names).
// ---------------------------------------------------------------------------
static ControlMap g_controls;

static void load_controls() {
    const char* spec = std::getenv("PSPRECOMP_CONTROLS");
    std::vector<std::string> log;
    g_controls = ControlMap::build(
        spec ? spec : "", [](const std::string& n) { return static_cast<int32_t>(SDL_GetKeyFromName(n.c_str())); },
        [](const std::string& n) { return static_cast<int>(SDL_GameControllerGetButtonFromString(n.c_str())); },
        &log);
    for (const std::string& l : log) std::fprintf(stderr, "%s\n", l.c_str());
    std::fprintf(stderr, "[CONTROLS] %s%s\n", spec ? "" : "defaults: ", g_controls.describe().c_str());
}
```
  - `handle_controller_event`: `const uint32_t mask = pad_button_to_psp(ev.cbutton.button);` →
    `const uint32_t mask = g_controls.pad_mask(ev.cbutton.button);`
  - `handle_sdl_event`: `uint32_t bit = key_to_psp_button(ev.key.keysym.sym);` →
    `uint32_t bit = g_controls.key_mask(static_cast<int32_t>(ev.key.keysym.sym));`
  - in `psp_runtime_init_sdl`, right after the `SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)` block, call
    `load_controls();`.
  `pad_button_to_psp` stays in `psp_pad.cpp` (the defaults test compares against it).

- [ ] **Step 5: Docs.** `docs/contracts/settings-v1.md`: replace the reserved `[controls]` line with the
  spec's §1 block (keys, names, fixed buttons, validation, `PSPRECOMP_CONTROLS` form) and add
  `display.osk_skin` to the core key table (`PSPRECOMP_OSK_SKIN`, default `deck`). `docs/ENV_FLAGS.md`: new
  sections `PSPRECOMP_CONTROLS` (format, defaults, `[CONTROLS]` log lines) and `PSPRECOMP_OSK_SKIN` (skin
  file; missing → built-in Deck skin), same layout as the neighbouring entries.

- [ ] **Step 6: Gates.** Runtime gate → ctest **76/76** (75 + `controls_tests`); `test_settings` passes;
  test_p3 12/12. Boot check without the game window staying open:
  `cd /c/Users/torso/Documents/deco/build/rtw && PSPRECOMP_DEBUG_PORT=9998 PSPRECOMP_CONTROLS="cross=K|y" PSPRECOMP_FULLSCREEN=0 timeout 20 ./psprecomp_runtime.exe 2>&1 | grep "\[CONTROLS\]"`
  → `[CONTROLS] up=Up|dpup;…;cross=K|y;…`. Report (no commit). Claude commits
  `feat(runtime): remappable keys and gamepad buttons (settings.ini [controls])`.

---

### Task 2: Text cursor + Steam Deck style keyboard model (psprecomp, pure)

**Files:**
- Modify: `runtime/include/psp_text_edit.h`, `runtime/src/psp_text_edit.cpp`, `runtime/tests/test_text_edit.cpp`,
  `runtime/include/psp_osk_session.h`, `runtime/src/psp_osk_session.cpp`, `runtime/tests/test_osk_session.cpp`
- Create: `runtime/include/psp_osk_keyboard.h`, `runtime/src/psp_osk_keyboard.cpp`, `runtime/tests/test_osk_keyboard.cpp`
- Modify: `runtime/CMakeLists.txt`

**Interfaces:**
- Produces: `TextEdit::move_cursor(int)`, `TextEdit::cursor() const` (code points before the cursor);
  `OskSession::move_cursor(int)`, `OskSession::View::cursor`; `struct OskTarget` (virtual `type`,
  `backspace`, `move_cursor`, `accept`, `cancel`); `enum class OskAction`; `enum class OskKeyKind`;
  `struct OskKey { OskKeyKind kind; std::string label, text; float width; }`;
  `class OskKeyboard { explicit OskKeyboard(const std::string& lang); void act(OskAction, OskTarget&);
  const std::vector<std::vector<OskKey>>& rows() const; int focus_row() const; int focus_col() const;
  bool shift() const; bool caps() const; bool symbols() const; const std::string& pending_accent() const; }`;
  `class OskStickRepeat { std::optional<OskAction> update(float x, float y, uint64_t now_ms); }`.

- [ ] **Step 1: Failing tests.**
  `runtime/tests/test_text_edit.cpp` gains (and `main` calls it):

```cpp
static void cursor_editing() {
    TextEdit e;
    e.reset("Pon", 5);
    CHECK(e.cursor() == 3, "cursor starts at the end");
    e.move_cursor(-2);
    e.insert("a");
    CHECK(e.utf8() == "Paon" && e.cursor() == 2, "insert at the cursor");
    e.backspace();
    CHECK(e.utf8() == "Pon" && e.cursor() == 1, "backspace before the cursor");
    e.move_cursor(-9);
    e.backspace();
    CHECK(e.utf8() == "Pon" && e.cursor() == 0, "backspace at the start does nothing");
    e.move_cursor(99);
    CHECK(e.cursor() == 3, "cursor clamps at the end");
    e.insert("ñañá");  // limit 5: only two more characters
    CHECK(e.utf8() == "Ponña" && e.cursor() == 5, "limit counted in characters at the cursor");
}
```
  `runtime/tests/test_osk_session.cpp` gains a check that `move_cursor(-1)` then `type("x")` inserts before
  the last character and `view().cursor` reports it (write it in the file's existing style).
  Create `runtime/tests/test_osk_keyboard.cpp`:

```cpp
// Unit tests for psp_osk_keyboard: the Steam Deck style key grid, navigation, layers, shift / caps,
// accents and the edits it sends to its target.
#include "psp_osk_keyboard.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

struct Fake : OskTarget {
    std::string text;
    int cursor_moves = 0, accepted = 0, cancelled = 0;
    void type(const std::string& s) override { text += s; }
    void backspace() override {
        if (text.empty()) return;
        size_t i = text.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i--;
        text.erase(i);
    }
    void move_cursor(int d) override { cursor_moves += d; }
    void accept() override { accepted++; }
    void cancel() override { cancelled++; }
};

static const OskKey& focused(const OskKeyboard& k) { return k.rows()[k.focus_row()][k.focus_col()]; }

// Moves the focus to the key labelled `label` on the current layer (test helper, uses only actions).
static bool go_to(OskKeyboard& k, Fake& f, const std::string& label) {
    for (int guard = 0; guard < 200; guard++) {
        if (focused(k).label == label) return true;
        k.act(OskAction::Right, f);
        if (k.focus_col() == 0) k.act(OskAction::Down, f);
    }
    return false;
}

static void starts_on_q_and_types() {
    OskKeyboard k("es");
    Fake f;
    CHECK(focused(k).label == "q", "focus starts on q");
    k.act(OskAction::Press, f);
    CHECK(f.text == "q", "Press types the focused key");
    CHECK(go_to(k, f, "ñ"), "es layout has ñ");
    k.act(OskAction::Press, f);
    CHECK(f.text == "qñ", "ñ typed as UTF-8");
}

static void navigation_wraps_and_snaps() {
    OskKeyboard k("es");
    Fake f;
    k.act(OskAction::Left, f);
    CHECK(k.focus_row() == 1 && k.focus_col() == static_cast<int>(k.rows()[1].size()) - 1, "left wraps in the row");
    k.act(OskAction::Up, f);
    CHECK(k.focus_row() == 0, "up to the number row");
    k.act(OskAction::Up, f);
    CHECK(k.focus_row() == static_cast<int>(k.rows().size()) - 1, "up wraps to the bottom row");
    CHECK(focused(k).kind == OskKeyKind::Cancel || focused(k).kind == OskKeyKind::Right ||
              focused(k).kind == OskKeyKind::Space, "snapped to the bottom row key under it");
}

static void shift_then_caps() {
    OskKeyboard k("en");
    Fake f;
    k.act(OskAction::Shift, f);
    CHECK(k.shift() && !k.caps() && focused(k).label == "Q", "shift shows upper case");
    k.act(OskAction::Press, f);
    k.act(OskAction::Press, f);
    CHECK(f.text == "Qq" && !k.shift(), "shift is one letter");
    k.act(OskAction::Shift, f);
    k.act(OskAction::Shift, f);
    CHECK(k.caps(), "double shift = caps");
    k.act(OskAction::Press, f);
    k.act(OskAction::Press, f);
    CHECK(f.text == "QqQQ" && k.caps(), "caps stays");
    k.act(OskAction::Shift, f);
    CHECK(!k.caps() && !k.shift(), "third shift turns it off");
}

static void accents() {
    OskKeyboard k("es");
    Fake f;
    CHECK(go_to(k, f, "´"), "accent key on the letters layer");
    k.act(OskAction::Press, f);
    CHECK(k.pending_accent() == "´" && f.text.empty(), "accent waits for a vowel");
    CHECK(go_to(k, f, "e"), "find e");
    k.act(OskAction::Press, f);
    CHECK(f.text == "é" && k.pending_accent().empty(), "´ + e = é");
    go_to(k, f, "´");
    k.act(OskAction::Press, f);
    k.act(OskAction::Shift, f);
    go_to(k, f, "O");
    k.act(OskAction::Press, f);
    CHECK(f.text == "éÓ", "´ + shift o = Ó");
    go_to(k, f, "´");
    k.act(OskAction::Press, f);
    go_to(k, f, "t");
    k.act(OskAction::Press, f);
    CHECK(f.text == "éÓ´t", "´ + consonant types both");
    k.act(OskAction::Layer, f);
    CHECK(k.symbols() && go_to(k, f, "¨"), "diaeresis on the symbols layer");
    k.act(OskAction::Press, f);
    k.act(OskAction::Layer, f);
    go_to(k, f, "u");
    k.act(OskAction::Press, f);
    CHECK(f.text == "éÓ´tü", "¨ + u = ü");
}

static void symbols_and_special_keys() {
    OskKeyboard k("es");
    Fake f;
    k.act(OskAction::Layer, f);
    CHECK(go_to(k, f, "¿"), "¿ on the symbols layer");
    k.act(OskAction::Press, f);
    k.act(OskAction::Space, f);
    k.act(OskAction::Backspace, f);
    CHECK(f.text == "¿", "space then backspace");
    k.act(OskAction::CursorLeft, f);
    k.act(OskAction::CursorRight, f);
    k.act(OskAction::CursorRight, f);
    CHECK(f.cursor_moves == 1, "cursor actions reach the target");
    k.act(OskAction::Accept, f);
    CHECK(f.accepted == 1, "Start accepts");
    k.act(OskAction::Layer, f);
    CHECK(!k.symbols(), "layer toggles back");
    CHECK(go_to(k, f, "Cancelar"), "cancel key (es label)");
    k.act(OskAction::Press, f);
    CHECK(f.cancelled == 1, "cancel key cancels");
    OskKeyboard en("en");
    Fake g;
    CHECK(go_to(en, g, "Cancel"), "cancel key (en label)");
}

static void stick_repeat() {
    OskStickRepeat s;
    CHECK(s.update(0.0f, -0.9f, 0) == OskAction::Up, "first push moves (SDL: -y is up)");
    CHECK(!s.update(0.0f, -0.9f, 300).has_value(), "no repeat before 400 ms");
    CHECK(s.update(0.0f, -0.9f, 400) == OskAction::Up, "repeat");
    CHECK(s.update(0.0f, -0.9f, 520) == OskAction::Up, "then every 120 ms");
    CHECK(!s.update(0.0f, 0.1f, 530).has_value(), "centre");
    CHECK(s.update(0.8f, 0.0f, 600) == OskAction::Right, "right");
}

int main() {
    starts_on_q_and_types();
    navigation_wraps_and_snaps();
    shift_then_caps();
    accents();
    symbols_and_special_keys();
    stick_repeat();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
```
  CMake (next to `osk_session_tests`):

```cmake
add_executable(test_osk_keyboard tests/test_osk_keyboard.cpp src/psp_osk_keyboard.cpp)
target_include_directories(test_osk_keyboard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_osk_keyboard PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME osk_keyboard_tests COMMAND test_osk_keyboard)
```
  and `src/psp_osk_keyboard.cpp` joins the runtime source list next to `src/psp_osk_session.cpp`.
  Run the three targets → FAIL to build.

- [ ] **Step 2: Cursor.** `psp_text_edit.h`: add `void move_cursor(int delta);`,
  `uint32_t cursor() const { return static_cast<uint32_t>(cursor_); }` and member `size_t cursor_ = 0;`;
  update the class comment ("insert / backspace work at the cursor"). `psp_text_edit.cpp`: `reset` sets
  `cursor_ = chars_.size()` after inserting the initial text (insert first with the cursor at 0 → simplest:
  `chars_.clear(); cursor_ = 0; insert(initial);` leaves the cursor at the end because insert advances it);
  `insert` inserts each accepted char at `chars_.begin() + cursor_` and increments `cursor_`; `backspace`
  erases `chars_[cursor_ - 1]` when `cursor_ > 0` and decrements; `move_cursor(d)` clamps
  `cursor_ + d` into `[0, chars_.size()]` (use signed arithmetic). `psp_osk_session.h/.cpp`: add
  `void move_cursor(int delta);` (Editing only, like `backspace`) and `uint32_t cursor = 0;` in `View`,
  filled from `edit_.cursor()`.

- [ ] **Step 3: Keyboard model.** `runtime/include/psp_osk_keyboard.h`:

```cpp
#pragma once
// Steam Deck style on-screen keyboard (pure): a key grid with two layers (letters; numbers and
// symbols), shift (one letter) / caps (double shift), an accent key (´ á é í ó ú, ¨ ä ë ï ö ü) and
// the special keys Backspace, Space, cursor ← →, Accept and Cancel. Gamepad actions in; edits out
// through an OskTarget (the OskSession in the runtime, a fake in the tests). Layouts: "es" (QWERTY +
// ñ) and "en". Main thread only.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct OskTarget {
    virtual ~OskTarget() = default;
    virtual void type(const std::string& utf8) = 0;
    virtual void backspace() = 0;
    virtual void move_cursor(int delta) = 0;
    virtual void accept() = 0;
    virtual void cancel() = 0;
};

enum class OskKeyKind { Char, Shift, Backspace, Space, Layer, Accent, Left, Right, Accept, Cancel };

struct OskKey {
    OskKeyKind kind;
    std::string label;  // what the key shows (follows shift / caps for letters)
    std::string text;   // Char: what it types (lower case); Accent: "´" or "¨"
    float width;        // in key units (a letter is 1)
};

enum class OskAction { Up, Down, Left, Right, Press, Backspace, Shift, Space, CursorLeft, CursorRight,
                       Layer, Accept, Cancel };

class OskKeyboard {
public:
    explicit OskKeyboard(const std::string& lang = "es");
    void act(OskAction a, OskTarget& t);
    const std::vector<std::vector<OskKey>>& rows() const { return view_; }
    int focus_row() const { return row_; }
    int focus_col() const { return col_; }
    bool shift() const { return shift_; }
    bool caps() const { return caps_; }
    bool symbols() const { return symbols_; }
    const std::string& pending_accent() const { return accent_; }

private:
    void rebuild();                       // view_ from the layer + case
    void press(const OskKey& k, OskTarget& t);
    void type_char(const std::string& c, OskTarget& t);
    void move_vertical(int dir);
    std::vector<std::vector<OskKey>> letters_, symbols_rows_, view_;
    bool en_ = false, shift_ = false, caps_ = false, symbols_ = false;
    std::string accent_;
    int row_ = 1, col_ = 0;
};

// Left-stick navigation with key repeat (SDL axes: -y is up), like the launcher's StickRepeat.
class OskStickRepeat {
public:
    std::optional<OskAction> update(float x, float y, uint64_t now_ms);

private:
    std::optional<OskAction> dir_;
    uint64_t next_ms_ = 0;
};
```
  `runtime/src/psp_osk_keyboard.cpp`:

```cpp
#include "psp_osk_keyboard.h"

#include <cmath>

namespace {

OskKey ch(const char* s) { return {OskKeyKind::Char, s, s, 1.0f}; }

// Upper case of the letters the layouts hold (ASCII, ñ, accented vowels).
std::string upper(const std::string& s) {
    static const char* const pairs[][2] = {{"ñ", "Ñ"}, {"á", "Á"}, {"é", "É"}, {"í", "Í"}, {"ó", "Ó"},
                                           {"ú", "Ú"}, {"ä", "Ä"}, {"ë", "Ë"}, {"ï", "Ï"}, {"ö", "Ö"},
                                           {"ü", "Ü"}};
    for (const auto& p : pairs) if (s == p[0]) return p[1];
    if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') return std::string(1, static_cast<char>(s[0] - 32));
    return s;
}

std::string accented(const std::string& accent, const std::string& vowel) {
    static const char* const acute[][2] = {{"a", "á"}, {"e", "é"}, {"i", "í"}, {"o", "ó"}, {"u", "ú"}};
    static const char* const dia[][2] = {{"a", "ä"}, {"e", "ë"}, {"i", "ï"}, {"o", "ö"}, {"u", "ü"}};
    for (const auto& p : accent == "´" ? acute : dia) if (vowel == p[0]) return p[1];
    return "";
}

}  // namespace

OskKeyboard::OskKeyboard(const std::string& lang) : en_(lang == "en") {
    const std::string space = en_ ? "space" : "espacio", ok = en_ ? "OK" : "Aceptar",
                      cancel = en_ ? "Cancel" : "Cancelar";
    letters_ = {
        {ch("1"), ch("2"), ch("3"), ch("4"), ch("5"), ch("6"), ch("7"), ch("8"), ch("9"), ch("0"),
         {OskKeyKind::Backspace, "⌫", "", 1.5f}},
        {ch("q"), ch("w"), ch("e"), ch("r"), ch("t"), ch("y"), ch("u"), ch("i"), ch("o"), ch("p"),
         en_ ? ch("-") : OskKey{OskKeyKind::Accent, "´", "´", 1.0f}},
        {ch("a"), ch("s"), ch("d"), ch("f"), ch("g"), ch("h"), ch("j"), ch("k"), ch("l"),
         en_ ? ch("'") : ch("ñ"), {OskKeyKind::Accept, ok, "", 1.5f}},
        {{OskKeyKind::Shift, "⇧", "", 1.5f}, ch("z"), ch("x"), ch("c"), ch("v"), ch("b"), ch("n"),
         ch("m"), ch(","), ch("."), {OskKeyKind::Shift, "⇧", "", 1.0f}},
        {{OskKeyKind::Layer, "?123", "", 1.5f}, {OskKeyKind::Left, "←", "", 1.0f},
         {OskKeyKind::Space, space, " ", 5.0f}, {OskKeyKind::Right, "→", "", 1.0f},
         {OskKeyKind::Cancel, cancel, "", 2.0f}},
    };
    symbols_rows_ = {
        {ch("1"), ch("2"), ch("3"), ch("4"), ch("5"), ch("6"), ch("7"), ch("8"), ch("9"), ch("0"),
         {OskKeyKind::Backspace, "⌫", "", 1.5f}},
        {ch("@"), ch("#"), ch("€"), ch("%"), ch("&"), ch("*"), ch("("), ch(")"), ch("-"), ch("_"),
         {OskKeyKind::Accent, "¨", "¨", 1.0f}},
        {ch("¿"), ch("?"), ch("¡"), ch("!"), ch("'"), ch("\""), ch(":"), ch(";"), ch("/"), ch("+"),
         {OskKeyKind::Accept, ok, "", 1.5f}},
        {{OskKeyKind::Shift, "⇧", "", 1.5f}, ch("~"), ch("="), ch("["), ch("]"), ch("<"), ch(">"),
         ch("{"), ch("}"), ch(","), {OskKeyKind::Shift, "⇧", "", 1.0f}},
        {{OskKeyKind::Layer, "ABC", "", 1.5f}, {OskKeyKind::Left, "←", "", 1.0f},
         {OskKeyKind::Space, space, " ", 5.0f}, {OskKeyKind::Right, "→", "", 1.0f},
         {OskKeyKind::Cancel, cancel, "", 2.0f}},
    };
    rebuild();
}

void OskKeyboard::rebuild() {
    view_ = symbols_ ? symbols_rows_ : letters_;
    if (shift_ || caps_) {
        for (auto& r : view_) for (auto& k : r) if (k.kind == OskKeyKind::Char) k.label = upper(k.text);
    }
    if (row_ >= static_cast<int>(view_.size())) row_ = 0;
    if (col_ >= static_cast<int>(view_[row_].size())) col_ = static_cast<int>(view_[row_].size()) - 1;
}

void OskKeyboard::type_char(const std::string& c, OskTarget& t) {
    std::string out = (shift_ || caps_) ? upper(c) : c;
    if (!accent_.empty()) {
        const std::string a = accented(accent_, c);
        if (a.empty()) out = accent_ + out;
        else out = (shift_ || caps_) ? upper(a) : a;
        accent_.clear();
    }
    t.type(out);
    if (shift_ && !caps_) {
        shift_ = false;
        rebuild();
    }
}

void OskKeyboard::press(const OskKey& k, OskTarget& t) {
    switch (k.kind) {
    case OskKeyKind::Char: type_char(k.text, t); break;
    case OskKeyKind::Accent:
        if (accent_ == k.text) { t.type(accent_); accent_.clear(); }
        else accent_ = k.text;
        break;
    case OskKeyKind::Shift: act(OskAction::Shift, t); break;
    case OskKeyKind::Backspace: act(OskAction::Backspace, t); break;
    case OskKeyKind::Space: act(OskAction::Space, t); break;
    case OskKeyKind::Layer: act(OskAction::Layer, t); break;
    case OskKeyKind::Left: t.move_cursor(-1); break;
    case OskKeyKind::Right: t.move_cursor(1); break;
    case OskKeyKind::Accept: t.accept(); break;
    case OskKeyKind::Cancel: t.cancel(); break;
    }
}

// Key centre in key units from the row start.
static float centre(const std::vector<OskKey>& r, int col) {
    float x = 0;
    for (int i = 0; i < col; i++) x += r[i].width;
    return x + r[col].width * 0.5f;
}

void OskKeyboard::move_vertical(int dir) {
    const float c = centre(view_[row_], col_);
    const int n = static_cast<int>(view_.size());
    row_ = (row_ + dir + n) % n;
    int best = 0;
    float best_d = 1e9f;
    for (int i = 0; i < static_cast<int>(view_[row_].size()); i++) {
        const float d = std::fabs(centre(view_[row_], i) - c);
        if (d < best_d) { best_d = d; best = i; }
    }
    col_ = best;
}

void OskKeyboard::act(OskAction a, OskTarget& t) {
    const int n = static_cast<int>(view_[row_].size());
    switch (a) {
    case OskAction::Up: move_vertical(-1); break;
    case OskAction::Down: move_vertical(1); break;
    case OskAction::Left: col_ = (col_ - 1 + n) % n; break;
    case OskAction::Right: col_ = (col_ + 1) % n; break;
    case OskAction::Press: press(view_[row_][col_], t); break;
    case OskAction::Backspace:
        if (!accent_.empty()) accent_.clear();
        else t.backspace();
        break;
    case OskAction::Shift:
        if (caps_) { caps_ = false; shift_ = false; }
        else if (shift_) { caps_ = true; shift_ = false; }
        else shift_ = true;
        rebuild();
        break;
    case OskAction::Space:
        if (!accent_.empty()) { t.type(accent_); accent_.clear(); }
        t.type(" ");
        break;
    case OskAction::CursorLeft: t.move_cursor(-1); break;
    case OskAction::CursorRight: t.move_cursor(1); break;
    case OskAction::Layer: symbols_ = !symbols_; rebuild(); break;
    case OskAction::Accept: t.accept(); break;
    case OskAction::Cancel: t.cancel(); break;
    }
}

std::optional<OskAction> OskStickRepeat::update(float x, float y, uint64_t now_ms) {
    std::optional<OskAction> d;
    if (y < -0.6f) d = OskAction::Up;
    else if (y > 0.6f) d = OskAction::Down;
    else if (x < -0.6f) d = OskAction::Left;
    else if (x > 0.6f) d = OskAction::Right;
    if (!d) { dir_.reset(); return std::nullopt; }
    if (dir_ != d) { dir_ = d; next_ms_ = now_ms + 400; return d; }
    if (now_ms >= next_ms_) { next_ms_ = now_ms + 120; return d; }
    return std::nullopt;
}
```
  Notes: in `accents()` the test reaches `´` on the letters layer, which only `es` has; a shifted `O`
  label is found by `go_to(k, f, "O")` because shift changes labels. If any test expectation turns out to
  contradict this code, stop BLOCKED and report — do not change the test.

- [ ] **Step 4: Gates.** Runtime gate → ctest **77/77** (76 + `osk_keyboard_tests`), `text_edit_tests`,
  `osk_session_tests`, `osk_tests` still pass. Report (no commit). Claude commits
  `feat(runtime): Steam Deck style keyboard model and a text cursor`.

---

### Task 3: Skins, keyboard overlay, gamepad routing, demo (psprecomp)

**Files:**
- Create: `runtime/include/psp_osk_skin.h`, `runtime/src/psp_osk_skin.cpp`,
  `runtime/assets/osk_skins/deck.json`, `runtime/assets/osk_skins/light.json`, `runtime/assets/osk_skins/patapon.json`
- Modify: `runtime/tests/test_osk_keyboard.cpp` (skin tests), `runtime/src/psp_osk_overlay.cpp`,
  `runtime/include/psp_osk_overlay.h`, `runtime/src/psp_event_loop.cpp`, `runtime/CMakeLists.txt`,
  `games/patapon3/scripts/p3.sh`, `docs/ENV_FLAGS.md`, `ARCHITECTURE.md`, `README.md`

**Interfaces:**
- Consumes: T2 `OskKeyboard`, `OskStickRepeat`, `OskTarget`, `OskSession::move_cursor`, `View::cursor`.
- Produces: `struct OskColor { float r, g, b, a; }`, `struct OskSkin`, `OskSkin osk_skin_parse(const std::string& json, const std::string& skin_dir, std::vector<std::string>* log)`,
  `bool osk_parse_color(const std::string&, OskColor*)`; `OskKeyboard& psp_osk_keyboard()` (overlay owns it,
  main thread); env `PSPRECOMP_OSK_DEMO`.

- [ ] **Step 1: Failing skin tests** — append to `runtime/tests/test_osk_keyboard.cpp` (include
  `psp_osk_skin.h`, call from `main`):

```cpp
static void skins() {
    OskColor c{};
    CHECK(osk_parse_color("#1A9FFF", &c) && std::fabs(c.b - 1.0f) < 1e-6f && std::fabs(c.a - 1.0f) < 1e-6f, "#RRGGBB");
    CHECK(osk_parse_color("#00000080", &c) && std::fabs(c.a - 128 / 255.0f) < 1e-6f, "#RRGGBBAA");
    CHECK(!osk_parse_color("1A9FFF", &c) && !osk_parse_color("#12345", &c) && !osk_parse_color("#GGGGGG", &c), "bad colours");
    std::vector<std::string> log;
    OskSkin s = osk_skin_parse(R"({"name":"Patapon","key":"#F5F0E6","radius":4,"gap":6,"font":"f.ttf"})", "C:/s", &log);
    CHECK(s.name == "Patapon" && std::fabs(s.key.r - 0xF5 / 255.0f) < 1e-6f && s.radius == 4 && s.gap == 6, "fields read");
    CHECK(s.font == "C:/s/f.ttf", "font relative to the skin folder");
    CHECK(log.empty(), "no log for a good skin");
    const OskSkin d;
    s = osk_skin_parse(R"({"key":"red","radius":-3,"opacity":7})", "C:/s", &log);
    CHECK(std::fabs(s.key.r - d.key.r) < 1e-6f && s.radius == d.radius && s.opacity == d.opacity, "bad fields keep defaults");
    CHECK(log.size() == 3, "one log line per bad field");
    log.clear();
    s = osk_skin_parse("{not json", "C:/s", &log);
    CHECK(s.name == d.name && log.size() == 1, "invalid JSON -> built-in Deck skin");
}
```
  (add `#include <cmath>`, `#include <vector>`, `#include "psp_osk_skin.h"`). CMake: `test_osk_keyboard`
  gains `src/psp_osk_skin.cpp` and the include dir of nlohmann json used by `psp_settings.cpp` (copy the
  include line the `test_settings` target uses for `json.hpp`). Run → FAIL to build.

- [ ] **Step 2: Skin parser.** `runtime/include/psp_osk_skin.h`:

```cpp
#pragma once
// On-screen keyboard skins (pure): colours, corner radius, key gap, opacity and an optional font, from
// a JSON file (settings.ini [display] osk_skin -> PSPRECOMP_OSK_SKIN). Missing or bad fields keep the
// built-in Deck skin's values, each problem adds a log line. Sizes are pixels at 1080p.
#include <string>
#include <vector>

struct OskColor { float r, g, b, a; };

struct OskSkin {
    std::string name = "Deck";
    OskColor background{0.0f, 0.0f, 0.0f, 0.45f};     // over the game image
    OskColor panel{0.106f, 0.157f, 0.220f, 0.96f};    // keyboard panel
    OskColor key{0.165f, 0.247f, 0.353f, 1.0f};       // key caps
    OskColor key_text{0.922f, 0.933f, 0.949f, 1.0f};
    OskColor focus{0.102f, 0.624f, 1.0f, 1.0f};       // focused key
    OskColor focus_text{1.0f, 1.0f, 1.0f, 1.0f};
    OskColor accent{0.102f, 0.624f, 1.0f, 1.0f};      // title, active shift / caps / layer
    float radius = 10.0f, gap = 8.0f, opacity = 1.0f;
    std::string font;                                  // absolute path, or empty for the system font
};

bool osk_parse_color(const std::string& s, OskColor* out);  // "#RRGGBB" or "#RRGGBBAA"
OskSkin osk_skin_parse(const std::string& json, const std::string& skin_dir, std::vector<std::string>* log);
```
  `runtime/src/psp_osk_skin.cpp`:

```cpp
#include "psp_osk_skin.h"

#include "json.hpp"

#include <cctype>

bool osk_parse_color(const std::string& s, OskColor* out) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    for (size_t i = 1; i < s.size(); i++) if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    auto byte = [&](size_t i) { return std::stoi(s.substr(i, 2), nullptr, 16) / 255.0f; };
    *out = {byte(1), byte(3), byte(5), s.size() == 9 ? byte(7) : 1.0f};
    return true;
}

OskSkin osk_skin_parse(const std::string& json, const std::string& skin_dir, std::vector<std::string>* log) {
    OskSkin s;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        log->push_back("[OSK] skin is not valid JSON: using the built-in Deck skin");
        return s;
    }
    if (j.contains("name") && j["name"].is_string()) s.name = j["name"].get<std::string>();
    const std::pair<const char*, OskColor*> colours[] = {
        {"background", &s.background}, {"panel", &s.panel}, {"key", &s.key}, {"key_text", &s.key_text},
        {"focus", &s.focus}, {"focus_text", &s.focus_text}, {"accent", &s.accent}};
    for (const auto& [k, dst] : colours) {
        if (!j.contains(k)) continue;
        OskColor c{};
        if (j[k].is_string() && osk_parse_color(j[k].get<std::string>(), &c)) *dst = c;
        else log->push_back(std::string("[OSK] skin ") + k + ": not a #RRGGBB(AA) colour, default kept");
    }
    const std::tuple<const char*, float*, float, float> nums[] = {
        {"radius", &s.radius, 0.0f, 40.0f}, {"gap", &s.gap, 0.0f, 40.0f}, {"opacity", &s.opacity, 0.1f, 1.0f}};
    for (const auto& [k, dst, lo, hi] : nums) {
        if (!j.contains(k)) continue;
        if (j[k].is_number() && j[k].get<float>() >= lo && j[k].get<float>() <= hi) *dst = j[k].get<float>();
        else log->push_back(std::string("[OSK] skin ") + k + ": out of range, default kept");
    }
    if (j.contains("font") && j["font"].is_string() && !j["font"].get<std::string>().empty()) {
        const std::string f = j["font"].get<std::string>();
        const bool abs = f[0] == '/' || f[0] == '\\' || (f.size() > 1 && f[1] == ':');
        s.font = abs || skin_dir.empty() ? f : skin_dir + "/" + f;
    }
    return s;
}
```
  Shipped skins (`runtime/assets/osk_skins/`):
  `deck.json` = `{"name": "Deck", "background": "#00000073", "panel": "#1B2838F5", "key": "#2A3F5A", "key_text": "#EBEEF2", "focus": "#1A9FFF", "focus_text": "#FFFFFF", "accent": "#1A9FFF", "radius": 10, "gap": 8, "opacity": 1}`;
  `light.json` = `{"name": "Claro / Light", "background": "#00000059", "panel": "#F2F4F7F5", "key": "#FFFFFF", "key_text": "#1E2228", "focus": "#2D7FF9", "focus_text": "#FFFFFF", "accent": "#2D7FF9", "radius": 10, "gap": 8, "opacity": 1}`;
  `patapon.json` = `{"name": "Patapon", "background": "#00000080", "panel": "#0D0D0DF2", "key": "#F5F0E6", "key_text": "#111111", "focus": "#E8382C", "focus_text": "#FFF4C2", "accent": "#F9C80E", "radius": 4, "gap": 6, "opacity": 1}`
  (pretty-printed, one field per line). `src/psp_osk_skin.cpp` joins the runtime source list.

- [ ] **Step 3: Overlay.** In `runtime/src/psp_osk_overlay.cpp`:
  - shader: add `uniform vec2 u_size; uniform float u_radius;` to the fragment shader and multiply the
    alpha by a rounded-rect coverage: `vec2 p = v_uv * u_size; vec2 q = abs(p - u_size * 0.5) - (u_size * 0.5 - vec2(u_radius)); float d = length(max(q, 0.0)) - u_radius; float cov = u_radius > 0.0 ? clamp(0.5 - d, 0.0, 1.0) : 1.0;`
    and `o = vec4(u_color.rgb, u_color.a * a * cov);`; store the two uniform locations in `Gl`;
  - `rect(...)` gains a trailing `float radius = 0.0f` parameter, sets `u_size` to `(w, h)` and
    `u_radius` to `min(radius, min(w, h) * 0.5f)` (text quads pass 0);
  - the font: `font()` first tries the skin's `font` when set;
  - add the skin and keyboard singletons:

```cpp
// The keyboard model the event loop drives and this overlay draws (main thread only).
OskKeyboard& psp_osk_keyboard_ref() {
    static OskKeyboard k(std::getenv("PSPRECOMP_UI_LANG") && std::string(std::getenv("PSPRECOMP_UI_LANG")) == "en" ? "en" : "es");
    return k;
}

const OskSkin& skin() {
    static OskSkin s;
    static bool loaded = false;
    if (loaded) return s;
    loaded = true;
    const char* path = std::getenv("PSPRECOMP_OSK_SKIN");
    if (!path || !*path) return s;
    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        std::fprintf(stderr, "[OSK] skin %s not found: built-in Deck skin\n", path);
        return s;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    std::fclose(fp);
    std::string dir = path;
    const size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? "." : dir.substr(0, slash);
    std::vector<std::string> log;
    s = osk_skin_parse(text, dir, &log);
    for (const std::string& l : log) std::fprintf(stderr, "%s\n", l.c_str());
    std::fprintf(stderr, "[OSK] skin %s (%s)\n", s.name.c_str(), path);
    return s;
}
```
  and expose in `psp_osk_overlay.h`: `#include "psp_osk_keyboard.h"` and
  `OskKeyboard& psp_osk_keyboard();  // the keyboard the event loop drives (main thread)`, implemented as
  `OskKeyboard& psp_osk_keyboard() { return psp_osk_keyboard_ref(); }`. Add
  `void psp_osk_keyboard_reset();` that does `psp_osk_keyboard_ref() = OskKeyboard(<same lang rule>);`.
  - rewrite `psp_osk_overlay_draw` (keep its signature) to draw, with the skin colours (multiply every
    alpha by `skin().opacity`):
    1. the background over the whole game rectangle;
    2. a panel `rw * 0.92` wide anchored to the bottom of the game rectangle, height `rh * 0.78`, radius
       `skin.radius * rh / 1080`;
    3. the title (accent colour) and the text field (key colour) with the text rendered in two pieces —
       before / after `v.cursor` code points — and a 2 px caret between them blinking every 500 ms, the
       `length/max` counter right-aligned;
    4. the grid: for each row of `psp_osk_keyboard().rows()`, keys laid out left to right with widths
       proportional to `OskKey::width` (row total width = panel inner width; row height
       `rh * 0.085`; gap `skin.gap * rh / 1080`), key colour `skin.key` (Shift keys use `skin.accent` when
       `shift()` or `caps()`; the Layer key uses `skin.accent` on the symbols layer), the focused key
       `skin.focus` with `skin.focus_text` label, the others `skin.key_text`; a pending accent shows the
       Accent key in `skin.accent`; labels centred, `rh * 0.045` px;
    5. a hint line in `PSPRECOMP_UI_LANG`: es `"Ⓐ escribir   Ⓑ borrar   Ⓧ mayús   Ⓨ espacio   LB/RB cursor   Select símbolos   Start aceptar"`,
       en `"Ⓐ type   Ⓑ delete   Ⓧ shift   Ⓨ space   LB/RB cursor   Select symbols   Start OK"`.
    Cache key-label textures in a `std::map<std::string, TextTex>` keyed by `label + "@" + px`.
    Keep the menu-prompt overlay unchanged except for the new `rect` parameter defaults.
- [ ] **Step 4: Event loop routing** (`runtime/src/psp_event_loop.cpp`):
  - a target adapter:

```cpp
// The keyboard grid edits the game's text through the OSK session.
struct SessionTarget : OskTarget {
    void type(const std::string& s) override { osk_session().type(s); }
    void backspace() override { osk_session().backspace(); }
    void move_cursor(int d) override { osk_session().move_cursor(d); }
    void accept() override { osk_session().confirm(); }
    void cancel() override { osk_session().cancel(); }
};
static SessionTarget g_osk_target;
static OskStickRepeat g_osk_stick;
static float g_osk_axis_x = 0.0f, g_osk_axis_y = 0.0f;
```
  - `handle_osk_event`: keep the keyboard cases (add `SDLK_LEFT` / `SDLK_RIGHT` → `osk_session().move_cursor(∓1)`);
    replace the two controller cases with:

```cpp
    case SDL_CONTROLLERBUTTONDOWN: {
        static const std::pair<int, OskAction> kPad[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_UP, OskAction::Up}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, OskAction::Down},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, OskAction::Left}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, OskAction::Right},
            {SDL_CONTROLLER_BUTTON_A, OskAction::Press}, {SDL_CONTROLLER_BUTTON_B, OskAction::Backspace},
            {SDL_CONTROLLER_BUTTON_X, OskAction::Shift}, {SDL_CONTROLLER_BUTTON_Y, OskAction::Space},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, OskAction::CursorLeft},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, OskAction::CursorRight},
            {SDL_CONTROLLER_BUTTON_BACK, OskAction::Layer}, {SDL_CONTROLLER_BUTTON_START, OskAction::Accept}};
        for (const auto& [b, a] : kPad) {
            if (ev.cbutton.button == b) { psp_osk_keyboard().act(a, g_osk_target); break; }
        }
        return true;
    }
    case SDL_CONTROLLERBUTTONUP:
        return true;
    case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_osk_axis_x = ev.caxis.value / 32767.0f;
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_osk_axis_y = ev.caxis.value / 32767.0f;
        return true;
```
    and update the function comment (gamepad buttons per the Global Constraints);
  - `sync_osk_text_input`: when it becomes active also `psp_osk_keyboard_reset();`, clear
    `g_pads` buttons for every pad (`for (auto& [id, c] : g_controllers) g_pads.remove(id);` then
    `publish_pads();`), `g_osk_axis_x = g_osk_axis_y = 0`;
  - main loop (next to `publish_wheel();`): 
    `if (osk_session().active()) { if (auto a = g_osk_stick.update(g_osk_axis_x, g_osk_axis_y, steady_ms())) psp_osk_keyboard().act(*a, g_osk_target); }`;
  - demo: before the loop `const bool osk_demo = std::getenv("PSPRECOMP_OSK_DEMO") && std::string(std::getenv("PSPRECOMP_OSK_DEMO")) == "1"; const uint64_t demo_at = steady_ms() + 2000; bool demo_open = false;`
    and inside the loop:

```cpp
        // PSPRECOMP_OSK_DEMO=1: one keyboard request over whatever is on screen, to look at the skins.
        if (osk_demo && !demo_open && steady_ms() >= demo_at && !osk_session().active()) {
            osk_session().open("Demo", "", 16);
            demo_open = true;
            std::fprintf(stderr, "[OSK] demo keyboard open\n");
        }
        if (demo_open) {
            std::string text;
            if (osk_session().poll(&text)) std::fprintf(stderr, "[OSK] demo text: %s\n", text.c_str());
        }
```
    (`steady_ms()` already exists in this file; if its type differs, adapt the two locals).
- [ ] **Step 5: Build copies the skins.** `games/patapon3/scripts/p3.sh` `cmd_build`, after the `SDL2.dll`
  copy: `mkdir -p "$BUILD/skins/osk" && cp -u "$REPO/runtime/assets/osk_skins/"*.json "$BUILD/skins/osk/"`.
- [ ] **Step 6: Docs.** `docs/ENV_FLAGS.md`: `PSPRECOMP_OSK_DEMO`. `ARCHITECTURE.md`: the input row
  (ControlMap from `PSPRECOMP_CONTROLS`) and the on-screen keyboard row (model `psp_osk_keyboard`, skins
  `psp_osk_skin`, overlay). `README.md`: one sentence each — controls are configurable in `settings.ini`
  `[controls]`; the in-game keyboard works with a gamepad, skins in `skins/osk/`.
- [ ] **Step 7: Gates.** Runtime gate → ctest **77/77** with `osk_keyboard_tests` now covering skins; test_p3
  12/12; `bash games/patapon3/scripts/p3.sh build` copies `build/rtw/skins/osk/{deck,light,patapon}.json`.
  Report (no commit; do not run the game). Claude then runs the demo for each skin and looks at it:

```bash
cd /c/Users/torso/Documents/deco/build/rtw
for s in deck light patapon; do
  PSPRECOMP_DEBUG_PORT=9998 PSPRECOMP_OSK_DEMO=1 PSPRECOMP_OSK_SKIN=$PWD/skins/osk/$s.json PSPRECOMP_FULLSCREEN=0 \
    timeout 25 ./psprecomp_runtime.exe > ../briefs_p4/demo_$s.log 2>&1 &
  sleep 12; printf "S C:/Users/torso/Documents/deco/build/briefs_p4/osk_$s.tga\n" | nc -w 5 127.0.0.1 9998; wait
done
```
  (convert the TGAs to PNG and read them; adjust colours / layout if a skin is unreadable). Claude commits
  `feat(runtime): Steam Deck style on-screen keyboard with skins and gamepad typing`.

---

### Task 4: Launcher — binding options, validation, gamepad capture, skins (launcher Rust)

**Files:**
- Modify: `src-tauri/core_options.json`, `src-tauri/src/options.rs`, `src-tauri/src/settings.rs`,
  `src-tauri/src/gamepad.rs`, `src-tauri/src/lib.rs`, `scripts/make-portable.mjs`
- Create: `src-tauri/src/controls.rs`

**Interfaces:**
- Produces: option kinds `"keys"` and `"pad"`; 24 options `controls.<action>_keys` / `controls.<action>_pad`
  (tab `controls`) and `display.osk_skin` (type `string`, default `deck`, tab `controls`);
  `controls::ACTIONS`, `controls::KEY_NAMES`, `controls::PAD_NAMES`, `controls::normalize_keys(&str) -> Option<String>`,
  `controls::normalize_pad(&str) -> Option<String>`, `controls::duplicates(&BTreeMap<String,String>) -> Vec<String>`;
  `gamepad::sdl_pad_name(Button) -> Option<&'static str>`; commands `start_pad_capture`, `cancel_pad_capture`
  (event `pad_captured { name: string | null }`), `list_osk_skins` → `[{ id, name, panel, key, key_text, focus }]`.

- [ ] **Step 1: Failing tests.** In `src-tauri/src/controls.rs` (new module, add `mod controls;` to lib.rs)
  write the module with its tests (the tests first, implementation next step):

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;
    #[test] fn key_lists() {
        assert_eq!(normalize_keys("x, return").as_deref(), Some("X, Return"));
        assert_eq!(normalize_keys(" Left Shift ").as_deref(), Some("Left Shift"));
        assert_eq!(normalize_keys("").as_deref(), Some(""));
        assert_eq!(normalize_keys("X, X"), None, "same key twice");
        assert_eq!(normalize_keys("X, Z, A"), None, "three keys");
        assert_eq!(normalize_keys("F1"), None, "reserved");
        assert_eq!(normalize_keys("Escape"), None, "the capture's cancel key");
        assert_eq!(normalize_keys("Nope"), None);
    }
    #[test] fn pad_names() {
        assert_eq!(normalize_pad("A").as_deref(), Some("a"));
        assert_eq!(normalize_pad("").as_deref(), Some(""));
        assert_eq!(normalize_pad("guide"), None);
        assert_eq!(normalize_pad("lefttrigger"), None, "triggers are fixed L / R");
    }
    #[test] fn duplicate_detection() {
        let mut v = BTreeMap::new();
        v.insert("controls.cross_keys".to_string(), "X, Return".to_string());
        v.insert("controls.circle_keys".to_string(), "Return".to_string());
        v.insert("controls.cross_pad".to_string(), "a".to_string());
        v.insert("controls.square_pad".to_string(), "a".to_string());
        let d = duplicates(&v);
        assert!(d.iter().any(|m| m.contains("Return")) && d.iter().any(|m| m.contains("a")), "{d:?}");
        v.insert("controls.circle_keys".to_string(), "Z".to_string());
        v.insert("controls.square_pad".to_string(), "x".to_string());
        assert!(duplicates(&v).is_empty());
    }
    #[test] fn catalog_has_every_binding() {
        let defs = crate::options::core_catalog().options;
        for a in ACTIONS {
            for (suffix, kind) in [("_keys", "keys"), ("_pad", "pad")] {
                let d = defs.iter().find(|d| d.section == "controls" && d.key == format!("{}{suffix}", a.0))
                    .unwrap_or_else(|| panic!("controls.{}{suffix} missing", a.0));
                assert_eq!(d.kind, kind);
                assert_eq!(d.tab, "controls");
            }
        }
        let cross = defs.iter().find(|d| d.key == "cross_keys").unwrap();
        assert_eq!(cross.default, "X, Return");
        assert!(defs.iter().any(|d| d.section == "display" && d.key == "osk_skin" && d.default == "deck"));
    }
}
```
  In `settings.rs` tests add:

```rust
    #[test] fn bindings_save_and_refuse_duplicates() {
        let d = root();
        load(d.path()).unwrap();
        let mut ok = BTreeMap::new();
        ok.insert("controls.cross_keys".to_string(), "K, Return".to_string());
        save(d.path(), &ok).unwrap();
        assert_eq!(load(d.path()).unwrap()["controls.cross_keys"], "K, Return");
        let mut dup = BTreeMap::new();
        dup.insert("controls.circle_keys".to_string(), "K".to_string());
        assert!(save(d.path(), &dup).unwrap_err().contains("K"), "K is already cross");
        let mut clear = BTreeMap::new();
        clear.insert("controls.select_keys".to_string(), "".to_string());
        save(d.path(), &clear).unwrap();
        assert_eq!(load(d.path()).unwrap()["controls.select_keys"], "", "a binding can be cleared");
    }
```
  In `gamepad.rs` tests add:

```rust
    #[test] fn sdl_names() {
        assert_eq!(sdl_pad_name(Button::South), Some("a"));
        assert_eq!(sdl_pad_name(Button::East), Some("b"));
        assert_eq!(sdl_pad_name(Button::West), Some("x"));
        assert_eq!(sdl_pad_name(Button::North), Some("y"));
        assert_eq!(sdl_pad_name(Button::LeftTrigger), Some("leftshoulder"));
        assert_eq!(sdl_pad_name(Button::RightTrigger), Some("rightshoulder"));
        assert_eq!(sdl_pad_name(Button::Select), Some("back"));
        assert_eq!(sdl_pad_name(Button::DPadLeft), Some("dpleft"));
        assert_eq!(sdl_pad_name(Button::LeftTrigger2), None);
        assert_eq!(sdl_pad_name(Button::Mode), None);
    }
```
  And in `lib.rs` tests a `list_osk_skins` test: write two skin files into a temp root `skins/osk/`
  (`deck.json` with `"name":"Deck","panel":"#1B2838F5","key":"#2A3F5A","key_text":"#EBEEF2","focus":"#1A9FFF"`
  and a broken `x.json` = `{`) → `osk_skins(root)` returns one entry `{id:"deck", name:"Deck", ...}` and
  skips the broken one. Run the Rust gate → compile errors.

- [ ] **Step 2: Implementation.**
  `src-tauri/src/controls.rs` (above the tests):

```rust
//! Key / button bindings (settings.ini [controls], psprecomp docs/contracts/settings-v1.md): names
//! the UI can produce, reserved keys, value normalisation and duplicate detection.
use std::collections::BTreeMap;

/// (action, default keys, default pad) in contract order.
pub const ACTIONS: [(&str, &str, &str); 12] = [
    ("up", "Up", "dpup"), ("down", "Down", "dpdown"), ("left", "Left", "dpleft"), ("right", "Right", "dpright"),
    ("cross", "X, Return", "a"), ("circle", "Z", "b"), ("square", "A", "x"), ("triangle", "S", "y"),
    ("l", "Q", "leftshoulder"), ("r", "W", "rightshoulder"), ("start", "Space", "start"), ("select", "V, Tab", "back"),
];

/// SDL key names the launcher can capture (src/controls.ts produces exactly these spellings).
pub const KEY_NAMES: &[&str] = &[
    "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
    "0","1","2","3","4","5","6","7","8","9","F2","F3","F4","F5","F6","F7","F8","F9","F10","F12",
    "Return","Space","Tab","Up","Down","Left","Right","Left Shift","Right Shift","Left Ctrl","Right Ctrl",
    "Left Alt","Right Alt","Insert","Home","End","PageUp","PageDown","CapsLock",
    "Keypad 0","Keypad 1","Keypad 2","Keypad 3","Keypad 4","Keypad 5","Keypad 6","Keypad 7","Keypad 8","Keypad 9",
    "Keypad Enter","Keypad +","Keypad -","Keypad *","Keypad /","Keypad .",
    ",",".","-","=","/",";","'","[","]","\\","`",
];

/// SDL GameController button names a binding may use (guide and the triggers are not bindable).
pub const PAD_NAMES: &[&str] = &["a","b","x","y","back","start","leftshoulder","rightshoulder",
    "leftstick","rightstick","dpup","dpdown","dpleft","dpright","misc1","paddle1","paddle2","paddle3","paddle4","touchpad"];

pub fn normalize_keys(v: &str) -> Option<String> {
    let mut out: Vec<&str> = Vec::new();
    for part in v.split(',').map(str::trim).filter(|p| !p.is_empty()) {
        let name = KEY_NAMES.iter().find(|k| k.eq_ignore_ascii_case(part))?;
        if out.contains(name) { return None; }
        out.push(name);
    }
    (out.len() <= 2).then(|| out.join(", "))
}

pub fn normalize_pad(v: &str) -> Option<String> {
    let v = v.trim();
    if v.is_empty() { return Some(String::new()); }
    PAD_NAMES.iter().find(|p| p.eq_ignore_ascii_case(v)).map(|p| p.to_string())
}

/// "key X bound to cross and circle" for every key / pad button used by two actions in `values`
/// (keys "controls.<action>_keys" / "controls.<action>_pad").
pub fn duplicates(values: &BTreeMap<String, String>) -> Vec<String> {
    let mut seen: BTreeMap<String, &str> = BTreeMap::new();
    let mut out = Vec::new();
    for (action, _, _) in ACTIONS {
        let keys = values.get(&format!("controls.{action}_keys")).map(String::as_str).unwrap_or("");
        let pad = values.get(&format!("controls.{action}_pad")).map(String::as_str).unwrap_or("");
        let items = keys.split(',').map(str::trim).filter(|k| !k.is_empty()).map(|k| format!("key {k}"))
            .chain((!pad.is_empty()).then(|| format!("pad button {pad}")));
        for item in items {
            match seen.get(&item) {
                Some(first) => out.push(format!("{item} bound to {first} and {action}")),
                None => { seen.insert(item, action); }
            }
        }
    }
    out
}
```
  `options.rs` `normalize`: add arms `"keys" => crate::controls::normalize_keys(v),` and
  `"pad" => crate::controls::normalize_pad(v),` before `"enum"` (note `normalize` trims first, and an empty
  value is valid for these two kinds).
  `settings.rs` `save`: after the normalisation loop, build the effective values with the changes applied
  (`let mut eff = effective(&IniFile::parse(&text), &defs); for (s, k, v) in &norm { eff.insert(format!("{s}.{k}"), v.clone()); }`
  — read `text` before the loop for this), then
  `let d = crate::controls::duplicates(&eff); if !d.is_empty() { return Err(d.join("; ")); }`.
  `core_options.json`: append the 24 binding options, e.g.

```json
    { "section": "controls", "key": "cross_keys", "type": "keys", "default": "X, Return", "tab": "controls",
      "label": { "es": "✕ Cruz (Don)", "en": "✕ Cross (Don)" } },
    { "section": "controls", "key": "cross_pad", "type": "pad", "default": "a", "tab": "controls",
      "label": { "es": "✕ Cruz (Don)", "en": "✕ Cross (Don)" } },
```
  for every action with labels — up "↑ Arriba"/"↑ Up", down "↓ Abajo"/"↓ Down", left "← Izquierda"/"← Left",
  right "→ Derecha"/"→ Right", cross "✕ Cruz (Don)"/"✕ Cross (Don)", circle "○ Círculo (Pon)"/"○ Circle (Pon)",
  square "□ Cuadrado (Pata)"/"□ Square (Pata)", triangle "△ Triángulo (Chaka)"/"△ Triangle (Chaka)",
  l "L", r "R", start "Start", select "Select" — and
  `{ "section": "display", "key": "osk_skin", "type": "string", "default": "deck", "tab": "controls", "label": { "es": "Aspecto del teclado", "en": "Keyboard skin" } }`.
  The launcher's default `settings.ini` therefore writes `[controls]` and `osk_skin` (the game reads them).
  `gamepad.rs`: add

```rust
/// SDL GameController button name of a gilrs button (what settings.ini [controls] stores).
pub fn sdl_pad_name(b: Button) -> Option<&'static str> {
    Some(match b {
        Button::South => "a", Button::East => "b", Button::West => "x", Button::North => "y",
        Button::LeftTrigger => "leftshoulder", Button::RightTrigger => "rightshoulder",
        Button::Select => "back", Button::Start => "start",
        Button::LeftThumb => "leftstick", Button::RightThumb => "rightstick",
        Button::DPadUp => "dpup", Button::DPadDown => "dpdown", Button::DPadLeft => "dpleft", Button::DPadRight => "dpright",
        _ => return None,
    })
}

/// While set, the next pad button press is reported as `pad_captured` instead of navigating.
#[derive(Default)]
pub struct PadCapture(pub std::sync::atomic::AtomicBool);

#[derive(Serialize, Clone)]
struct Captured { name: Option<&'static str> }
```
  and in `spawn`'s event loop, before mapping a press to navigation:

```rust
                if let EventType::ButtonPressed(b, _) = ev.event {
                    let capture = app.state::<PadCapture>();
                    if capture.0.swap(false, std::sync::atomic::Ordering::SeqCst) {
                        let _ = app.emit("pad_captured", Captured { name: sdl_pad_name(b) });
                        continue;
                    }
                    if let Some(action) = map_button(b) { let _ = app.emit("nav", Nav { action, device }); }
                }
```
  (replace the existing `if let EventType::ButtonPressed` block; while capturing, the stick is ignored too:
  skip the stick block when `capture.0.load(..)` is true). `lib.rs`: `.manage(gamepad::PadCapture::default())`,
  commands

```rust
#[tauri::command]
fn start_pad_capture(c: State<'_, gamepad::PadCapture>) { c.0.store(true, Ordering::SeqCst); }

#[tauri::command]
fn cancel_pad_capture(c: State<'_, gamepad::PadCapture>) { c.0.store(false, Ordering::SeqCst); }

#[derive(Serialize, Debug, PartialEq)]
struct OskSkinInfo { id: String, name: String, panel: String, key: String, key_text: String, focus: String }

/// skins/osk/*.json of the portable folder, by id; unreadable files are skipped.
fn osk_skins(root: &std::path::Path) -> Vec<OskSkinInfo> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(root.join("skins").join("osk")) else { return out };
    for e in rd.flatten() {
        let p = e.path();
        if p.extension().and_then(|x| x.to_str()) != Some("json") { continue; }
        let Some(id) = p.file_stem().and_then(|s| s.to_str()).map(str::to_string) else { continue };
        let Ok(text) = std::fs::read_to_string(&p) else { continue };
        let Ok(v) = serde_json::from_str::<serde_json::Value>(&text) else { continue };
        let s = |k: &str, d: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or(d).to_string();
        out.push(OskSkinInfo { name: s("name", &id), panel: s("panel", "#1B2838F5"), key: s("key", "#2A3F5A"),
                               key_text: s("key_text", "#EBEEF2"), focus: s("focus", "#1A9FFF"), id });
    }
    out.sort_by(|a, b| a.id.cmp(&b.id));
    out
}

#[tauri::command]
fn list_osk_skins() -> Vec<OskSkinInfo> { osk_skins(&settings::root_dir()) }
```
  registered in `generate_handler!`. `scripts/make-portable.mjs`: after copying the games, copy the first
  `--game` folder's `skins/` (if present) to `<out>/skins/` (`cpSync(join(firstGameDir, "skins"), join(out, "skins"), { recursive: true })`)
  and add `skins` to the list of folders it replaces; update its header comment.

- [ ] **Step 3: Gates.** Launcher Rust gate → all pass (66 + 4 controls + 1 settings + 1 gamepad + 1 lib = 73
  passed, 1 ignored; paste the real line); `cargo build -q` no new warnings. Report (no commit). Claude commits
  `feat(launcher): key and button bindings, gamepad capture, keyboard skins`.

---

### Task 5: Launcher UI — Controls editor and skin picker

**Files:**
- Create: `src/controls.ts`, `src/controls.test.ts`, `src/components/ControlsEditor.tsx`
- Modify: `src/types.ts`, `src/api.ts`, `src/screens/Options.tsx`, `src/screens/Options.test.tsx`,
  `src/styles.css`, `lang/es.json`, `lang/en.json`, `README.md`

**Interfaces:**
- Consumes: T4 option kinds / keys, commands `start_pad_capture`, `cancel_pad_capture`, event `pad_captured`,
  `list_osk_skins`.
- Produces: `controls.ts` — `ACTIONS: { id: string }[]` (contract order), `codeToSdl(code: string): string | null`,
  `splitKeys(v: string): string[]`, `assign(draft, action, slot: 0 | 1 | "pad", name) => { draft, movedFrom: string | null }`,
  `padLabel(name: string, device: Device): string`; `ControlsEditor` (forwardRef, `handle(a: NavAction): boolean`).

- [ ] **Step 1: Strings.** `lang/es.json` adds:

```json
  "controls.key1": "Tecla 1",
  "controls.key2": "Tecla 2",
  "controls.pad": "Mando",
  "controls.press_key": "Pulsa una tecla… (Esc cancela)",
  "controls.press_button": "Pulsa un botón del mando… (Esc cancela)",
  "controls.moved": "Quitada de {action}",
  "controls.unknown_key": "Esa tecla no se puede usar",
  "controls.reserved": "Esa tecla está reservada (F1, F11, Esc)",
  "controls.none": "—",
  "controls.restore": "Restaurar valores por defecto",
  "controls.skin": "Aspecto del teclado en pantalla",
  "controls.hint": "Confirmar: cambiar · △/R: borrar"
```
  `lang/en.json` the same keys: "Key 1", "Key 2", "Gamepad", "Press a key… (Esc cancels)", "Press a gamepad
  button… (Esc cancels)", "Removed from {action}", "That key cannot be used", "That key is reserved (F1, F11,
  Esc)", "—", "Restore defaults", "On-screen keyboard skin", "Confirm: change · △/R: clear".

- [ ] **Step 2: `src/controls.ts` + tests.**

```ts
// Key / button bindings of settings.ini [controls] (psprecomp docs/contracts/settings-v1.md). Key names
// are SDL's (src-tauri/src/controls.rs KEY_NAMES has exactly these spellings); pad names are SDL
// GameController button names.
import type { Device } from "./types";

export const ACTIONS = ["up", "down", "left", "right", "cross", "circle", "square", "triangle", "l", "r", "start", "select"]
  .map((id) => ({ id }));

const NAMED: Record<string, string> = {
  Enter: "Return", Space: "Space", Tab: "Tab", ArrowUp: "Up", ArrowDown: "Down", ArrowLeft: "Left", ArrowRight: "Right",
  ShiftLeft: "Left Shift", ShiftRight: "Right Shift", ControlLeft: "Left Ctrl", ControlRight: "Right Ctrl",
  AltLeft: "Left Alt", AltRight: "Right Alt", Insert: "Insert", Home: "Home", End: "End", PageUp: "PageUp",
  PageDown: "PageDown", CapsLock: "CapsLock", NumpadEnter: "Keypad Enter", NumpadAdd: "Keypad +",
  NumpadSubtract: "Keypad -", NumpadMultiply: "Keypad *", NumpadDivide: "Keypad /", NumpadDecimal: "Keypad .",
  Comma: ",", Period: ".", Minus: "-", Equal: "=", Slash: "/", Semicolon: ";", Quote: "'", BracketLeft: "[",
  BracketRight: "]", Backslash: "\\", Backquote: "`",
};
export const RESERVED_CODES = ["F1", "F11", "Escape", "Backspace", "Delete"];

/// SDL key name of a KeyboardEvent.code, or null when it cannot be bound.
export function codeToSdl(code: string): string | null {
  if (RESERVED_CODES.includes(code)) return null;
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit[0-9]$/.test(code)) return code.slice(5);
  if (/^Numpad[0-9]$/.test(code)) return `Keypad ${code.slice(6)}`;
  if (/^F([2-9]|10|12)$/.test(code)) return code;
  return NAMED[code] ?? null;
}

export function splitKeys(v: string): string[] {
  return v.split(",").map((s) => s.trim()).filter(Boolean);
}

const keysId = (a: string) => `controls.${a}_keys`;
const padId = (a: string) => `controls.${a}_pad`;

/// Puts `name` in `action`'s slot; when another action already uses it, it is removed there first
/// (Steam style) and that action is returned as movedFrom.
export function assign(draft: Record<string, string>, action: string, slot: 0 | 1 | "pad", name: string):
    { draft: Record<string, string>; movedFrom: string | null } {
  const next = { ...draft };
  let movedFrom: string | null = null;
  for (const { id } of ACTIONS) {
    if (slot === "pad") {
      if (id !== action && next[padId(id)] === name) { next[padId(id)] = ""; movedFrom = id; }
    } else {
      const ks = splitKeys(next[keysId(id)] ?? "");
      if (ks.includes(name) && !(id === action && ks.indexOf(name) === slot)) {
        next[keysId(id)] = ks.filter((k) => k !== name).join(", ");
        if (id !== action) movedFrom = id;
      }
    }
  }
  if (slot === "pad") next[padId(action)] = name;
  else {
    const ks = splitKeys(next[keysId(action)] ?? "");
    const out = [ks[0] ?? "", ks[1] ?? ""];
    out[slot] = name;
    next[keysId(action)] = out.filter(Boolean).join(", ");
  }
  return { draft: next, movedFrom };
}

export function clearSlot(draft: Record<string, string>, action: string, slot: 0 | 1 | "pad"): Record<string, string> {
  const next = { ...draft };
  if (slot === "pad") next[padId(action)] = "";
  else next[keysId(action)] = splitKeys(next[keysId(action)] ?? "").filter((_, i) => i !== slot).join(", ");
  return next;
}

const PS: Record<string, string> = { a: "✕", b: "○", x: "□", y: "△", leftshoulder: "L1", rightshoulder: "R1",
  back: "Create", start: "Options", leftstick: "L3", rightstick: "R3", touchpad: "Touchpad" };
const XB: Record<string, string> = { a: "A", b: "B", x: "X", y: "Y", leftshoulder: "LB", rightshoulder: "RB",
  back: "View", start: "Menu", leftstick: "LS", rightstick: "RS", touchpad: "Touchpad" };
const DPAD: Record<string, string> = { dpup: "D-pad ↑", dpdown: "D-pad ↓", dpleft: "D-pad ←", dpright: "D-pad →" };

export function padLabel(name: string, device: Device): string {
  if (!name) return "";
  return DPAD[name] ?? (device === "playstation" ? PS : XB)[name] ?? name;
}
```
  `src/controls.test.ts`:

```ts
import { assign, clearSlot, codeToSdl, padLabel } from "./controls";

const base = { "controls.cross_keys": "X, Return", "controls.circle_keys": "Z", "controls.cross_pad": "a", "controls.square_pad": "x" };

test("KeyboardEvent.code to SDL names", () => {
  expect(codeToSdl("KeyK")).toBe("K");
  expect(codeToSdl("Digit7")).toBe("7");
  expect(codeToSdl("Numpad4")).toBe("Keypad 4");
  expect(codeToSdl("Enter")).toBe("Return");
  expect(codeToSdl("ShiftLeft")).toBe("Left Shift");
  expect(codeToSdl("F5")).toBe("F5");
  for (const c of ["F1", "F11", "Escape", "Backspace", "Delete", "MetaLeft", "F13"]) expect(codeToSdl(c)).toBeNull();
});

test("assign moves a key from another action", () => {
  const r = assign(base, "circle", 0, "Return");
  expect(r.draft["controls.circle_keys"]).toBe("Return");
  expect(r.draft["controls.cross_keys"]).toBe("X");
  expect(r.movedFrom).toBe("cross");
  const s = assign(base, "cross", 1, "K");
  expect(s.draft["controls.cross_keys"]).toBe("X, K");
  expect(s.movedFrom).toBeNull();
  const p = assign(base, "square", "pad", "a");
  expect(p.draft["controls.square_pad"]).toBe("a");
  expect(p.draft["controls.cross_pad"]).toBe("");
  expect(p.movedFrom).toBe("cross");
});

test("clear a slot and pad labels", () => {
  expect(clearSlot(base, "cross", 0)["controls.cross_keys"]).toBe("Return");
  expect(clearSlot(base, "cross", "pad")["controls.cross_pad"]).toBe("");
  expect(padLabel("a", "playstation")).toBe("✕");
  expect(padLabel("a", "xbox")).toBe("A");
  expect(padLabel("dpup", "xbox")).toBe("D-pad ↑");
});
```
  Run the UI gate → the new test file fails (module missing) until the file exists; then passes.

- [ ] **Step 3: Types and api.** `types.ts`: `OptionDef["type"]` adds `"keys" | "pad"`; add
  `export type OskSkinInfo = { id: string; name: string; panel: string; key: string; key_text: string; focus: string };`.
  `api.ts`: `startPadCapture: () => invoke<void>("start_pad_capture")`, `cancelPadCapture: () => invoke<void>("cancel_pad_capture")`,
  `listOskSkins: () => invoke<OskSkinInfo[]>("list_osk_skins")`.

- [ ] **Step 4: `ControlsEditor` + Options wiring.** `src/components/ControlsEditor.tsx`:

```tsx
import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { api } from "../api";
import { ACTIONS, assign, clearSlot, codeToSdl, padLabel, splitKeys } from "../controls";
import { t } from "../i18n";
import type { Device, NavAction, OptionDef, OskSkinInfo } from "../types";

type Slot = 0 | 1 | "pad";
const SLOTS: Slot[] = [0, 1, "pad"];
export type ControlsHandle = { handle(a: NavAction): boolean };
type Props = { strings: Record<string, string>; lang: "es" | "en"; defs: OptionDef[]; draft: Record<string, string>;
  device: Device; onChange(next: Record<string, string>): void };

// Options › Controls: one row per PSP button (key 1, key 2, gamepad), then the keyboard skin and
// "restore defaults". Confirm captures, reset (R / △) clears the focused cell.
export const ControlsEditor = forwardRef<ControlsHandle, Props>(function ControlsEditor(p, ref) {
  const [row, setRow] = useState(0);
  const [col, setCol] = useState(0);
  const [capture, setCapture] = useState<Slot | null>(null);
  const [notice, setNotice] = useState("");
  const [skins, setSkins] = useState<OskSkinInfo[]>([]);
  const st = useRef({ row, col, capture, draft: p.draft });
  st.current = { row, col, capture, draft: p.draft };
  const skinRow = ACTIONS.length, restoreRow = ACTIONS.length + 1;
  const label = (id: string) => p.defs.find((d) => d.key === `${id}_keys`)?.label[p.lang] ?? id;

  useEffect(() => { api.listOskSkins().then(setSkins).catch(() => setSkins([])); }, []);

  const finish = (name: string | null, why?: string) => {
    const { row: r, capture: c, draft } = st.current;
    setCapture(null);
    if (c === null) return;
    if (!name) { setNotice(why ?? ""); return; }
    const { draft: next, movedFrom } = assign(draft, ACTIONS[r].id, c, name);
    p.onChange(next);
    setNotice(movedFrom ? t(p.strings, "controls.moved", { action: label(movedFrom) }) : "");
  };

  // Keyboard capture: a window capture-phase listener runs before the Options nav handler.
  useEffect(() => {
    if (capture === null) return;
    const onKey = (e: KeyboardEvent) => {
      e.preventDefault();
      e.stopPropagation();
      if (e.code === "Escape") { if (capture === "pad") void api.cancelPadCapture(); finish(null); return; }
      if (capture === "pad") return;
      const name = codeToSdl(e.code);
      finish(name, t(p.strings, ["F1", "F11", "Backspace", "Delete"].includes(e.code) ? "controls.reserved" : "controls.unknown_key"));
    };
    window.addEventListener("keydown", onKey, true);
    let un: (() => void) | null = null;
    let closed = false;
    if (capture === "pad") {
      void api.startPadCapture();
      listen<{ name: string | null }>("pad_captured", (ev) => finish(ev.payload.name, t(p.strings, "controls.unknown_key")))
        .then((u) => { if (closed) u(); else un = u; }).catch(() => {});
    }
    return () => { closed = true; window.removeEventListener("keydown", onKey, true); un?.(); };
  }, [capture]);  // eslint-disable-line react-hooks/exhaustive-deps

  const stepSkin = (dir: 1 | -1) => {
    if (!skins.length) return;
    const cur = skins.findIndex((s) => s.id === p.draft["display.osk_skin"]);
    const next = skins[(Math.max(0, cur) + dir + skins.length) % skins.length];
    p.onChange({ ...p.draft, "display.osk_skin": next.id });
  };
  const restore = () => {
    const next = { ...p.draft };
    for (const d of p.defs) if (d.tab === "controls" && d.section === "controls") next[`${d.section}.${d.key}`] = d.default;
    p.onChange(next);
  };

  useImperativeHandle(ref, () => ({
    handle(a: NavAction) {
      const { row: r, col: c } = st.current;
      if (st.current.capture !== null) return true;
      const rows = ACTIONS.length + 2;
      switch (a) {
        case "up": setRow((r - 1 + rows) % rows); return true;
        case "down": setRow((r + 1) % rows); return true;
        case "left": if (r === skinRow) stepSkin(-1); else setCol((c + 2) % 3); return true;
        case "right": if (r === skinRow) stepSkin(1); else setCol((c + 1) % 3); return true;
        case "confirm":
          if (r === restoreRow) restore();
          else if (r < ACTIONS.length) { setNotice(""); setCapture(SLOTS[c]); }
          return true;
        case "reset":
          if (r < ACTIONS.length) p.onChange(clearSlot(st.current.draft, ACTIONS[r].id, SLOTS[c]));
          return true;
        default: return false;
      }
    },
  }));

  const cell = (id: string, s: Slot) => {
    const v = s === "pad" ? padLabel(p.draft[`controls.${id}_pad`] ?? "", p.device) : splitKeys(p.draft[`controls.${id}_keys`] ?? "")[s] ?? "";
    return v || t(p.strings, "controls.none");
  };
  const skin = skins.find((s) => s.id === p.draft["display.osk_skin"]);
  return (
    <div className="panel controls-editor" data-testid="controls-editor">
      <table>
        <thead><tr><th /><th>{t(p.strings, "controls.key1")}</th><th>{t(p.strings, "controls.key2")}</th><th>{t(p.strings, "controls.pad")}</th></tr></thead>
        <tbody>
          {ACTIONS.map(({ id }, r) => (
            <tr key={id} data-testid={`ctl-${id}`}>
              <th>{label(id)}</th>
              {SLOTS.map((s, c) => (
                <td key={String(s)} className={row === r && col === c ? "selected" : ""} aria-selected={row === r && col === c}
                    onMouseEnter={() => { setRow(r); setCol(c); }} onClick={() => { setRow(r); setCol(c); setNotice(""); setCapture(s); }}>
                  {cell(id, s)}
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
      <div className={`skin-row${row === skinRow ? " selected" : ""}`} onMouseEnter={() => setRow(skinRow)} data-testid="skin-row">
        <span>{t(p.strings, "controls.skin")}:</span>
        <button className="arrow" onClick={() => stepSkin(-1)}>◂</button>
        <span>{skin?.name ?? p.draft["display.osk_skin"]}</span>
        <button className="arrow" onClick={() => stepSkin(1)}>▸</button>
        {skin && <span className="swatch" style={{ background: skin.panel }}><i style={{ background: skin.key }} /><i style={{ background: skin.focus }} /></span>}
      </div>
      <div className={`restore-row${row === restoreRow ? " selected" : ""}`} onMouseEnter={() => setRow(restoreRow)} onClick={restore}>
        {t(p.strings, "controls.restore")}
      </div>
      <p className="muted" role="status">
        {capture !== null ? t(p.strings, capture === "pad" ? "controls.press_button" : "controls.press_key") : notice || t(p.strings, "controls.hint")}
      </p>
    </div>
  );
});
```
  `Options.tsx`: remove the `CONTROLS` table constant and the read-only table; import `ControlsEditor`,
  `ControlsHandle`; `const editor = useRef<ControlsHandle>(null);`; in `handle(a)`, right after the `leave`
  block: `if (TABS[tab] === "controls" && editor.current?.handle(a)) return;` (tab switching, back and the
  unsaved prompt keep working because `handle` returns false for them); render
  `{TABS[tab] === "controls" ? <ControlsEditor ref={editor} strings={p.strings} lang={p.lang} defs={p.defs} draft={draft} device={device} onChange={setDraft} /> : (...existing option list...)}`;
  the `reset` case in Options must not reset the controls tab (the editor consumes `reset` there). The
  PromptBar on the controls tab shows `select`, `confirm`, `reset` (label `controls.hint`'s clear part via
  `prompt.reset`) and `back`.
  `styles.css`: `.controls-editor table` (reuse `.controls table` rules), `.controls-editor td.selected`,
  `.skin-row.selected`, `.restore-row.selected` (accent background, dark text), `.swatch` (inline-flex, 4em ×
  1.2em, rounded; its `i` children 1em squares).

- [ ] **Step 5: Options tests.** `Options.test.tsx`: extend `defs` with
  `{ section: "controls", key: "cross_keys", type: "keys", default: "X, Return", tab: "controls", label: L("✕ Cruz (Don)") }`,
  `{ section: "controls", key: "circle_keys", type: "keys", default: "Z", tab: "controls", label: L("○ Círculo (Pon)") }`,
  `{ section: "controls", key: "cross_pad", type: "pad", default: "a", tab: "controls", label: L("✕ Cruz (Don)") }`,
  `{ section: "controls", key: "circle_pad", type: "pad", default: "b", tab: "controls", label: L("○ Círculo (Pon)") }`,
  `{ section: "display", key: "osk_skin", type: "string", default: "deck", tab: "controls", label: L("Aspecto") }`
  and the matching `values`; mock `../api` with `listOskSkins: vi.fn(async () => [{ id: "deck", name: "Deck", panel: "#000", key: "#111", key_text: "#fff", focus: "#00f" }, { id: "patapon", name: "Patapon", panel: "#000", key: "#eee", key_text: "#000", focus: "#f00" }])`,
  `startPadCapture: vi.fn(async () => {})`, `cancelPadCapture: vi.fn(async () => {})`; keep the `listen` mock but
  make it capture handlers by event name so a test can fire `pad_captured`. Tests:
  1. Controls tab (press `e` twice from General): rows show `✕ Cruz (Don)` with `X`, `Return`, `✕`/`A`;
     Down to the circle row, Enter → "Pulsa una tecla…"; `fireEvent.keyDown(window, { code: "Enter" })`
     → circle key 1 shows `Return`, cross keeps only `X`, status shows "Quitada de ✕ Cruz (Don)"; Enter on the
     list saves (`onSaved` called with `controls.circle_keys: "Return"` and `controls.cross_keys: "X"`) — use the
     existing save flow (leave via Escape → unsaved box → Guardar).
  2. Escape during a capture cancels it and does NOT leave Options (`onBack` not called).
  3. `r` on the cross key 1 cell clears it (`X` disappears, `Return` stays).
  4. Pad: move to the cross pad column, Enter → "Pulsa un botón…" and `startPadCapture` called; fire
     `pad_captured` with `{ name: "b" }` → cross pad shows `B` (xbox labels by default device) and circle pad
     becomes `—`.
  5. Skin row: Down to the skin row, ArrowRight → shows `Patapon`.
  Write the tests with the same helpers the file already uses (`setup`, `userEvent.keyboard`).

- [ ] **Step 6: README.** Launcher `README.md` (es + en): section "Controles" — Options › Controles, 2 keys +
  1 button per PSP button, capture / move / clear / restore, the keyboard skin, `skins/osk/*.json` format
  (copy the field list from the spec §2.6).

- [ ] **Step 7: Gates.** Launcher UI gate: Vitest all pass (35 + 3 controls + 5 Options = 43; paste the real
  count), `tsc --noEmit` clean; launcher Rust gate still green. Report (no commit). Claude commits
  `feat(launcher): Controls editor with key / button capture and keyboard skins`.

---

### Task 6: Rebuild, package, end to end, Findings (Claude)

- [ ] **Step 1:** Rebuild the three exes (`p3.sh build`, `P3_VARIANT=v115b`, `P3_VARIANT=vanilla`); 1.1.4 tour
  PASS; v115b / vanilla `movie_check.py` MOVIE OK.
- [ ] **Step 2:** `npm run tauri build -- --no-bundle`; make-portable into `build/e2e_portable3` and
  `build/e2e_portable` (keeps versions, saves, settings); check `skins/osk/` exists in both.
- [ ] **Step 3 (CDP driver `build/e2e3/cdp.mjs`, launcher started with
  `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222`, game on 9998):** Options › Controles →
  set cross key 1 = `K` (dispatch a `keydown` with `code: "KeyK"` through CDP), square pad = `y` (fire the
  `pad_captured` path is not reachable from CDP: set it by `assign` through the UI with a real pad later, or
  edit `settings.ini` directly for the E2E) → save → `settings.ini` has `cross_keys = K, Return` → Start → the
  game log has `[CONTROLS] …cross=K,Return|a…`. Skin → Patapon → save → `PSPRECOMP_OSK_SKIN` in the
  `[SETTINGS]` lines.
- [ ] **Step 4:** `PSPRECOMP_OSK_DEMO=1` screenshots of the three skins (Task 3 Step 7 loop) in the final build.
- [ ] **Step 5:** Ask the user to: type a hero name with the DualSense only in each skin (new game → hero
  name), remap a key and play a mission with it. Write `## Findings` (commits, E2E table, plan defects),
  commit `docs(plan): launcher part 4 findings`. Nothing pushed without the user's OK.

## Findings (2026-10-18, Claude)

Built by DeepSeek v4.1 flash (`#max`) one task at a time, reviewed and committed by Claude. Nothing pushed.

**Commits.** psprecomp `patapon3-windows`: bf4c166 (T1 ControlMap, `[controls]` → `PSPRECOMP_CONTROLS`),
4deb3fb (T2 keyboard model + text cursor), a4814c5 (T3 skins, overlay, pad routing, demo). Launcher `main`:
1177b0c (T4 Rust), 3200377 (T5 Controls editor), 3b2164f (E2E fixes).

**Gates.** ctest 77/77; test_p3 12/12; 1.1.4 tour `build/tour/p4_t6` RT_REGRESS PASS (21 gated checkpoints);
v115b and vanilla rebuilt, `movie_check.py` MOVIE OK both; launcher cargo 73 passed + 1 ignored, Vitest 43/43,
tsc clean; `e2e_portable3` and `e2e_portable` repackaged, `skins/osk/{deck,light,patapon}.json` in both.

| E2E (CDP on e2e_portable3, game on 9998) | Result |
|---|---|
| Options › Controles shows today's mapping (X / Return / A ...) | OK |
| Cross key 1 ← `KeyK`; save | `settings.ini` `cross_keys = K, Return` |
| Circle key 1 ← Enter | Return moves from cross, status "Quitada de ✕ Cruz (Don)" |
| Esc during a capture | capture cancelled, still in Options |
| R on a cell / Restore defaults | cell cleared / every binding back to default |
| Skin → Patapon; save; Start | game log `[SETTINGS] PSPRECOMP_CONTROLS=cross=K, Return\|a`, `PSPRECOMP_OSK_SKIN=…/skins/osk/patapon.json`, `[CONTROLS] …cross=K,Return\|a…` |
| `PSPRECOMP_OSK_DEMO=1` window captures (deck, light, patapon, en) | readable after the fixes below (`build/e2e4/osk_*.png`) |

**Plan defects found.**
1. T3: Segoe UI has no ⌫ ⇧ Ⓐ Ⓑ Ⓧ Ⓨ; the overlay needs a symbol fallback font (`seguisym.ttf`, DejaVuSans on Linux).
2. T3: the existing rasteriser dropped any glyph reaching left of its pen ("j" was blank); the bitmap now covers
   the union of the glyph boxes.
3. T3: the hint was drawn in `key_text`, invisible on the patapon skin (dark text, dark panel); it now follows
   the panel's luminance, and the panel fits its content.
4. T3 Step 7: the debug socket `S` screenshot is the PSP frame only (no overlay); the skins need a window
   capture (`build/e2e4/winshot.ps1`, `osk_demo.py`).
5. T4: `default_ini` pushed a second empty `[controls]` once the catalog has controls options (DeepSeek fixed).
6. T5: at 1080p the editor ran under the prompt bar; R kept the "Restore settings" label on the Controls tab;
   leaving the editor with a pad capture armed left it armed; Esc without `code` was refused.
7. Process: the first T3 run hung ~5 h because the user's interactive OpenCode (and its `serve --service`)
   shared the local DB. Close every OpenCode before a headless run; a file-change watchdog caught nothing
   after that.

**Not verified here (user check).** Typing a hero name with only the gamepad in each skin, and playing a mission
with a remapped key: key injection from this background session does not reach the game or launcher windows.
