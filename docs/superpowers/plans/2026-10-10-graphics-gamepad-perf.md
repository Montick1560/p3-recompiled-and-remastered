# Graphics bugs, gamepad and performance — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. The three parts are independent. Part B (gamepad) and Part C (performance measurement) need no user input until their final check, so start there if the user is away. Part A starts with a gate that needs the user at the keyboard.

**Goal:** Fix the graphics bugs in the user's 2026-10-10 screenshots, add game-controller support, and find and fix why the hideout runs at about 23 fps and why going from the skipped opening movie to the title menu is slow.

**Architecture:**
- **Part A** is diagnosis-first: for each bug, the user brings the game to the screen, we capture `S` + `D` + `X` bisection, and compare with PPSSPP (`tools/gelist.py`). Then a TDD fix goes into the pure unit that owns the wrong value. Render-to-texture (branch `wt-rtt`) is merged only if a bug proves to need it.
- **Part B** adds SDL GameController input: a pure mapping unit plus event-loop glue, with the analog stick feeding `sceCtrl`.
- **Part C** adds a generic `PSPRECOMP_PERF=1` per-second counter line, measures, and fixes the biggest cost first.

**Tech Stack:** C++17, SDL2 2.32 (GameController API), OpenGL 3.3 core, ctest standalone tests, debug socket TCP 9999, PPSSPP as the oracle (`DECO/PPSSPP`, source in `DECO/tools/ppsspp-src`).

**Spec:** `docs/superpowers/specs/2026-10-07-patapon3-native-port-design.md`. The user agreed on this scope on 2026-10-10: "vamos con el 1 y 2" (graphics bugs, gamepad) plus the performance issues they reported. Multiplayer (Paragate ad hoc) comes after this plan and gets its own spec. See "After this plan".

## Where things stand (read first; the session that wrote this plan was cleared)

- Repo `C:/Users/torso/Documents/deco/psprecomp` (**DECO** = `C:/Users/torso/Documents/deco`), branch `patapon3-windows`, pushed to `p3` main. Read `DECO/build/autopilot/HANDOFF.md` first.
- Done on 2026-10-10:
  - M3b render scale (`PSPRECOMP_RENDER_SCALE`).
  - M3c HD texture pack (`PSPRECOMP_TEXTURES`, pack at `DECO/PPSSPP/memstick/PSP/TEXTURES/INFN00001`).
  - `DECO/jugar.bat` sets scale 4 + the pack.
- The user's screenshots are on `C:/Users/torso/Desktop/`. Their own words:
  - "rojito.png", "sukopon malo.png", "negro.png", "lombris y hatapon.png", "sin cofres.png".
  - Performance: "al saltar la cinemática del inicio e ir al menú (nuevo juego / continuar) … es muy lento, se escucha la música del menú pero tarda en llegar"; "el rendimiento dentro del escondite también es malo, andará como en los 23 fps … incluso antes de la implementación del modo HD".
- Tools:
  - `python -I DECO/build/tools/to_square.py <tag> [ENV=VAL ...]`: boots with a copy of the save and reaches the hideout. Leaves the game running; shots go to `DECO/build/shots/<tag>/`, the log to `DECO/build/logs/<tag>.log`.
  - Debug socket: `S <abs.tga>`, `D <lists>`, `X <lo> <hi>`, `I`, `R`.
  - `tools/gelist.py` diffs a display list against PPSSPP's.
  - `tools/tgadiff.py a b`.
- Known harmless issue: a `[CRASH]` after `[RT] Shutdown complete` when the window closes (detached guest threads). Part C Task C5 fixes it.

## Global Constraints

- Generic runtime code only: no Patapon addresses or names in `runtime/src` / `runtime/include` (DEBUGGING.md §2). Game-specific fixes go in `games/patapon3/runtime/`.
- All GL calls stay on the render (main) thread.
- Scale 1 without a pack must stay pixel-identical unless a Part A fix intentionally changes a buggy screen. The language-menu baseline is `DECO/build/shots/m3b_base_a.tga` (`tools/tgadiff.py` → 0 differing pixels).
- Commits are authored `Patapon Community` (repo-local config), with **no `Co-Authored-By` trailer**. Push with `git push p3 patapon3-windows:main`; never force.
- Gates after every runtime change:
  - `cd DECO/build/rtw && ninja && ctest` with 0 failures (31 tests today).
  - `cd DECO/psprecomp && cargo test -q` all ok.
  - If the user's game is running (the exe is locked), build test targets only: `ninja $(ninja -t targets all | grep -oE "^test_[A-Za-z0-9_]+\.exe" | sort -u)`.
- The user navigates for visual checks. Do not write new navigation scripts (user feedback 2026-10-09). Existing tools (`to_square.py`, `flow.py`) and socket commands are fine.
- Talk to the user in Spanish; code, comments and commits in English. The user has given full autonomy (2026-10-10) except destroying their C: drive.

## Review Focus

1. **A speed-up that serves stale textures.** Caching texture hashes per frame (Part C) must still catch a texture the CPU rewrites between frames: font atlases, the movie ring buffer, palette fades. Expected: Task C3 test `rehash_next_frame`, and the opening movie still animates.
2. **Controller and keyboard at the same time.** Releasing a pad button must not release a key that is held, and the other way round. Unplugging a pad must release its buttons. Expected: Task B1 tests `combine_sources` and `unplug_releases`.
3. **Analog stick at rest.** A worn stick reports small offsets. Expected: dead zone → exactly 128, test `deadzone_centres`. Before C-perf measurements, check that the game does not move menus on its own with a pad plugged in.
4. **A graphics fix that breaks another screen.** Expected: each Part A fix re-runs the scale-1 baseline diff and the hideout `to_square.py` shot comparison.
5. **Performance numbers measured on the wrong build.** Expected: Part C measures at scale 1 without the pack (the user's 23 fps was before HD) and again with `jugar.bat` settings, and records both.

---

## Part A — Graphics bugs (user screenshots, 2026-10-10)

The screenshots are from the user's own sessions; the names are theirs. What each shows, as read by the plan author (**confirm each with the user in Task A1**):

| File | What it shows | First guess |
|------|---------------|-------------|
| `negro.png` | Mission, rhythm tutorial: a large **black rectangle** covers the middle of the screen. Its left and right edges look blurred; the HUD and the speech bubble draw on top. | A full-screen effect that samples the framebuffer (blur/fog). With no render-to-texture it samples stale guest VRAM. Strong RTT candidate (branch `wt-rtt`). |
| `lombris y hatapon.png` | Mission in FEVER: the "¡FIEBRE!" worm is a flat red bar, and Hatapon (the flag bearer) looks wrong to the user. | Worm segments missing (a draw path or blend issue), or animation/sprite composition. Needs the user's description and a PPSSPP shot. |
| `rojito.png` | Tutorial "□□□○ = Marchar" box: small **red marks** below the treasure-chest icons at the top right. | Wrong CLUT/alpha on a small HUD sprite, or a sprite from a neighbouring atlas cell. |
| `sukopon malo.png` | Hideout watchtower with Sukopon (the telescope character). The user says it is wrong. | Missing colour layer or wrong blend on a character sprite. Compare with PPSSPP. |
| `sin cofres.png` | 3D room with three pedestals labelled "Nvl" (level): **no chests/objects** on them, only the light beam. | 3D models not drawn: transform-mode draws culled, depth or vertex-format problem, or RTT if the objects are pre-rendered. Related to the old "barracks preview light box" (M3a). |

### Task A1: Bug confirmation and evidence capture (needs the user)

**Files:** scratch only. Results go in "Part A evidence" at the end of this file.

- [ ] **Step 1: Ask the user (Spanish)**, one bug at a time: what is wrong in each screenshot compared with PPSSPP, and how to reach that screen. If possible, they also take the same screenshot in PPSSPP with the same pack (`DECO/PPSSPP/PPSSPPWindows64.exe`, Replace textures ON).
- [ ] **Step 2: For each bug the user can reach,** they launch `jugar.bat` and stop on the screen without closing the game. We then run:

```bash
cd /c/Users/torso/Documents/deco/build
python -I -c "import socket,sys;s=socket.create_connection(('127.0.0.1',9999));s.sendall(('S C:/Users/torso/Documents/deco/build/shots/bug_%s.tga\n'%sys.argv[1]).encode());print(s.recv(64))" <name>
python -I -c "import socket;s=socket.create_connection(('127.0.0.1',9999));s.sendall(b'D 1\n');print(s.recv(64))"
grep -a "^\[DL\] i=" logs/jugar.log | tail -400 > logs/bug_<name>_dl.txt
```
Then bisect the guilty draws with `X <lo> <hi>` (each time: `S` to a new file, compare). The first `X` that makes the artefact disappear names the PRIM indices. Record the `[DL]` lines of those PRIMs: vtype, prim, tex, fmt, blend, alpha test, z test, fb.

- [ ] **Step 3: Oracle diff.** For the same screen in PPSSPP, use `tools/gelist.py` (usage in DEBUGGING.md) to dump PPSSPP's list over its debugger port and diff it with ours. Note which register or vertex differs.
- [ ] **Step 4: Classify** each bug in "Part A evidence" as one of:
  - (a) state mapping (blend, alpha test, depth, CLUT);
  - (b) vertex/transform;
  - (c) needs render-to-texture;
  - (d) game logic (a stub or HLE returning the wrong thing).

  Order the fixes by visibility: `negro.png` and `sin cofres.png` first.

### Task A2..An: One task per confirmed bug (write them from the A1 evidence)

For each bug, append a task to this file in the usual format before coding: files, failing test, fix, verification. Rules:
- **(a)/(b):** the failing test goes in the pure unit that computes the wrong value (`test_ge_blend`, `test_ge_vertex`, `test_ge_viewport`, `test_ge_texdecode`, …), with values from the PPSSPP list diff. The fix goes in that unit.
- **(c) render-to-texture:** read `docs/superpowers/plans/2026-10-09-m3-graphics-rtt.md` (RTT tasks) and the branch `wt-rtt` (`git log --oneline patapon3-windows..wt-rtt`). Rebase or merge it onto `patapon3-windows`, rerun its tests, and enable it for the failing screen only after the scale-1 baseline still shows 0 diff.
- **(d):** follow DEBUGGING.md (HLE trace around the screen; PPSSPP HLE semantics in `DECO/tools/ppsspp-src/Core/HLE`).
- **Verification:** the user checks the screen again. The scale-1 language-menu baseline must still show 0 diff, and a `to_square.py` hideout shot is compared visually with `DECO/build/shots/m3c_hideout_s4.png`.
- **Commit:** `fix(ge): <what> (<screen>)` or `fix(hle): …`.

---

## Part B — Game controller support

### Task B1: Pure pad mapping (SDL GameController → PSP buttons and analog)

**Files:**
- Create: `runtime/include/psp_pad.h`, `runtime/src/psp_pad.cpp`, `runtime/tests/test_pad.cpp`
- Modify: `runtime/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `uint32_t pad_button_to_psp(int sdl_button);`: `SDL_GameControllerButton` value → PSP mask, 0 if unmapped.
  - `uint8_t pad_axis_to_psp(int axis_value, int deadzone);`: -32768..32767 → 0..255, with 128 inside the dead zone.
  - `bool pad_trigger_pressed(int axis_value);`: trigger axis (0..32767) beyond 1/3 of its travel.
  - `class PadSet`, with:
    - `void set_button(int32_t pad, uint32_t mask, bool down);`
    - `void set_trigger(int32_t pad, bool left, bool down);`
    - `void set_stick(int32_t pad, uint8_t x, uint8_t y);`
    - `void remove(int32_t pad);`
    - `uint32_t buttons() const;` (OR of all pads)
    - `uint8_t analog_x() const;`, `uint8_t analog_y() const;` (the most recently moved pad, else 128)

  `PadSet` is not thread-safe; the event loop owns it and publishes atomics.

Mapping (PSP face positions, the PPSSPP default for XInput-style pads):

| SDL | PSP |
|-----|-----|
| A (bottom) | CROSS 0x4000 |
| B (right) | CIRCLE 0x2000 |
| X (left) | SQUARE 0x8000 |
| Y (top) | TRIANGLE 0x1000 |
| LEFTSHOULDER or left trigger | L 0x0100 |
| RIGHTSHOULDER or right trigger | R 0x0200 |
| START | START 0x0008 |
| BACK | SELECT 0x0001 |
| DPAD up/right/down/left | 0x0010 / 0x0020 / 0x0040 / 0x0080 |
| left stick | analog X/Y (Y down = larger, as on the PSP) |

- [ ] **Step 1: Failing test** `runtime/tests/test_pad.cpp`:

```cpp
// psp_pad: SDL GameController -> PSP buttons/analog (pure; no SDL linkage).
#include "psp_pad.h"

#include <SDL_gamecontroller.h>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static void buttons() {
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_A) == 0x4000, "A = CROSS");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_B) == 0x2000, "B = CIRCLE");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_X) == 0x8000, "X = SQUARE");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_Y) == 0x1000, "Y = TRIANGLE");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) == 0x0100, "LB = L");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) == 0x0200, "RB = R");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_START) == 0x0008, "START");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_BACK) == 0x0001, "BACK = SELECT");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_DPAD_UP) == 0x0010, "dpad up");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) == 0x0020, "dpad right");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_DPAD_DOWN) == 0x0040, "dpad down");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_DPAD_LEFT) == 0x0080, "dpad left");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_GUIDE) == 0, "guide unmapped");
}

static void deadzone_centres() {
    CHECK(pad_axis_to_psp(0, 8000) == 128, "rest = 128");
    CHECK(pad_axis_to_psp(7999, 8000) == 128, "inside dead zone = 128");
    CHECK(pad_axis_to_psp(-7999, 8000) == 128, "inside dead zone (neg) = 128");
    CHECK(pad_axis_to_psp(32767, 8000) == 255, "full right = 255");
    CHECK(pad_axis_to_psp(-32768, 8000) == 0, "full left = 0");
    const uint8_t half = pad_axis_to_psp(20384, 8000);  // halfway between dead zone and max
    CHECK(half > 180 && half < 200, "rescaled past the dead zone");
    CHECK(!pad_trigger_pressed(10000) && pad_trigger_pressed(12000), "trigger threshold ~1/3");
}

static void combine_sources() {
    PadSet p;
    p.set_button(1, 0x4000, true);
    p.set_button(2, 0x4000, true);
    p.set_button(1, 0x4000, false);
    CHECK(p.buttons() == 0x4000, "pad 2 still holds CROSS after pad 1 releases it");
    p.set_trigger(1, true, true);
    CHECK(p.buttons() == (0x4000 | 0x0100), "left trigger = L");
    p.set_button(1, 0x0100, true);
    p.set_trigger(1, true, false);
    CHECK((p.buttons() & 0x0100) != 0, "LB still holds L after the trigger releases");
}

static void unplug_releases() {
    PadSet p;
    p.set_button(7, 0x0010, true);
    p.set_stick(7, 10, 250);
    CHECK(p.analog_x() == 10 && p.analog_y() == 250, "stick of the moved pad");
    p.remove(7);
    CHECK(p.buttons() == 0, "unplugging releases its buttons");
    CHECK(p.analog_x() == 128 && p.analog_y() == 128, "and centres the stick");
}

int main() {
    buttons();
    deadzone_centres();
    combine_sources();
    unplug_releases();
    if (failures == 0) std::printf("test_pad: all passed\n");
    return failures == 0 ? 0 : 1;
}
```
CMake (next to the texrep tests):

```cmake
add_executable(test_pad tests/test_pad.cpp src/psp_pad.cpp)
target_include_directories(test_pad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${SDL2_INCLUDE_DIRS})
target_compile_options(test_pad PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME pad_tests COMMAND test_pad)
```
Also add `src/psp_pad.cpp` to `RUNTIME_SOURCES`. If `${SDL2_INCLUDE_DIRS}` is empty in this build, use the variable the runtime target uses for SDL headers: `grep -n "SDL2_INCLUDE" runtime/CMakeLists.txt`.

- [ ] **Step 2: Run** `cmake . && ninja test_pad`. Expected: error, `psp_pad.h` missing.
- [ ] **Step 3: Implement** `runtime/include/psp_pad.h`:

```cpp
#pragma once
// Game controller -> PSP input (pure). SDL GameController button/axis values
// in, PSP sceCtrl button mask and analog bytes out. Event-loop glue lives in
// psp_event_loop.cpp; this file has no SDL linkage (SDL enums are ints here).
#include <cstdint>
#include <map>

uint32_t pad_button_to_psp(int sdl_button);
uint8_t pad_axis_to_psp(int axis_value, int deadzone);
bool pad_trigger_pressed(int axis_value);

class PadSet {
public:
    void set_button(int32_t pad, uint32_t mask, bool down);
    void set_trigger(int32_t pad, bool left, bool down);
    void set_stick(int32_t pad, uint8_t x, uint8_t y);
    void remove(int32_t pad);
    uint32_t buttons() const;
    uint8_t analog_x() const;
    uint8_t analog_y() const;

private:
    struct Pad {
        uint32_t buttons = 0;
        bool ltrig = false, rtrig = false;
        uint8_t x = 128, y = 128;
    };
    std::map<int32_t, Pad> pads_;
    int32_t last_stick_pad_ = -1;
};
```
`runtime/src/psp_pad.cpp`:

```cpp
#include "psp_pad.h"

#include <algorithm>

uint32_t pad_button_to_psp(int b) {
    // SDL_GameControllerButton values (SDL 2.0.x, stable ABI).
    switch (b) {
    case 0: return 0x4000;   // A -> CROSS
    case 1: return 0x2000;   // B -> CIRCLE
    case 2: return 0x8000;   // X -> SQUARE
    case 3: return 0x1000;   // Y -> TRIANGLE
    case 4: return 0x0001;   // BACK -> SELECT
    case 6: return 0x0008;   // START
    case 9: return 0x0100;   // LEFTSHOULDER -> L
    case 10: return 0x0200;  // RIGHTSHOULDER -> R
    case 11: return 0x0010;  // DPAD_UP
    case 12: return 0x0040;  // DPAD_DOWN
    case 13: return 0x0080;  // DPAD_LEFT
    case 14: return 0x0020;  // DPAD_RIGHT
    default: return 0;
    }
}

uint8_t pad_axis_to_psp(int v, int deadzone) {
    if (v > -deadzone && v < deadzone) return 128;
    const double span = 32767.0 - deadzone;
    const double t = v > 0 ? (v - deadzone) / span : (v + deadzone) / span;  // -1..1
    const int out = static_cast<int>(128.0 + std::clamp(t, -1.0, 1.0) * 127.5);
    return static_cast<uint8_t>(std::clamp(out, 0, 255));
}

bool pad_trigger_pressed(int v) { return v > 32767 / 3; }

void PadSet::set_button(int32_t pad, uint32_t mask, bool down) {
    Pad& p = pads_[pad];
    p.buttons = down ? (p.buttons | mask) : (p.buttons & ~mask);
}

void PadSet::set_trigger(int32_t pad, bool left, bool down) {
    Pad& p = pads_[pad];
    (left ? p.ltrig : p.rtrig) = down;
}

void PadSet::set_stick(int32_t pad, uint8_t x, uint8_t y) {
    Pad& p = pads_[pad];
    p.x = x;
    p.y = y;
    last_stick_pad_ = pad;
}

void PadSet::remove(int32_t pad) {
    pads_.erase(pad);
    if (last_stick_pad_ == pad) last_stick_pad_ = -1;
}

uint32_t PadSet::buttons() const {
    uint32_t m = 0;
    for (const auto& [id, p] : pads_) {
        m |= p.buttons;
        if (p.ltrig) m |= 0x0100;
        if (p.rtrig) m |= 0x0200;
    }
    return m;
}

uint8_t PadSet::analog_x() const {
    const auto it = pads_.find(last_stick_pad_);
    return it == pads_.end() ? 128 : it->second.x;
}

uint8_t PadSet::analog_y() const {
    const auto it = pads_.find(last_stick_pad_);
    return it == pads_.end() ? 128 : it->second.y;
}
```
Check the halfway value by hand if the test fails: t = (20384 - 8000) / 24767 ≈ 0.5 → 128 + 63.75 = 191.

- [ ] **Step 4: Run** `ninja test_pad && ./test_pad.exe` → `all passed`. Then `ninja && ctest` (32/32).
- [ ] **Step 5: Commit** `feat(input): pure game-controller mapping (PadSet)`.

### Task B2: Event-loop glue and sceCtrl analog

**Files:** `runtime/src/psp_event_loop.cpp`, `runtime/include/psp_event_loop.h` (or wherever `g_host_buttons` is declared: `grep -rn "g_host_buttons" runtime/include`), `runtime/src/hle/psp_hle_ctrl.cpp`.

- [ ] **Step 1:** `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)`. If the GAMECONTROLLER subsystem fails, log `[RT] game controllers unavailable: <SDL_GetError()>` and continue with video only.
- [ ] **Step 2:** add atomics next to `g_host_buttons`: `std::atomic<uint32_t> g_pad_buttons{0}; std::atomic<uint16_t> g_pad_analog{0x8080};` (x low byte, y high byte), declared `extern` in the same header. Add a module-static `PadSet g_pads;` and `std::map<SDL_JoystickID, SDL_GameController*> g_controllers;`. In `handle_sdl_event`:
  - `SDL_CONTROLLERDEVICEADDED`: `SDL_GameControllerOpen(ev.cdevice.which)`. Store it by `SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c))` and log `[RT] controller connected: <SDL_GameControllerName(c)>`.
  - `SDL_CONTROLLERDEVICEREMOVED`: `g_pads.remove(ev.cdevice.which)`, close it, log it.
  - `SDL_CONTROLLERBUTTONDOWN/UP`: `g_pads.set_button(ev.cbutton.which, pad_button_to_psp(ev.cbutton.button), down)`.
  - `SDL_CONTROLLERAXISMOTION`:
    - triggers (`SDL_CONTROLLER_AXIS_TRIGGERLEFT/RIGHT`) → `set_trigger(which, left, pad_trigger_pressed(value))`;
    - `LEFTX/LEFTY` → update that pad's x/y with `pad_axis_to_psp(value, 8000)`. Keep the other axis from the last event: `PadSet` stores both, so read the current pair, change one, and call `set_stick`. Add `uint8_t PadSet::stick_x(int32_t)` / `stick_y` accessors if needed, with a test.
  - After any of these: `g_pad_buttons.store(g_pads.buttons()); g_pad_analog.store(g_pads.analog_x() | (g_pads.analog_y() << 8));`.
  - On `SDL_WINDOWEVENT_FOCUS_LOST`: leave pads alone (SDL keeps delivering controller events without focus).
- [ ] **Step 3:** In `psp_hle_ctrl.cpp`, OR `g_pad_buttons` into `buttons`, and write the analog bytes from `g_pad_analog` instead of the constant 128s.
- [ ] **Step 4:**
  - `ninja && ctest`.
  - Check the off path: with no controller plugged in, the scale-1 baseline still shows 0 diff.
  - Ask the user (Spanish) to plug in their controller and play the hideout, the SQUARE menu and a mission. Check rhythm-input responsiveness and that nothing moves on its own at rest.
  - Update `DECO/jugar.bat`'s comment line with the pad mapping.
- [ ] **Step 5: Docs + commit.** README "Building and Running": add one sentence on controller support and the mapping table. DEBUGGING.md: the `[RT] controller connected` log line. Commit `feat(input): SDL game controllers drive sceCtrl buttons and the analog stick`.

---

## Part C — Performance

### Task C1: `PSPRECOMP_PERF=1` per-second counters (pure formatter + wiring)

**Files:** create `runtime/include/psp_perf.h`, `runtime/src/psp_perf.cpp`, `runtime/tests/test_perf.cpp`. Modify `psp_ge.cpp` (list timing), `psp_ge_draw.cpp` (presents, draw calls), `psp_ge_texture.cpp` (binds, misses, hashed bytes), `CMakeLists.txt`.

**Interfaces:**
- `struct PerfCounters { std::atomic<uint64_t> presents, ge_lists, ge_ns, prims, draw_calls, tex_binds, tex_misses, tex_hash_bytes, cpu_wait_ns; };` with a global `extern PerfCounters g_perf;`.
- `struct PerfSnapshot { uint64_t presents, ge_lists, ge_ns, prims, draw_calls, tex_binds, tex_misses, tex_hash_bytes, cpu_wait_ns; };` and `PerfSnapshot perf_snapshot();`.
- `std::string perf_format(const PerfSnapshot& a, const PerfSnapshot& b, double seconds);`. Pure. It returns `[PERF] fps=29.9 ge_ms/frame=12.3 prims/frame=870 draws/frame=860 binds/frame=410 tex_miss/s=2 tex_hash_MB/frame=35.1 cpu_wait_ms/s=0`.
- `bool perf_enabled();` reads `PSPRECOMP_PERF` once.

- [ ] **Step 1: Failing test** `test_perf.cpp`. Feed two snapshots 2 s apart:
  - `presents` 0 → 60, `ge_ns` 0 → 60·12'345'678, `prims` → 60·870, `tex_hash_bytes` → 60·35·2^20.
  - Assert the string contains `fps=30.0`, `ge_ms/frame=12.3`, `prims/frame=870` and `tex_hash_MB/frame=35.0`.
  - Assert `presents` 0 → 0 gives `fps=0.0` and per-frame fields `0` (no division by zero).
- [ ] **Step 2: RED**, then implement `perf_format` with `snprintf`. Per-frame values divide by `max(1, presents delta)`.
- [ ] **Step 3: Wire the counters** (each one guarded by `if (perf_enabled())`, so the off path costs nothing):
  - `ge_ns` and `ge_lists` around the list interpreter's main loop in `psp_ge.cpp`, with `std::chrono::steady_clock`;
  - `presents` in `present_blit`;
  - `prims` and `draw_calls` in `ge_draw_prim` (before `glDrawArrays`);
  - `tex_binds`, `tex_misses` and `tex_hash_bytes` (`+= span`) in `ge_texture_bind`;
  - `cpu_wait_ns` where the CPU lock waits (find it: `grep -n "waited 5 s" runtime/src/*.cpp`).

  The main loop logs `perf_format` every 2 s when enabled. Find where the event loop iterates: `grep -n "while (!g_should_exit" runtime/src/*.cpp`.
- [ ] **Step 4:** `ninja && ctest`, then commit `feat(perf): PSPRECOMP_PERF per-second frame counters`.

### Task C2: Measure (no fixes yet)

- [ ] **Step 1: Hideout, scale 1, no pack** (the user's 23 fps case):

```bash
cd /c/Users/torso/Documents/deco/build
timeout 400 python -I tools/to_square.py perf_s1 PSPRECOMP_PERF=1
python -I -c "import time;time.sleep(30)"; taskkill //IM psprecomp_runtime.exe //F
grep -a "^\[PERF\]" logs/perf_s1.log | tail -12
```
Then repeat with `PSPRECOMP_RENDER_SCALE=4 PSPRECOMP_TEXTURES=C:/Users/torso/Documents/deco/PPSSPP/memstick/PSP/TEXTURES/INFN00001` (tag `perf_s4hd`). Record both in "Part C evidence".

**Reading the numbers:** the PSP target is 30 fps (33.3 ms per frame).
- `ge_ms/frame` close to 30 means GE/GL work on the main thread is the bottleneck.
- A large `tex_hash_MB/frame` points at the per-bind FNV re-hash (see C3).
- `cpu_wait_ms/s` high means the game threads are blocked on the CPU lock.
- If none of these explains it, profile the main thread with Windows' built-in sampler: `wpr -start CPU` … `wpr -stop out.etl`. Check `where wpr` first; if it is missing, use `perf`-style manual timers around suspect blocks.

- [ ] **Step 2: Title transition.** Run `flow.py` (it skips the movie with START) with `PSPRECOMP_HLE_TRACE=1` and timestamps:
  - Add a monotonic ms prefix to `[HLE]` lines if they do not have one: check `grep -n "HLE_TRACE" runtime/src/hle/*.cpp`, and add the prefix only under the trace flag.
  - Find the gap between the START press that skips the movie and the first title-menu draw.
  - List what runs in that gap: file reads (`sceIoRead` sizes and counts), waits (`sceKernelWait*` with timeouts, `sceMpeg*` after the skip), `[CPU] waited` lines.
  - Compare with PPSSPP if needed: the same skip there is fast per the user.
- [ ] **Step 3: Record** both findings in "Part C evidence" and write the fix tasks (C3, C4) from them. Leading hypotheses, to confirm or reject with the numbers:
  1. **Texture re-hash on every bind.** `ge_texture_bind` runs FNV-1a, byte by byte, over the texture's whole span (plus 1 KiB of CLUT) on every bind, and the hideout binds hundreds of textures per frame.
  2. **Per-PRIM overhead.** A `std::vector` allocation for decoded vertices, uniforms re-uploaded, and `glBufferData` per PRIM (about 870 PRIMs per list in the hideout).
  3. **Transition.** A wait with a timeout after the movie skip (e.g. the movie thread waiting for a ring-buffer callback that never comes, until the timeout), or many small synchronous file reads.

### Task C3: (expected) Hash each texture at most once per frame

Do this only if C2 shows `tex_hash_MB/frame` is significant (more than about 5 ms of the frame, roughly 5 MB/frame at 1 GB/s FNV).

- [ ] **Failing test first:** extract the cache-check decision into a pure helper. Example: `bool tex_needs_rehash(uint32_t entry_frame_hashed, uint32_t frame_now)` plus a small memo keyed by `(addr, params)`, unit-tested in a new `test_tex_memo.cpp`. Cases:
  - same frame → reuse the hash;
  - next frame → rehash (`rehash_next_frame`);
  - different params at the same address in the same frame → rehash.
- [ ] Use it in `ge_texture_bind`, so the FNV runs once per (texture, frame).
- [ ] **Verify:**
  - the opening movie still animates, since its 512x512 frames are rewritten every frame;
  - fonts update (menus);
  - scale-1 baseline 0 diff;
  - `[PERF]` before/after recorded.
- [ ] Commit `perf(ge): hash each texture at most once per frame`.

### Task C4..: Fix whatever else C2 measured

Same pattern: a failing test for the pure part where one exists, the change, a before/after `[PERF]` line, user confirmation of fps in the hideout and of the transition time. Target 30 fps steady in the hideout at scale 1 and at scale 4 with the pack.

### Task C5: Clean shutdown (the post-`Shutdown complete` crash)

- [ ] **Reproduce:** close the window during the hideout. The log shows `[RT] Shutdown complete` followed by `[CRASH] exception 0xC0000005 … last_guest_func=…`. Guest threads that "did not join in 2s" were detached and still run guest code while the runtime frees memory.
- [ ] **Fix the root cause:** after shutdown, either do not free guest memory or GL state while detached threads may still run, or end the process with `std::_Exit(0)` / `TerminateProcess` once logs are flushed and saves are written (check where saves are flushed first: `grep -n "Shutdown complete" runtime/src/*.cpp`). Prefer exiting without running teardown that races detached threads. Add a log line `[RT] exiting with N detached threads`.
- [ ] **Verify:** close the window three times from the hideout: no `[CRASH]` line, and the save still loads next boot. Commit `fix(rt): exit without racing detached guest threads`.

---

## Part D — Docs, handoff, push

- [ ] README/DEBUGGING.md/GRAPHICS.md updated by the tasks above, kept true (CLAUDE.md §8).
- [ ] HANDOFF.md:
  - status: what was fixed (bugs, gamepad, fps numbers);
  - log line;
  - next item: multiplayer (below).
- [ ] Gates, then `git push p3 patapon3-windows:main` and `git ls-remote p3 refs/heads/main` == HEAD.

## After this plan

1. **Multiplayer via Paragate (patapon.net).** Paragate is run by Madwig, the DxD author. It is PPSSPP's PRO ad hoc server with "Server-provided packet relay"; players set PPSSPP's ad hoc server to `patapon.net`. It is **not** a PSN/infrastructure revival.
   - The game imports `sceNetAdhoc*` (PDP + PTP), `sceNetAdhocMatching*` and `sceNetAdhocctl*`; none are implemented.
   - Plan: brainstorm → spec → plan for an ad hoc layer compatible with PPSSPP's protocol.
   - References: `DECO/tools/ppsspp-src/Core/HLE/proAdhoc.cpp`, `sceNetAdhoc.cpp`, `sceNetAdhocMatching.cpp`, `proAdhocServer.cpp`, `ext/aemu_postoffice`.
   - Test locally first: two runtime instances, or the runtime plus the user's PPSSPP, against a local PRO ad hoc server.
   - Paragate has anti-cheat, and the DxD anti-tamper checks fail in our runtime (HANDOFF). Ask the user to contact Madwig before pointing the port at `patapon.net`.
2. Deferred minors from M3c: VRAM cap, mipmaps for replacements, skip hashing video frames.

## Part A evidence

(fill in per bug: user description, how to reach, PRIM indices from `X`, `[DL]` state, PPSSPP diff, class a/b/c/d)

## Part C evidence

(fill in: `[PERF]` lines for perf_s1 and perf_s4hd, transition gap analysis)
