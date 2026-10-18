# Launcher part 5b, Plan 2 — Achievements in the game (detection, unlocked.json, toast) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **Execution (proposed, like Plan 1; the user confirms at the start of the session):** Task 1 (reverse
> engineering) and Task 8 (end to end, performance gate, Findings) are Claude's. Tasks 2–7 are briefs for
> **DeepSeek v4.1 flash at max effort** (`python ~/.claude/skills/albanil-grok/grok-build.py <brief> --repo
> C:/Users/torso/Documents/deco --model "deepseek/deepseek-flash#max" --timeout 3600 --msg "<no-commit message>"`),
> one at a time, briefs in `DECO/build/briefs_p5b2/` (template: `DECO/build/briefs_p5b/T1.md`: plan path, line
> range, "verbatim", files, no commit, gates, report). **Before every run `Get-Process opencode` must be empty.**
> Claude diffs every new file against this plan's code blocks, re-runs the gates and commits.

**Goal:** While the player plays, the game reads its own progress struct in guest RAM once a second, grants the
official achievements whose condition holds, records them in `<portable root>/achievements/unlocked.json` (which
the launcher's Logros screen already reads) and shows a toast — at no measurable frame cost.

**Architecture:** A pure engine in the runtime core (`psp_achievements.*`) compiles the official catalogue
(`achievements.json` next to the exe) against a table of named facts that the game module provides per release
(`games/patapon3/runtime/p3_achievements.*`), and evaluates the pending checks straight from guest RAM. A vblank
hook runs one check every 60 vblanks on the game thread; a granted achievement goes into a lock-free ring; a
low-priority worker appends it to `unlocked.json` (temp file + rename) and rasterizes the toast text, which the GL
thread uploads once and draws over the game image for 4 s with the launcher theme's colours.

**Tech Stack:** C++17 runtime (clang/MinGW, CMake + Ninja, ctest), nlohmann/json (vendored), stb_truetype (the
overlay's), Python 3 tools in `DECO/build/tools/` for the debug socket (port 9998). Launcher (Rust/TS) only in
Task 7 (catalogue test threshold) and Task 8 (E2E).

**Spec:** `docs/superpowers/specs/2026-10-18-launcher-part5b-themes-achievements-design.md` §2, §4, §5, §6 (read
them first). Plan 1 (launcher) shipped: `docs/superpowers/plans/2026-10-18-launcher-part5b-plan1-themes-achievements.md`
(Findings at its end). Contract: `docs/contracts/achievements-v1.md`.

## Global Constraints

- `<root>/achievements/unlocked.json` = `{"version": 1, "earned": {"<id>": {"date": "<ISO-8601 UTC>", "save":
  "DATA04", "hero": "Edson", "release": "dxd-1.1.4"}}}`. Global on the PC, **append-only, written only by the
  game** (temp file + rename). Entries already in the file are never changed or removed (also ids this build does
  not know). A file with another `version` is never touched.
- Catalogue `achievements.json` next to the exe (copied by `runtime/CMakeLists.txt`, Plan 1): `{"version": 1,
  "achievements": [{"id", "icon", "hidden", "name": {"es","en"}, "desc": {"es","en"}, "when"?}]}`; `when` =
  `{"fact", "op": ">=" | "==" | "!=", "value": <integer>}` or `{"all": [leaf...]}` / `{"any": [leaf...]}` (one
  level, no nesting). An `id` never changes once pushed with a `when`; a dropped id is never reused.
- An achievement enters the catalogue only when its fact is **verified** (spec §4.2): present as expected in the
  user's DATA04, absent or lower in DATA08, and — for a statistic — equal to what the game shows. Unverifiable ones
  are dropped, never guessed.
- Off unless `PSPRECOMP_ACHIEVEMENTS_FILE` is set (the settings reader sets it from `settings.ini`'s folder);
  `PSPRECOMP_ACHIEVEMENTS=0` turns everything off (no catalogue, no worker, no hook). A release without a field
  map has no achievements (one `[ACH]` line). Nothing in achievements may stop or slow the game: at worst an
  `[ACH]` line in `logs/game.log`.
- **Performance above the feature** (spec §5): one decrement + compare on 59 of 60 vblanks; a check reads only the
  bytes the pending checks name, no allocation, no JSON, no strings, **< 20 µs** (micro-benchmark test); no I/O or
  text work on the game or GL threads; a toast costs one texture upload and one quad per frame only while visible;
  `build/tools/perf_hideout.py` gate before/after.
- Toast: top right of the game image, ~4 s, "Logro desbloqueado" / "Achievement unlocked" + icon and name in
  `PSPRECOMP_UI_LANG`; more than 3 earned in the same check collapse into one "🏆 N logros"; never blocks input;
  `[game] achievement_toasts = false` (env `PSPRECOMP_ACHIEVEMENT_TOASTS=0`) hides toasts, achievements are still
  recorded. Colours from `PSPRECOMP_UI_COLORS` (`menu_prompt_colors`, already shipped).
- Guest RAM: `rdram[addr & 0x07FFFFFFu]`; user RAM is `[0x08800000, 0x0A000000)`. No game-specific data in
  `runtime/src` (CLAUDE.md §3): addresses, offsets and release ids live in `games/patapon3/runtime/`.
- Saves: the user's decrypted saves (`DECO/build/save_main/plain_DATA04.bin`, `plain_DATA08.bin`, both 178 848 B,
  both layers removed) are **never committed**; tests that use them skip when the files are absent.
- Commits: psprecomp branch `patapon3-windows`, launcher `main`, with
  `git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit`; body ends with `Verified: …`
  and `Agent: …`; **no `Co-Authored-By`**. Push nothing without the user's OK (psprecomp goes to `p3/main` by
  cherry-pick in the `push-p3-6` worktree, never the icon commit f639180).
- Code, comments and commits in English; the user is spoken to in Spanish.

## Review Focus

1. **No save loaded / title screen / mid-load:** the struct pointer is 0, outside user RAM or points at garbage —
   nothing is granted and no streak survives (Task 2 `garbage_never_grants`).
2. **Imported DATA04 (1878 h):** ~20 achievements hold on the first two checks — all recorded in one file write,
   one grouped toast, not 20 toasts (Task 6 `a_big_batch_is_one_write_and_one_toast`, Task 5 `summary_over_three`).
3. **`unlocked.json` corrupt, from a newer build, or with ids this catalogue dropped:** a corrupt file is kept as
   `unlocked.corrupt.json` before rewriting; a newer-version file is never touched; unknown ids survive
   (Task 3 `existing_entries_are_never_lost`, `newer_file_is_never_touched`).
4. **The player leaves right after a grant (F1 → Sí, exit code 10, or closes the window):** stop() drains the ring
   before exit; if the process dies first, the next session re-detects it (nothing earned is lost for good)
   (Task 6 `stop_drains_the_ring`).
5. **No `settings.ini` (developer `jugar.bat`) or `PSPRECOMP_ACHIEVEMENTS=0`:** no hook is installed and the game
   runs exactly as before (Task 6 `off_without_file_or_with_kill_switch`).

## File map

psprecomp (`DECO/psprecomp`, branch `patapon3-windows`):
- T1: `docs/p3_achievement_facts.md` (new, the verified field map and evidence).
- T2: `runtime/include/psp_achievements.h`, `runtime/src/psp_achievements.cpp`, `runtime/tests/test_achievements.cpp`
  (new), `runtime/CMakeLists.txt`.
- T3: `runtime/include/psp_ach_store.h`, `runtime/src/psp_ach_store.cpp`, `runtime/tests/test_ach_store.cpp` (new),
  `runtime/CMakeLists.txt`.
- T4: `runtime/src/psp_settings.cpp`, `runtime/tests/test_settings.cpp`, `games/patapon3/game_options.json`,
  `docs/contracts/settings-v1.md`, `docs/ENV_FLAGS.md`.
- T5: `runtime/include/psp_toast.h`, `runtime/src/psp_toast.cpp`, `runtime/tests/test_toast.cpp` (new),
  `runtime/include/psp_osk_overlay.h`, `runtime/src/psp_osk_overlay.cpp`, `runtime/src/psp_ge_draw.cpp`,
  `runtime/CMakeLists.txt`.
- T6: `runtime/include/psp_vblank_hook.h`, `runtime/src/psp_vblank_hook.cpp`, `runtime/include/psp_achievements_run.h`,
  `runtime/src/psp_achievements_run.cpp`, `runtime/tests/test_achievements_run.cpp` (new),
  `runtime/src/hle/psp_hle_display.cpp`, `runtime/src/main.cpp`, `runtime/CMakeLists.txt`, `docs/ENV_FLAGS.md`.
- T7: `games/patapon3/runtime/p3_achievements.h`, `games/patapon3/runtime/p3_achievements.cpp`,
  `runtime/tests/test_p3_achievements.cpp` (new), `games/patapon3/runtime/hooks_main.cpp`,
  `games/patapon3/achievements.json`, `docs/contracts/achievements-v1.md`, `runtime/CMakeLists.txt`.
  Launcher (`DECO/launcher`, `main`): `src-tauri/src/achievements.rs` (catalogue test threshold), and only if
  `story_prologue` is dropped: `themes/arena/theme.json`, `scripts/themes.test.mjs`, `README.md`.
- T8: `docs/superpowers/plans/2026-10-18-launcher-part5b-plan2-game-achievements.md` (Findings).

Gates (exact commands, Git Bash):
- Build + tests: `cmake --build C:/Users/torso/Documents/deco/build/rtw 2>&1 | grep -E "error|warning: |FAILED" ;
  cd C:/Users/torso/Documents/deco/build/rtw && ctest 2>&1 | tail -3` (baseline 2026-10-18: 77/77).
- One test: `cmake --build C:/Users/torso/Documents/deco/build/rtw --target <test> && C:/Users/torso/Documents/deco/build/rtw/<test>.exe`.
- If the exe is locked (the user is playing from `build/rtw`), stop and report; never kill the user's game (port 9999).

---

### Task 1: Find the progress struct and verify the facts (Claude, reverse engineering)

**Files:**
- Create: `docs/p3_achievement_facts.md`
- Local only (not committed, `DECO/build` is not a repo): `DECO/build/tools/ach_probe.py`

**Interfaces:**
- Produces, in `docs/p3_achievement_facts.md`: per release (`dxd-1.1.4` required; `vanilla-eu`, `dxd-1.1.5b` when
  found within the time box) the **pointer chain** to the struct as `global, off1[, off2…]` meaning
  `base = rd32(...rd32(rd32(global) + off1)... + offN)` (no offsets = `base = rd32(global)`); and one table of
  **facts** (same offsets for every release, since the file layout is the struct layout):
  `| fact | offset | size | signed | bit | count | min | max | DATA04 | DATA08 | evidence |`
  (`bit` = -1 or 0..31; `count` = 0 or N entries of `size` bytes counted when non-zero), plus the **catalogue
  decision** for each of the 24 draft ids: `keep` with its `when` JSON, or `drop` with the reason, and the exact
  list of ids DATA04 earns and DATA08 earns. Task 7 copies these verbatim.

- [ ] **Step 1: Probe helper.** Write `DECO/build/tools/ach_probe.py` (port 9998 only; never 9999):

```python
"""ach_probe.py: read guest RAM of the runtime on port 9998 (R command).
  ach_probe.py rd <hexaddr> <len>          hex dump (u32 words)
  ach_probe.py find16 <text>               UTF-16LE hits of text in user RAM
  ach_probe.py findword <hexvalue>         addresses holding that u32 (aligned) in user RAM
  ach_probe.py dump <out.bin> <hexaddr> <len>"""
import socket, struct, sys
PORT = 9998
LO, HI, CH = 0x08800000, 0x0A000000, 0x10000
def rd(a, n):
    s = socket.create_connection(('127.0.0.1', PORT), timeout=20); s.sendall(f'R {a:08X} {n}\n'.encode())
    f = s.makefile('rb'); hdr = f.readline(); d = f.read(int(hdr.split()[1]) if hdr.startswith(b'OK') else 0); s.close(); return d
def ram():
    return b''.join(rd(a, CH) for a in range(LO, HI, CH))
cmd = sys.argv[1]
if cmd == 'rd':
    a, n = int(sys.argv[2], 16), int(sys.argv[3]); d = rd(a, n)
    for i in range(0, n, 16):
        print(f'{a + i:08X}: ' + ' '.join(f'{w:08X}' for w in struct.unpack_from(f'<{min(16, n - i) // 4}I', d, i)))
elif cmd == 'find16':
    b, p = ram(), sys.argv[2].encode('utf-16-le'); i = b.find(p)
    while i >= 0: print(f'{LO + i:08X}'); i = b.find(p, i + 1)
elif cmd == 'findword':
    b, v = ram(), struct.pack('<I', int(sys.argv[2], 16)); i = b.find(v)
    while i >= 0:
        if i % 4 == 0: print(f'{LO + i:08X}')
        i = b.find(v, i + 1)
elif cmd == 'dump':
    a, n = int(sys.argv[3], 16), int(sys.argv[4]); open(sys.argv[2], 'wb').write(b''.join(rd(x, min(CH, a + n - x)) for x in range(a, a + n, CH)))
```

- [ ] **Step 2: Boot DxD 1.1.4 with the user's save on port 9998** (a second instance; the user's own game stays
  on 9999). Save dir `DECO/build/save_main/test` holds `UCES01421_DATA00` + `UCES01421_DATA04`:

```bash
cd /c/Users/torso/Documents/deco/build/tools
P3DRIVE_SAVE=C:/Users/torso/Documents/deco/build/save_main/test python -I -c "
import sys; sys.path.insert(0,'.'); from p3drive import Instance
g = Instance('ach_dxd114', port=9998, env={'PSPRECOMP_SAVE_SLOT': '_DATA04'}); g.launch(); g.boot_to_hideout(); print('pid', g.proc.pid)"
```

  Expected: `hideout` printed; `DECO/build/shots/ach_dxd114/hideout.tga` is the Cuartel with Edson.

- [ ] **Step 3: Locate the struct.** `python -I ach_probe.py find16 Edson` → every hit `H`; the struct base is
  `S = H - 0xF1C8` for the hit where `rd S+0x9520` is `4` (Yumiyacha). Dump it and compare with the file:
  `python -I ach_probe.py dump ram_DATA04.bin <S> 178848` then
  `python -I -c "a=open('ram_DATA04.bin','rb').read(); b=open('../save_main/plain_DATA04.bin','rb').read(); print(sum(x!=y for x,y in zip(a,b)))"`
  Expected: few differing bytes (runtime-only fields). Record S and the count.

- [ ] **Step 4: The chain.** `python -I ach_probe.py rd 08AABD94 4` → `P`; `rd <P+0x50> 4` → must equal `S` (the
  RichPresence chain). If not: `findword <S>` → holders `H1…`; for each, look for a static pointer to the object
  that holds it (`findword` of `H - k` for k = 0..0x200 step 4 inside the module image `0x08804000..0x08BC6480`);
  prefer the shortest chain from a static global. Restart the game (Step 2 again) and confirm the chain still
  gives the struct (heap addresses may move; the chain must not). Then load DATA08 the same way
  (`PSPRECOMP_SAVE_SLOT=_DATA08` with a save dir holding DATA08 — make one: copy `save_main/test` to
  `save_main/test08` and replace DATA04 by the user's `UCES01421_DATA08` folder, decrypted like DATA04 per
  memory `p3-main-save`; if that is not possible in the time box, verify DATA08 facts on the file only).
  Also check the chain on the title screen (before Continue): base must be 0 or fail the sanity check of Task 7
  (hero name printable, class 1..34) — write what it is.

- [ ] **Step 5: Facts.** Diff `plain_DATA04.bin` vs `plain_DATA08.bin` (both in `DECO/build/save_main/`) for
  counters (u16/u32 where DATA04 ≫ DATA08), level fields (hero level 35 in DATA04 per the Cuartel), flag arrays
  (Dark Heroes, Archfiends, classes unlocked) and story progress. Confirm each candidate **live**: in the 9998
  instance change state (clear one mission with `build/tools/autoplay.py`/`to_mission.sh` on a copy of the save,
  or compare before/after a known event) and watch the offset move as expected; for statistics compare with what
  the game shows (hero level and class in the Cuartel; any record screen the game has). Reference names:
  `DECO/build/ext_ref_p5/PPEditor` (param files), `PataponRichPressence` (class list 0..34). Fill the table; every
  row needs evidence (what was seen, where). Plausible `min`/`max` per fact (e.g. level 1..99, counters 0..999999).

- [ ] **Step 6: Catalogue decision.** For each of the 24 ids in `games/patapon3/achievements.json`: `keep` +
  `when` (one leaf, or `all`/`any` of leaves over the table's facts) or `drop` + reason. Rule: only verified facts.
  `story_prologue` is wanted (theme `arena` locks on it); if it cannot be verified, name the verified early
  achievement that replaces it as arena's lock. Then compute, with the table, the exact earned lists for DATA04 and
  DATA08 and write them in the doc.

- [ ] **Step 7: Other releases (time box 45 min).** Same Steps 2–4 for vanilla EU
  (`P3DRIVE_EXE=../vanilla/rtw/psprecomp_runtime.exe P3DRIVE_DISC0=C:/Users/torso/Documents/deco/build/vanilla/disc0`
  — check those paths exist first) and DxD 1.1.5b (`../v115b/rtw/…`, `build/v115b/disc0`). Record each chain, or
  "not found (time box)" — such a release ships without achievements (spec §2).

- [ ] **Step 8: Close the 9998 instance** (`taskkill /PID <pid printed in Step 2> /F` — only that pid) and commit:

```bash
cd /c/Users/torso/Documents/deco/psprecomp
git add docs/p3_achievement_facts.md
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "docs(p3): achievement field map, verified facts and catalogue decisions" -m "Verified: struct found at <S> via <chain> on dxd-1.1.4 (port 9998, DATA04), <n> facts confirmed live" -m "Agent: Claude Opus 5.5"
```

---

### Task 2: Achievement engine — facts, catalogue compile, evaluation (pure)

**Files:**
- Create: `runtime/include/psp_achievements.h`, `runtime/src/psp_achievements.cpp`, `runtime/tests/test_achievements.cpp`
- Modify: `runtime/CMakeLists.txt` (source list + test)

**Interfaces:**
- Produces: `PspFact`, `PspAchievementSource`, `PspAchOp`, `PspAchLeaf`, `PspAchPending`, `PspAchChecks`,
  `PspAchEntry`, `PspAchCatalogue`, `PspAchBatch`, `template <size_t N> class PspAchRing`,
  `int64_t psp_ach_read(const PspFact&, const uint8_t* rdram, uint32_t base)`,
  `PspAchCatalogue psp_ach_compile(const std::string& json, const PspAchievementSource&, const std::vector<std::string>& earned)`,
  `size_t psp_ach_evaluate(PspAchChecks&, const PspAchievementSource&, const uint8_t* rdram, uint16_t* granted, size_t max)`.

- [ ] **Step 0: Keep the performance baseline exe** (Task 8 compares against it; Claude does this before the
  first brief): `cp C:/Users/torso/Documents/deco/build/rtw/psprecomp_runtime.exe C:/Users/torso/Documents/deco/build/rtw/psprecomp_runtime_pre_ach.exe`.

- [ ] **Step 1: Header** `runtime/include/psp_achievements.h`:

```cpp
#pragma once
// Achievements (docs/contracts/achievements-v1.md; spec 2026-10-18 launcher part 5b §4-§5). The game
// module names facts in its progress struct (PspAchievementSource); the official catalogue says which
// value of which fact earns what. This core is pure: no threads, no I/O, no getenv. A check reads only
// the bytes its pending achievements name and never allocates.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One readable value in the progress struct, relative to its base.
struct PspFact {
    const char* name;   // e.g. "hero.level"
    uint32_t offset;
    uint8_t size;       // 1, 2 or 4 bytes, little endian
    bool is_signed;
    int8_t bit;         // -1 = the whole value; 0..31 = that bit of it (0 or 1)
    uint16_t count;     // 0 = one value; N = how many of N entries (`size` bytes each) are non-zero
    int64_t min, max;   // plausible range; a value outside it means the struct is not valid right now
};

// What the game module provides for one release.
struct PspAchievementSource {
    const char* release;                               // release.json id, e.g. "dxd-1.1.4"
    uint32_t (*base)(const uint8_t* rdram);            // struct address now (0 = none)
    bool (*sane)(const uint8_t* rdram, uint32_t base); // cheap plausibility check of the struct
    void (*hero)(const uint8_t* rdram, uint32_t base, char16_t out[17]);  // hero name, 0-terminated
    uint32_t size;                                     // struct size in bytes
    const PspFact* facts;
    size_t fact_count;
};

enum class PspAchOp : uint8_t { Ge, Eq, Ne };
struct PspAchLeaf { uint16_t fact; PspAchOp op; int64_t value; };

// One achievement not earned yet: its leaves are checks.leaves[first, first + n).
struct PspAchPending {
    uint16_t index;   // position in the catalogue
    bool any;         // true: any leaf holds; false: every leaf holds
    uint8_t streak;   // consecutive valid checks in which the condition held
    uint16_t first, n;
};

struct PspAchChecks {
    std::vector<PspAchLeaf> leaves;
    std::vector<PspAchPending> pending;
    std::vector<uint16_t> used;      // facts the pending checks read, once each
    std::vector<int64_t> values;     // scratch, one per source fact (sized at compile time)
};

struct PspAchEntry {
    std::string id, icon, name_es, name_en;
    bool hidden = false, has_when = false;
};

struct PspAchCatalogue {
    std::vector<PspAchEntry> entries;  // every catalogue entry, in file order (index = position)
    PspAchChecks checks;
    std::vector<std::string> log;      // "[ACH] ..." lines
};

// The value of one fact (count / bit / signed rules of PspFact).
int64_t psp_ach_read(const PspFact& f, const uint8_t* rdram, uint32_t base);

// Parses the catalogue and compiles every entry that has a `when` and is not in `earned` into checks
// over src.facts. An entry whose `when` names an unknown fact, has a bad op, a non-integer value, an
// empty or nested all/any is left out with one log line. At most 255 entries.
PspAchCatalogue psp_ach_compile(const std::string& json, const PspAchievementSource& src,
                                const std::vector<std::string>& earned);

// One check. Grants (writes catalogue indices into granted[0..max), removes them from checks.pending)
// the achievements whose condition held in this valid check and the previous one. An invalid struct
// (no base, outside user RAM, sane() false, a used fact outside its range) grants nothing and resets
// every streak. Returns how many were granted.
size_t psp_ach_evaluate(PspAchChecks& checks, const PspAchievementSource& src, const uint8_t* rdram,
                        uint16_t* granted, size_t max);

// Everything one check granted, on its way from the game thread to the worker (one push per check,
// so the worker never splits a batch: more than 3 = one summary toast).
constexpr size_t kAchBatchMax = 64;
struct PspAchBatch { uint16_t n; uint16_t index[kAchBatchMax]; char16_t hero[17]; };

// Single-producer single-consumer ring (game thread pushes, worker pops). Lock-free.
template <size_t N>
class PspAchRing {
public:
    bool push(const PspAchBatch& g) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N) return false;
        buf_[h % N] = g;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(PspAchBatch* out) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        *out = buf_[t % N];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
private:
    std::array<PspAchBatch, N> buf_{};
    std::atomic<size_t> head_{0}, tail_{0};
};
```

- [ ] **Step 2: Failing tests** `runtime/tests/test_achievements.cpp`:

```cpp
// Achievement engine (psp_achievements.h): reads, compile rules, the two-check rule, garbage
// rejection, shrinking pending list and the < 20 us budget of one check.
#include "psp_achievements.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// 16 MB of guest RAM is enough: addresses 0x08000000..0x08FFFFFF mask into it.
static std::vector<uint8_t> g_ram(0x01000000, 0);
constexpr uint32_t kPtr = 0x08AA0000u, kBase = 0x08C00000u;
static void w8(uint32_t a, uint8_t v) { g_ram[a & 0x07FFFFFFu] = v; }
static void w16(uint32_t a, uint16_t v) { w8(a, v & 0xFF); w8(a + 1, v >> 8); }
static void w32(uint32_t a, uint32_t v) { w16(a, v & 0xFFFF); w16(a + 2, v >> 16); }
static uint32_t r32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(ram[(a + i) & 0x07FFFFFFu]) << (8 * i);
    return v;
}

static const PspFact kFacts[] = {
    {"missions", 0x10, 4, false, -1, 0, 0, 100000},
    {"level", 0x20, 1, false, -1, 0, 1, 99},
    {"flags.ragewolf", 0x30, 4, false, 3, 0, 0, 1},
    {"classes", 0x40, 1, false, -1, 8, 0, 8},
    {"temp", 0x50, 2, true, -1, 0, -1000, 1000},
};
static uint32_t test_base(const uint8_t* ram) { return r32(ram, kPtr); }
static bool test_sane(const uint8_t* ram, uint32_t base) { return ram[base & 0x07FFFFFFu] == 'E'; }
static void test_hero(const uint8_t*, uint32_t, char16_t out[17]) { out[0] = u'E'; out[1] = 0; }
static const PspAchievementSource kSrc = {"test-1", test_base, test_sane, test_hero, 0x100, kFacts, 5};

static const char* kCat = R"({"version":1,"achievements":[
  {"id":"m1","icon":"🥁","name":{"es":"Uno","en":"One"},"desc":{"es":"d","en":"d"},"when":{"fact":"missions","op":">=","value":1}},
  {"id":"m100","name":{"es":"Cien","en":"Hundred"},"when":{"fact":"missions","op":">=","value":100}},
  {"id":"lvl10","name":{"es":"N","en":"L"},"when":{"fact":"level","op":"==","value":10}},
  {"id":"wolf","name":{"es":"W","en":"W"},"when":{"fact":"flags.ragewolf","op":"==","value":1}},
  {"id":"cls3","name":{"es":"C","en":"C"},"when":{"fact":"classes","op":">=","value":3}},
  {"id":"anyc","name":{"es":"A","en":"A"},"when":{"any":[{"fact":"level","op":">=","value":50},{"fact":"missions","op":">=","value":1000}]}},
  {"id":"allc","name":{"es":"B","en":"B"},"when":{"all":[{"fact":"missions","op":">=","value":1},{"fact":"level","op":">=","value":5}]}},
  {"id":"neg","name":{"es":"T","en":"T"},"when":{"fact":"temp","op":"!=","value":0}},
  {"id":"nowhen","name":{"es":"X","en":"X"}},
  {"id":"badfact","name":{"es":"X","en":"X"},"when":{"fact":"nope","op":">=","value":1}},
  {"id":"badop","name":{"es":"X","en":"X"},"when":{"fact":"level","op":"<","value":1}},
  {"id":"badval","name":{"es":"X","en":"X"},"when":{"fact":"level","op":">=","value":"1"}},
  {"id":"nested","name":{"es":"X","en":"X"},"when":{"all":[{"any":[{"fact":"level","op":">=","value":1}]}]}},
  {"id":"emptyall","name":{"es":"X","en":"X"},"when":{"all":[]}}]})";

static void reset_ram() {
    std::fill(g_ram.begin(), g_ram.end(), 0);
    w32(kPtr, kBase);
    w8(kBase, 'E');
    w8(kBase + 0x20, 1);  // level 1 (the plausible minimum)
}

static std::vector<std::string> pending_ids(const PspAchCatalogue& c) {
    std::vector<std::string> v;
    for (const auto& p : c.checks.pending) v.push_back(c.entries[p.index].id);
    std::sort(v.begin(), v.end());
    return v;
}

static std::vector<std::string> run(PspAchCatalogue& c) {
    uint16_t g[64];
    const size_t n = psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 64);
    std::vector<std::string> v;
    for (size_t i = 0; i < n; i++) v.push_back(c.entries[g[i]].id);
    std::sort(v.begin(), v.end());
    return v;
}

static void reads() {
    reset_ram();
    w32(kBase + 0x10, 123456);
    CHECK(psp_ach_read(kFacts[0], g_ram.data(), kBase) == 123456, "u32");
    w32(kBase + 0x30, 0x8);
    CHECK(psp_ach_read(kFacts[2], g_ram.data(), kBase) == 1, "bit 3 set");
    w32(kBase + 0x30, 0x7);
    CHECK(psp_ach_read(kFacts[2], g_ram.data(), kBase) == 0, "bit 3 clear");
    w8(kBase + 0x40, 1); w8(kBase + 0x42, 9); w8(kBase + 0x47, 1);
    CHECK(psp_ach_read(kFacts[3], g_ram.data(), kBase) == 3, "count of non-zero entries");
    w16(kBase + 0x50, 0xFFFE);
    CHECK(psp_ach_read(kFacts[4], g_ram.data(), kBase) == -2, "signed 16");
}

static void compile_rules() {
    PspAchCatalogue c = psp_ach_compile(kCat, kSrc, {"m100"});
    CHECK(c.entries.size() == 14, "every entry listed");
    CHECK(c.entries[0].icon == "🥁" && c.entries[0].name_es == "Uno" && c.entries[0].name_en == "One", "texts");
    CHECK(!c.entries[8].has_when && c.entries[0].has_when, "has_when");
    const std::vector<std::string> want = {"allc", "anyc", "cls3", "lvl10", "m1", "neg", "wolf"};
    CHECK(pending_ids(c) == want, "pending = with when, valid, not earned");
    int left_out = 0;
    for (const auto& l : c.log) left_out += l.find("left out") != std::string::npos;
    CHECK(left_out == 5, "badfact, badop, badval, nested, emptyall each logged");
    CHECK(psp_ach_compile("{", kSrc, {}).checks.pending.empty(), "bad json = nothing");
    CHECK(psp_ach_compile(R"({"version":2,"achievements":[]})", kSrc, {}).log.size() == 1, "other version logged");
    CHECK(c.checks.values.size() == 5, "scratch per fact");
}

static void two_check_rule() {
    reset_ram();
    PspAchCatalogue c = psp_ach_compile(kCat, kSrc, {});
    w32(kBase + 0x10, 1);
    w8(kBase + 0x20, 5);
    CHECK(run(c).empty(), "first check: nothing yet");
    const std::vector<std::string> want = {"allc", "m1"};
    CHECK(run(c) == want, "second consecutive check grants");
    CHECK(run(c).empty(), "granted once");
    w32(kBase + 0x10, 100);
    CHECK(run(c).empty(), "m100 first hold");
    w32(kBase + 0x10, 99);
    CHECK(run(c).empty(), "dropped back: streak reset");
    w32(kBase + 0x10, 100);
    CHECK(run(c).empty(), "hold again: first");
    CHECK(run(c) == std::vector<std::string>{"m100"}, "then granted");
}

static void garbage_never_grants() {
    reset_ram();
    PspAchCatalogue c = psp_ach_compile(kCat, kSrc, {});
    w32(kBase + 0x10, 5);
    CHECK(run(c).empty(), "first hold");
    w32(kPtr, 0);
    CHECK(run(c).empty(), "no struct");
    w32(kPtr, kBase);
    CHECK(run(c).empty(), "streak was reset by the invalid check");
    CHECK(run(c) == std::vector<std::string>{"m1"}, "two valid checks again");
    w32(kPtr, 0x04000000u);
    CHECK(run(c).empty() && run(c).empty(), "outside user RAM");
    w32(kPtr, 0x09FFFFC0u);
    CHECK(run(c).empty() && run(c).empty(), "struct would run past user RAM");
    w32(kPtr, kBase);
    w8(kBase, 'x');
    CHECK(run(c).empty() && run(c).empty(), "sane() false");
    w8(kBase, 'E');
    w8(kBase + 0x20, 200);
    w32(kBase + 0x30, 0x8);
    CHECK(run(c).empty() && run(c).empty(), "level 200 is implausible: the whole check is skipped");
}

static void shrinking_and_max() {
    reset_ram();
    PspAchCatalogue c = psp_ach_compile(kCat, kSrc, {});
    w32(kBase + 0x10, 1000); w8(kBase + 0x20, 50); w32(kBase + 0x30, 0x8);
    w8(kBase + 0x40, 1); w8(kBase + 0x41, 1); w8(kBase + 0x42, 1); w16(kBase + 0x50, 7);
    uint16_t g[64];
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 0, "first hold");
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 2, "at most max per call");
    size_t total = 2;
    for (int i = 0; i < 8; i++) total += psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2);
    CHECK(total == 7 && c.checks.pending.size() == 1, "the rest follow; only lvl10 (level 50 != 10) is left");
    w8(kBase + 0x20, 10);
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 0, "lvl10 first hold");
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 1 && c.checks.pending.empty(), "pending empties");
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 0, "nothing left to do");
}

static void ring() {
    static PspAchRing<2> r;
    PspAchBatch a{}, b{}, c{}, out{};
    a.n = 1; b.n = 2; c.n = 3;
    CHECK(r.push(a) && r.push(b) && !r.push(c), "holds N, then full");
    CHECK(r.pop(&out) && out.n == 1 && r.push(c), "FIFO, room again");
    CHECK(r.pop(&out) && out.n == 2 && r.pop(&out) && out.n == 3 && !r.pop(&out), "order kept, then empty");
}

static void budget() {
    reset_ram();
    std::string cat = R"({"version":1,"achievements":[)";
    for (int i = 0; i < 40; i++) {
        cat += (i ? "," : "") + std::string(R"({"id":"a)") + std::to_string(i) +
               R"(","name":{"es":"a","en":"a"},"when":{"all":[{"fact":"missions","op":">=","value":)" +
               std::to_string(1000 + i) + R"(},{"fact":"classes","op":">=","value":8}]}})";
    }
    cat += "]}";
    PspAchCatalogue c = psp_ach_compile(cat, kSrc, {});
    CHECK(c.checks.pending.size() == 40, "40 pending");
    uint16_t g[64];
    const int iters = 2000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; i++) psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 64);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / iters;
    std::printf("test_achievements: one check with 40 pending = %.2f us\n", us);
    CHECK(us < 20.0, "a check stays under 20 us");
}

int main() {
    reads();
    compile_rules();
    two_check_rule();
    garbage_never_grants();
    shrinking_and_max();
    ring();
    budget();
    if (failures == 0) std::printf("test_achievements: all passed\n");
    return failures == 0 ? 0 : 1;
}
```

  And in `runtime/CMakeLists.txt`, right after the `menu_prompt_tests` block:

```cmake
# Achievement engine (pure): facts, catalogue compile, two-check rule, budget.
add_executable(test_achievements tests/test_achievements.cpp src/psp_achievements.cpp)
target_include_directories(test_achievements PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_achievements PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME achievements_tests COMMAND test_achievements)
```

- [ ] **Step 3: Run it to see it fail.** `cmake --build C:/Users/torso/Documents/deco/build/rtw --target test_achievements`
  → link error (`psp_achievements.cpp` missing). (Configure picks up the new target automatically.)

- [ ] **Step 4: Implement** `runtime/src/psp_achievements.cpp`, and add `src/psp_achievements.cpp` to the runtime
  sources right after `src/psp_menu_prompt.cpp` in `runtime/CMakeLists.txt`:

```cpp
#include "psp_achievements.h"

#include "json.hpp"

#include <algorithm>

namespace {

constexpr uint32_t kUserLo = 0x08800000u, kUserHi = 0x0A000000u;
constexpr size_t kMaxEntries = 255;

uint32_t rd(const uint8_t* rdram, uint32_t addr, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; i++) v |= static_cast<uint32_t>(rdram[(addr + i) & 0x07FFFFFFu]) << (8 * i);
    return v;
}

int fact_index(const PspAchievementSource& src, const std::string& name) {
    for (size_t i = 0; i < src.fact_count; i++)
        if (name == src.facts[i].name) return static_cast<int>(i);
    return -1;
}

bool parse_leaf(const nlohmann::json& j, const PspAchievementSource& src, PspAchLeaf* out, std::string* why) {
    if (!j.is_object() || !j.contains("fact") || !j["fact"].is_string()) { *why = "a leaf without fact"; return false; }
    const std::string name = j["fact"].get<std::string>();
    const int f = fact_index(src, name);
    if (f < 0) { *why = "unknown fact " + name; return false; }
    const std::string op = j.value("op", "");
    if (op == ">=") out->op = PspAchOp::Ge;
    else if (op == "==") out->op = PspAchOp::Eq;
    else if (op == "!=") out->op = PspAchOp::Ne;
    else { *why = "bad op \"" + op + "\""; return false; }
    if (!j.contains("value") || !j["value"].is_number_integer()) { *why = "value is not an integer"; return false; }
    out->fact = static_cast<uint16_t>(f);
    out->value = j["value"].get<int64_t>();
    return true;
}

}  // namespace

int64_t psp_ach_read(const PspFact& f, const uint8_t* rdram, uint32_t base) {
    if (f.count > 0) {
        int64_t n = 0;
        for (uint32_t i = 0; i < f.count; i++) n += rd(rdram, base + f.offset + i * f.size, f.size) != 0;
        return n;
    }
    const uint32_t raw = rd(rdram, base + f.offset, f.size);
    if (f.bit >= 0) return (raw >> f.bit) & 1u;
    if (!f.is_signed) return raw;
    const int shift = 32 - 8 * f.size;
    return static_cast<int32_t>(raw << shift) >> shift;
}

PspAchCatalogue psp_ach_compile(const std::string& json, const PspAchievementSource& src,
                                const std::vector<std::string>& earned) {
    PspAchCatalogue c;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("version", 0) != 1 || !j.contains("achievements") ||
        !j["achievements"].is_array()) {
        c.log.push_back("[ACH] achievements.json is not a version 1 catalogue: achievements off");
        return c;
    }
    for (const auto& a : j["achievements"]) {
        if (c.entries.size() >= kMaxEntries) { c.log.push_back("[ACH] more than 255 achievements: the rest ignored"); break; }
        if (!a.is_object() || !a.contains("id") || !a["id"].is_string()) continue;
        PspAchEntry e;
        e.id = a["id"].get<std::string>();
        e.icon = a.value("icon", "");
        e.hidden = a.value("hidden", false);
        if (a.contains("name") && a["name"].is_object()) {
            e.name_es = a["name"].value("es", "");
            e.name_en = a["name"].value("en", "");
        }
        e.has_when = a.contains("when");
        const uint16_t index = static_cast<uint16_t>(c.entries.size());
        c.entries.push_back(e);
        if (!e.has_when || std::find(earned.begin(), earned.end(), e.id) != earned.end()) continue;
        const nlohmann::json& w = a["when"];
        std::vector<PspAchLeaf> leaves;
        std::string why;
        bool ok = true, any = false;
        if (w.is_object() && (w.contains("all") || w.contains("any"))) {
            any = w.contains("any");
            const nlohmann::json& list = any ? w["any"] : w["all"];
            if (!list.is_array() || list.empty()) { ok = false; why = "empty all/any"; }
            for (size_t i = 0; ok && i < list.size(); i++) {
                const nlohmann::json& l = list[i];
                if (l.is_object() && (l.contains("all") || l.contains("any"))) { ok = false; why = "nested all/any"; break; }
                PspAchLeaf leaf{};
                ok = parse_leaf(l, src, &leaf, &why);
                if (ok) leaves.push_back(leaf);
            }
        } else {
            PspAchLeaf leaf{};
            ok = parse_leaf(w, src, &leaf, &why);
            if (ok) leaves.push_back(leaf);
        }
        if (!ok) { c.log.push_back("[ACH] " + e.id + ": " + why + "; left out"); continue; }
        c.checks.pending.push_back({index, any, 0, static_cast<uint16_t>(c.checks.leaves.size()),
                                    static_cast<uint16_t>(leaves.size())});
        c.checks.leaves.insert(c.checks.leaves.end(), leaves.begin(), leaves.end());
    }
    for (const PspAchLeaf& l : c.checks.leaves)
        if (std::find(c.checks.used.begin(), c.checks.used.end(), l.fact) == c.checks.used.end())
            c.checks.used.push_back(l.fact);
    c.checks.values.assign(src.fact_count, 0);
    return c;
}

size_t psp_ach_evaluate(PspAchChecks& k, const PspAchievementSource& src, const uint8_t* rdram,
                        uint16_t* granted, size_t max) {
    if (k.pending.empty()) return 0;
    const uint32_t base = src.base(rdram);
    bool valid = base >= kUserLo && base < kUserHi && src.size <= kUserHi - base && src.sane(rdram, base);
    for (size_t i = 0; valid && i < k.used.size(); i++) {
        const PspFact& f = src.facts[k.used[i]];
        const int64_t v = psp_ach_read(f, rdram, base);
        if (v < f.min || v > f.max) valid = false;
        k.values[k.used[i]] = v;
    }
    if (!valid) {
        for (PspAchPending& p : k.pending) p.streak = 0;
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < k.pending.size();) {
        PspAchPending& p = k.pending[i];
        bool hold = !p.any;
        for (uint16_t l = p.first; l < p.first + p.n; l++) {
            const PspAchLeaf& leaf = k.leaves[l];
            const int64_t v = k.values[leaf.fact];
            const bool t = leaf.op == PspAchOp::Ge ? v >= leaf.value : leaf.op == PspAchOp::Eq ? v == leaf.value
                                                                                                 : v != leaf.value;
            if (p.any && t) { hold = true; break; }
            if (!p.any && !t) { hold = false; break; }
        }
        if (!hold) { p.streak = 0; i++; continue; }
        if (p.streak < 2) p.streak++;
        if (p.streak < 2 || n == max) { i++; continue; }
        granted[n++] = p.index;
        k.pending[i] = k.pending.back();  // the moved entry has not been looked at in this check
        k.pending.pop_back();
    }
    return n;
}
```

- [ ] **Step 5: Run the test.** `cmake --build C:/Users/torso/Documents/deco/build/rtw --target test_achievements &&
  C:/Users/torso/Documents/deco/build/rtw/test_achievements.exe` → `test_achievements: one check with 40 pending =
  <x> us` (x well under 20) and `test_achievements: all passed`. Then the full gate (build + ctest: 78/78).

- [ ] **Step 6: Commit.**

```bash
cd /c/Users/torso/Documents/deco/psprecomp
git add runtime/include/psp_achievements.h runtime/src/psp_achievements.cpp runtime/tests/test_achievements.cpp runtime/CMakeLists.txt
git -c user.name="Patapon Community" -c user.email=rexgamer1560@gmail.com commit -m "feat(runtime): achievement engine - named facts, compiled checks, two-check rule" -m "Verified: test_achievements (one check with 40 pending = <x> us), ctest 78/78" -m "Agent: <builder>"
```

---

### Task 3: `unlocked.json` store — read, append atomically, never lose entries (pure I/O)

**Files:**
- Create: `runtime/include/psp_ach_store.h`, `runtime/src/psp_ach_store.cpp`, `runtime/tests/test_ach_store.cpp`
- Modify: `runtime/CMakeLists.txt`

**Interfaces:**
- Produces: `struct PspEarned { std::string date, save, hero, release; }`, `enum class PspStoreState { Ok, Missing,
  Corrupt, Newer }`, `std::map<std::string, PspEarned> psp_ach_store_read(const std::string& path, PspStoreState* state)`,
  `bool psp_ach_store_add(const std::string& path, const std::vector<std::pair<std::string, PspEarned>>& add, std::string* err)`,
  `std::string psp_ach_iso_utc(std::time_t t)`, `std::string psp_utf16_to_utf8(const char16_t* s, size_t max)`.

- [ ] **Step 1: Header** `runtime/include/psp_ach_store.h`:

```cpp
#pragma once
// <portable root>/achievements/unlocked.json (docs/contracts/achievements-v1.md): what this PC earned.
// Written only by the game, append-only, temp file + rename; the launcher only reads it.
#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct PspEarned { std::string date, save, hero, release; };

enum class PspStoreState { Ok, Missing, Corrupt, Newer };

// The earned entries (entries that are not objects are skipped). Missing file = Missing; unreadable or
// invalid JSON = Corrupt; a valid file with another "version" = Newer (never written by this build).
std::map<std::string, PspEarned> psp_ach_store_read(const std::string& path, PspStoreState* state);

// Adds the ids the file does not have yet and rewrites it as <path>.tmp + rename. Every entry already
// in the file stays exactly as it is (also ids this build does not know, and keys it does not use).
// Creates the folder. A Corrupt file is first renamed to unlocked.corrupt.json (same folder) so nothing
// is lost; a Newer file is never touched (returns false). Returns false with *err on any failure.
bool psp_ach_store_add(const std::string& path, const std::vector<std::pair<std::string, PspEarned>>& add,
                       std::string* err);

// "2026-10-18T12:00:00Z"
std::string psp_ach_iso_utc(std::time_t t);

// UTF-16 (0-terminated or at most `max` units; surrogate pairs joined, lone ones become U+FFFD) to UTF-8.
std::string psp_utf16_to_utf8(const char16_t* s, size_t max);
```

- [ ] **Step 2: Failing tests** `runtime/tests/test_ach_store.cpp`:

```cpp
// unlocked.json store (psp_ach_store.h): append-only, atomic, nothing ever lost.
#include "psp_ach_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
static void put(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << t;
}
static const PspEarned kE{"2026-10-18T12:00:00Z", "DATA04", "Edson", "dxd-1.1.4"};

static fs::path fresh(const char* name) {
    const fs::path d = fs::temp_directory_path() / "psp_ach_store_test" / name;
    fs::remove_all(d);
    return d;
}

static void missing_then_created() {
    const fs::path f = fresh("a") / "achievements" / "unlocked.json";
    PspStoreState st = PspStoreState::Ok;
    CHECK(psp_ach_store_read(f.string(), &st).empty() && st == PspStoreState::Missing, "missing");
    std::string err;
    CHECK(psp_ach_store_add(f.string(), {{"story_prologue", kE}}, &err), "add creates folder and file");
    const auto m = psp_ach_store_read(f.string(), &st);
    CHECK(st == PspStoreState::Ok && m.size() == 1 && m.at("story_prologue").hero == "Edson" &&
          m.at("story_prologue").release == "dxd-1.1.4" && m.at("story_prologue").save == "DATA04", "read back");
    CHECK(!fs::exists(f.string() + ".tmp"), "no temp file left");
}

static void existing_entries_are_never_lost() {
    const fs::path f = fresh("b") / "unlocked.json";
    put(f, R"({"version":1,"extra":true,"earned":{"gone_id":{"date":"2020-01-01T00:00:00Z","note":"x"},
              "missions_1":{"date":"2026-01-01T00:00:00Z","save":"DATA08","hero":"edd","release":"vanilla-eu"}}})");
    std::string err;
    PspEarned later = kE;
    later.date = "2030-01-01T00:00:00Z";
    CHECK(psp_ach_store_add(f.string(), {{"missions_1", later}, {"vs_1", kE}}, &err), "add");
    PspStoreState st;
    const auto m = psp_ach_store_read(f.string(), &st);
    CHECK(m.size() == 3, "gone_id kept, vs_1 added");
    CHECK(m.at("missions_1").date == "2026-01-01T00:00:00Z" && m.at("missions_1").hero == "edd", "first earn kept");
    const std::string text = slurp(f);
    CHECK(text.find("\"note\"") != std::string::npos && text.find("\"extra\"") != std::string::npos, "unknown keys kept");
}

static void corrupt_is_kept_aside() {
    const fs::path d = fresh("c");
    const fs::path f = d / "unlocked.json";
    put(f, "{");
    PspStoreState st;
    CHECK(psp_ach_store_read(f.string(), &st).empty() && st == PspStoreState::Corrupt, "corrupt");
    std::string err;
    CHECK(psp_ach_store_add(f.string(), {{"vs_1", kE}}, &err), "add over a corrupt file");
    CHECK(slurp(d / "unlocked.corrupt.json") == "{", "the corrupt bytes are kept");
    CHECK(psp_ach_store_read(f.string(), &st).count("vs_1") == 1 && st == PspStoreState::Ok, "new file valid");
    put(f, "");
    CHECK(psp_ach_store_read(f.string(), &st).empty() && st == PspStoreState::Corrupt, "empty = corrupt");
}

static void newer_file_is_never_touched() {
    const fs::path f = fresh("d") / "unlocked.json";
    const std::string v2 = R"({"version":2,"earned":{"a":{}}})";
    put(f, v2);
    PspStoreState st;
    CHECK(psp_ach_store_read(f.string(), &st).empty() && st == PspStoreState::Newer, "newer");
    std::string err;
    CHECK(!psp_ach_store_add(f.string(), {{"vs_1", kE}}, &err) && !err.empty(), "refused");
    CHECK(slurp(f) == v2, "byte-identical");
}

static void text_helpers() {
    CHECK(psp_ach_iso_utc(0) == "1970-01-01T00:00:00Z", "epoch");
    CHECK(psp_ach_iso_utc(1792324800) == "2026-10-18T12:00:00Z", "2026-10-18 12:00 UTC");
    const char16_t edson[] = u"Edson";
    CHECK(psp_utf16_to_utf8(edson, 16) == "Edson", "ascii");
    const char16_t n[] = u"Ñandú";
    CHECK(psp_utf16_to_utf8(n, 16) == "\xC3\x91" "and\xC3\xBA", "latin");
    const char16_t pair[] = {0xD83C, 0xDFC6, 0};
    CHECK(psp_utf16_to_utf8(pair, 16) == "\xF0\x9F\x8F\x86", "surrogate pair (U+1F3C6)");
    const char16_t lone[] = {0xD83C, u'a', 0};
    CHECK(psp_utf16_to_utf8(lone, 16) == "\xEF\xBF\xBD" "a", "lone surrogate");
    const char16_t full[] = u"ABCDEFGHIJKLMNOPQRS";
    CHECK(psp_utf16_to_utf8(full, 16).size() == 16, "max units");
}

int main() {
    missing_then_created();
    existing_entries_are_never_lost();
    corrupt_is_kept_aside();
    newer_file_is_never_touched();
    text_helpers();
    if (failures == 0) std::printf("test_ach_store: all passed\n");
    return failures == 0 ? 0 : 1;
}
```

  CMake (after the `achievements_tests` block):

```cmake
# unlocked.json store: append-only, atomic, corrupt kept aside, newer never touched.
add_executable(test_ach_store tests/test_ach_store.cpp src/psp_ach_store.cpp)
target_include_directories(test_ach_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_ach_store PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ach_store_tests COMMAND test_ach_store)
```

- [ ] **Step 3: Run it to see it fail** (link error: `psp_ach_store_read` undefined).

- [ ] **Step 4: Implement** `runtime/src/psp_ach_store.cpp` and add `src/psp_ach_store.cpp` to the runtime sources
  after `src/psp_achievements.cpp`:

```cpp
#include "psp_ach_store.h"

#include "json.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

bool read_text(const std::string& path, std::string* out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream s;
    s << f.rdbuf();
    *out = s.str();
    return true;
}

// Parses the file; state as psp_ach_store_read documents.
nlohmann::json load(const std::string& path, PspStoreState* state) {
    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) { *state = PspStoreState::Missing; return nlohmann::json(); }
    std::string text;
    if (!read_text(path, &text)) { *state = PspStoreState::Corrupt; return nlohmann::json(); }
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { *state = PspStoreState::Corrupt; return nlohmann::json(); }
    if (!j.contains("version") || !j["version"].is_number_integer()) { *state = PspStoreState::Corrupt; return nlohmann::json(); }
    if (j["version"].get<int64_t>() != 1) { *state = PspStoreState::Newer; return nlohmann::json(); }
    if (!j.contains("earned") || !j["earned"].is_object()) { *state = PspStoreState::Corrupt; return nlohmann::json(); }
    *state = PspStoreState::Ok;
    return j;
}

}  // namespace

std::map<std::string, PspEarned> psp_ach_store_read(const std::string& path, PspStoreState* state) {
    std::map<std::string, PspEarned> m;
    const nlohmann::json j = load(path, state);
    if (*state != PspStoreState::Ok) return m;
    for (auto it = j["earned"].begin(); it != j["earned"].end(); ++it) {
        if (!it.value().is_object()) continue;
        const nlohmann::json& e = it.value();
        m[it.key()] = {e.value("date", ""), e.value("save", ""), e.value("hero", ""), e.value("release", "")};
    }
    return m;
}

bool psp_ach_store_add(const std::string& path, const std::vector<std::pair<std::string, PspEarned>>& add,
                       std::string* err) {
    PspStoreState state;
    nlohmann::json j = load(path, &state);
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    if (state == PspStoreState::Newer) { *err = "unlocked.json has a newer version: left as it is"; return false; }
    if (state == PspStoreState::Corrupt) {
        fs::rename(p, p.parent_path() / "unlocked.corrupt.json", ec);
        if (ec) { *err = "cannot keep the corrupt unlocked.json aside: " + ec.message(); return false; }
    }
    if (state != PspStoreState::Ok) j = {{"version", 1}, {"earned", nlohmann::json::object()}};
    for (const auto& [id, e] : add) {
        if (j["earned"].contains(id)) continue;
        j["earned"][id] = {{"date", e.date}, {"save", e.save}, {"hero", e.hero}, {"release", e.release}};
    }
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = fs::u8path(path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { *err = "cannot write " + tmp.u8string(); return false; }
        f << j.dump(2) << "\n";
        if (!f.flush()) { *err = "write failed: " + tmp.u8string(); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { *err = "rename failed: " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

std::string psp_ach_iso_utc(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string psp_utf16_to_utf8(const char16_t* s, size_t max) {
    std::string out;
    auto put = [&out](uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (size_t i = 0; i < max && s[i]; i++) {
        const uint32_t u = s[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < max && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            put(0x10000 + ((u - 0xD800) << 10) + (s[i + 1] - 0xDC00));
            i++;
        } else if (u >= 0xD800 && u < 0xE000) {
            put(0xFFFD);
        } else {
            put(u);
        }
    }
    return out;
}
```

- [ ] **Step 5: Run the test** → `test_ach_store: all passed`; full gate (ctest 79/79). If `fs::rename` onto an
  existing file fails on this toolchain (Windows), report it with the error; do not change the test.

- [ ] **Step 6: Commit** — subject `feat(runtime): unlocked.json store - append-only, atomic, nothing lost`, body
  `Verified: test_ach_store, ctest 79/79`, `Agent: <builder>` (same `git -c …` form as Task 2, adding the three new
  files and `runtime/CMakeLists.txt`).

---

### Task 4: Settings — `PSPRECOMP_ACHIEVEMENTS_FILE` and `[game] achievement_toasts`

**Files:**
- Modify: `runtime/src/psp_settings.cpp`, `runtime/tests/test_settings.cpp`, `games/patapon3/game_options.json`,
  `docs/contracts/settings-v1.md`, `docs/ENV_FLAGS.md`

**Interfaces:**
- Produces: env `PSPRECOMP_ACHIEVEMENTS_FILE = <ini folder>/achievements/unlocked.json` whenever a `settings.ini` is
  resolved; env `PSPRECOMP_ACHIEVEMENT_TOASTS` = `1` | `0` from `[game] achievement_toasts` (default true).

- [ ] **Step 1: Failing test.** In `runtime/tests/test_settings.cpp` add before `int main()`:

```cpp
static void achievements_keys() {
    const SettingsResolution d = psp_settings_resolve(slurp(kFix + "defaults.ini"), kCatalog,
                                                      "C:/Juegos/Patapon 3", kNoEnv);
    CHECK(val(d, "PSPRECOMP_ACHIEVEMENTS_FILE") == "C:/Juegos/Patapon 3/achievements/unlocked.json",
          "achievements file next to settings.ini");
    CHECK(val(d, "PSPRECOMP_ACHIEVEMENT_TOASTS") == "1", "toasts on by default");
    const SettingsResolution off = psp_settings_resolve("[game]\nachievement_toasts = false\n", kCatalog,
                                                        "C:/P", kNoEnv);
    CHECK(val(off, "PSPRECOMP_ACHIEVEMENT_TOASTS") == "0", "toasts off");
    const EnvGetter env = [](const char* n) -> const char* {
        return std::string(n) == "PSPRECOMP_ACHIEVEMENTS_FILE" ? "D:/x.json" : nullptr;
    };
    CHECK(val(psp_settings_resolve("", kCatalog, "C:/P", env), "PSPRECOMP_ACHIEVEMENTS_FILE") == "<unset>",
          "an environment value wins (nothing emitted)");
}
```

  and call `achievements_keys();` after `controls_and_skin();` in `main`. Run `test_settings` → FAIL
  (`achievements file next to settings.ini`). (`val` returns `"<unset>"` for an env that was not emitted.)

- [ ] **Step 2: Implement.** In `runtime/src/psp_settings.cpp`, right after
  `emit("PSPRECOMP_TEXTURES", resolve_path(base_dir, pack), hd != "true");` add:

```cpp
    // Achievements (docs/contracts/achievements-v1.md): the earned list lives next to settings.ini.
    emit("PSPRECOMP_ACHIEVEMENTS_FILE", resolve_path(base_dir, "achievements/unlocked.json"), false);
```

  In `games/patapon3/game_options.json` append to `options` (after `fast_loading`, keep the file's formatting):

```json
    {
      "section": "game", "key": "achievement_toasts", "type": "bool", "default": "true",
      "tab": "general", "env": "PSPRECOMP_ACHIEVEMENT_TOASTS", "map": {"true": "1", "false": "0"},
      "label": {"es": "Avisos de logros", "en": "Achievement pop-ups"},
      "description": {"es": "Muestra un aviso en el juego al conseguir un logro (se guardan igual).", "en": "Shows a pop-up in game when you earn an achievement (they are recorded either way)."}
    }
```

- [ ] **Step 3: Contract and env docs.** `docs/contracts/settings-v1.md`: in the example ini, after
  `fast_loading = true         ; PSPRECOMP_P3_FAST_LOAD=1|0` add
  `achievement_toasts = true   ; PSPRECOMP_ACHIEVEMENT_TOASTS=1|0`; in the Core keys table, after the
  `display.osk_skin` row add
  `| (always) | PSPRECOMP_ACHIEVEMENTS_FILE | path | <ini folder>/achievements/unlocked.json (docs/contracts/achievements-v1.md) |`.
  `docs/ENV_FLAGS.md`, after the `PSPRECOMP_UI_COLORS` section:

```markdown
### `PSPRECOMP_ACHIEVEMENTS_FILE`
- **Read in:** `runtime/src/psp_achievements_run.cpp` (`psp_achievements_start`).
- **Type:** env (path). Set by the settings reader to `<ini folder>/achievements/unlocked.json`.
- **Default:** unset = achievements off (developer runs without `settings.ini` behave as before).
- **Effect:** where the game appends earned achievements (docs/contracts/achievements-v1.md).

### `PSPRECOMP_ACHIEVEMENT_TOASTS`
- **Read in:** `runtime/src/psp_achievements_run.cpp`.
- **Type:** env, `1` | `0`. From `[game] achievement_toasts` (game_options.json).
- **Default:** on.
- **Effect:** `0` hides the in-game "achievement unlocked" pop-ups; achievements are still recorded.

### `PSPRECOMP_ACHIEVEMENTS`
- **Read in:** `runtime/src/psp_achievements_run.cpp`.
- **Type:** env, `0` = off.
- **Default:** on (when `PSPRECOMP_ACHIEVEMENTS_FILE` is set).
- **Effect:** kill switch: no catalogue, no worker, no vblank hook.
```

- [ ] **Step 4: Gates.** `test_settings: all passed`; ctest 79/79. The launcher reads `game_options.json` from the
  release folder; its Options › General will list "Avisos de logros" after the next build (checked in Task 8).

- [ ] **Step 5: Commit** — subject `feat(settings): achievements file and achievement_toasts option`,
  `Verified: test_settings, ctest 79/79`, `Agent: <builder>`.

---

### Task 5: Toast — texts, timing, worker-side rasterizing, GL draw

**Files:**
- Create: `runtime/include/psp_toast.h`, `runtime/src/psp_toast.cpp`, `runtime/tests/test_toast.cpp`
- Modify: `runtime/include/psp_osk_overlay.h`, `runtime/src/psp_osk_overlay.cpp`, `runtime/src/psp_ge_draw.cpp`,
  `runtime/CMakeLists.txt`

**Interfaces:**
- Produces: `struct ToastText { std::string title, name; }`, `constexpr int64_t kToastMs = 4000`,
  `std::vector<ToastText> psp_toasts_for(const std::vector<std::pair<std::string, std::string>>& icon_name, const char* lang)`,
  `float psp_toast_alpha(int64_t elapsed_ms)`; overlay `void psp_toast_post(const ToastText&)` (any thread except
  the GL one; rasterizes on the caller's thread), `bool psp_toast_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh)`.

- [ ] **Step 1: Header** `runtime/include/psp_toast.h`:

```cpp
#pragma once
// In-game achievement toasts (spec 2026-10-18 part 5b §4.4): the texts for one batch of achievements
// earned in the same check, and the fade timing. Pure.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ToastText { std::string title, name; };

constexpr int64_t kToastMs = 4000;

// One toast per achievement ("Logro desbloqueado", "<icon> <name>"), or, when the batch has more than
// 3, a single summary ("Logros desbloqueados", "🏆 N logros"). lang "en" = English, else Spanish.
std::vector<ToastText> psp_toasts_for(const std::vector<std::pair<std::string, std::string>>& icon_name,
                                      const char* lang);

// Opacity at `elapsed_ms` since the toast appeared: fades in over 250 ms, out over the last 300 ms,
// 0 from kToastMs on.
float psp_toast_alpha(int64_t elapsed_ms);
```

- [ ] **Step 2: Failing tests** `runtime/tests/test_toast.cpp`:

```cpp
// Toast texts and timing (psp_toast.h).
#include "psp_toast.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void one_each_up_to_three() {
    const auto t = psp_toasts_for({{"📜", "El despertar"}, {"🥁", "Primer tambor"}}, "es");
    CHECK(t.size() == 2, "two toasts");
    CHECK(t[0].title == "Logro desbloqueado" && t[0].name == "📜 El despertar", "spanish");
    const auto e = psp_toasts_for({{"", "First Drum"}}, "en");
    CHECK(e.size() == 1 && e[0].title == "Achievement unlocked" && e[0].name == "First Drum", "english, no icon");
    CHECK(psp_toasts_for({{"a", "1"}, {"b", "2"}, {"c", "3"}}, nullptr).size() == 3, "three stay separate");
    CHECK(psp_toasts_for({}, "es").empty(), "nothing");
}

static void summary_over_three() {
    const auto t = psp_toasts_for({{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}}, "es");
    CHECK(t.size() == 1 && t[0].title == "Logros desbloqueados" && t[0].name == "🏆 4 logros", "spanish summary");
    const auto e = psp_toasts_for(std::vector<std::pair<std::string, std::string>>(20, {"x", "y"}), "en");
    CHECK(e.size() == 1 && e[0].title == "Achievements unlocked" && e[0].name == "🏆 20 achievements", "english");
}

static void fade() {
    CHECK(psp_toast_alpha(0) == 0.0f, "starts transparent");
    CHECK(psp_toast_alpha(125) > 0.45f && psp_toast_alpha(125) < 0.55f, "fading in");
    CHECK(psp_toast_alpha(250) == 1.0f && psp_toast_alpha(3700) == 1.0f, "solid");
    CHECK(psp_toast_alpha(3850) > 0.45f && psp_toast_alpha(3850) < 0.55f, "fading out");
    CHECK(psp_toast_alpha(kToastMs) == 0.0f && psp_toast_alpha(99999) == 0.0f, "gone");
}

int main() {
    one_each_up_to_three();
    summary_over_three();
    fade();
    if (failures == 0) std::printf("test_toast: all passed\n");
    return failures == 0 ? 0 : 1;
}
```

  CMake (after `ach_store_tests`):

```cmake
# Achievement toast texts and timing (pure).
add_executable(test_toast tests/test_toast.cpp src/psp_toast.cpp)
target_include_directories(test_toast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_toast PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME toast_tests COMMAND test_toast)
```

  Run → link error.

- [ ] **Step 3: Implement** `runtime/src/psp_toast.cpp` (add `src/psp_toast.cpp` to the runtime sources after
  `src/psp_ach_store.cpp`):

```cpp
#include "psp_toast.h"

#include <cstring>

std::vector<ToastText> psp_toasts_for(const std::vector<std::pair<std::string, std::string>>& icon_name,
                                      const char* lang) {
    const bool en = lang && std::strcmp(lang, "en") == 0;
    std::vector<ToastText> out;
    if (icon_name.size() > 3) {
        const std::string n = std::to_string(icon_name.size());
        out.push_back({en ? "Achievements unlocked" : "Logros desbloqueados",
                       "\xF0\x9F\x8F\x86 " + n + (en ? " achievements" : " logros")});
        return out;
    }
    for (const auto& [icon, name] : icon_name)
        out.push_back({en ? "Achievement unlocked" : "Logro desbloqueado", icon.empty() ? name : icon + " " + name});
    return out;
}

float psp_toast_alpha(int64_t ms) {
    if (ms <= 0 || ms >= kToastMs) return 0.0f;
    if (ms < 250) return static_cast<float>(ms) / 250.0f;
    if (ms > kToastMs - 300) return static_cast<float>(kToastMs - ms) / 300.0f;
    return 1.0f;
}
```

  Run `test_toast` → `all passed`.

- [ ] **Step 4: Thread-safe fonts + emoji fallback in `psp_osk_overlay.cpp`.** The worker (Task 6) rasterizes on
  its own thread while the GL thread may rasterize the keyboard, so the lazy `tried`/`loaded` flags must become
  one-time initialisation:
  1. Add `#include <mutex>` and `#include <deque>` to the includes.
  2. `font()` becomes (same paths and log lines as today, now run once even with two threads):

```cpp
Font& font() {
    static Font f;
    static std::once_flag once;
    std::call_once(once, [] {
        f.tried = true;
        std::vector<std::string> paths;
        if (!skin().font.empty()) paths.push_back(skin().font);
        if (const char* e = std::getenv("PSPRECOMP_OSK_FONT")) paths.push_back(e);
        paths.push_back("C:/Windows/Fonts/segoeui.ttf");
        paths.push_back("C:/Windows/Fonts/arial.ttf");
        paths.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
        paths.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        for (const std::string& p : paths) {
            if (load_font(f, p)) {
                std::fprintf(stderr, "[OSK] font %s\n", p.c_str());
                return;
            }
        }
        std::fprintf(stderr, "[OSK] no font found; set PSPRECOMP_OSK_FONT=<file.ttf>\n");
    });
    return f;
}
```

     Same pattern for `fallback_font()` (its loop inside `std::call_once`, `return;` instead of `return f;`) and for
     `skin()`: drop `static bool loaded` and its two lines, put everything after them in a `std::call_once`
     lambda, each `return s;` inside it becomes `return;`, and `return s;` after the call.
  3. Add, after `fallback_font()`:

```cpp
// Emoji outlines for achievement icons (Segoe UI Emoji keeps monochrome glyf outlines next to its colour
// layers). Silent when absent: the icon is then skipped by the rasterizer like any missing glyph.
Font& emoji_font() {
    static Font f;
    static std::once_flag once;
    std::call_once(once, [] {
        f.tried = true;
        const char* const paths[] = {"C:/Windows/Fonts/seguiemj.ttf",
                                     "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf"};
        for (const char* p : paths) {
            if (load_font(f, p)) return;
        }
    });
    return f;
}
```

  4. In `rasterize()`, next to `const float fb_scale = …;` add
     `Font& em = emoji_font();` and `const float em_scale = em.ok ? stbtt_ScaleForPixelHeight(&em.info, px) : 0.0f;`,
     and replace the glyph-choice loop with (same result as today for every glyph the emoji font does not add):

```cpp
    for (size_t k = 0; k < cps.size(); k++) {
        if (stbtt_FindGlyphIndex(&f.info, cps[k]) != 0) continue;
        if (fb.ok && stbtt_FindGlyphIndex(&fb.info, cps[k]) != 0) { infos[k] = &fb.info; scales[k] = fb_scale; }
        else if (em.ok && stbtt_FindGlyphIndex(&em.info, cps[k]) != 0) { infos[k] = &em.info; scales[k] = em_scale; }
        else if (fb.ok) { infos[k] = &fb.info; scales[k] = fb_scale; }  // as before: the symbol font's notdef
    }
```

- [ ] **Step 5: Overlay post + draw.** In `runtime/include/psp_osk_overlay.h` add `#include "psp_toast.h"` and:

```cpp
// Achievement toasts: psp_toast_post rasterizes on the caller's thread (the achievements worker, never
// the GL thread) and queues the images; psp_toast_overlay_draw uploads each once and draws the current
// one in the top right of the game image for kToastMs, in PSPRECOMP_UI_COLORS. Same arguments and
// return value as psp_osk_overlay_draw; costs one atomic load per frame while no toast is queued.
void psp_toast_post(const ToastText& t);
bool psp_toast_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh);
```

  In `runtime/src/psp_osk_overlay.cpp`, after `psp_menu_prompt_overlay_draw` (outside the anonymous namespace, but
  the helpers it uses are inside it — put the state struct inside the namespace and the two functions after it):

```cpp
namespace {
struct ToastImages { TextImage title, name; };
struct ToastShown { GLuint title = 0, name = 0; int tw = 0, th = 0, nw = 0, nh = 0; int64_t start_ms = -1; };
std::mutex g_toast_mu;
std::deque<ToastImages> g_toast_in;          // posted, not uploaded yet (guarded by g_toast_mu)
std::atomic<int> g_toast_posted{0};          // > 0: g_toast_in has something
std::atomic<int> g_toast_rh{720};            // last game image height, for the raster size

GLuint upload(const TextImage& img) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, img.w > 0 ? img.w : 1, img.h > 0 ? img.h : 1, 0, GL_RED,
                 GL_UNSIGNED_BYTE, img.a.empty() ? nullptr : img.a.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
}  // namespace

void psp_toast_post(const ToastText& t) {
    const int rh = g_toast_rh.load(std::memory_order_relaxed);
    ToastImages im{rasterize(t.title, rh * 0.036f), rasterize(t.name, rh * 0.050f)};
    std::lock_guard<std::mutex> lock(g_toast_mu);
    g_toast_in.push_back(std::move(im));
    g_toast_posted.fetch_add(1, std::memory_order_release);
}

bool psp_toast_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh) {
    static std::deque<ToastShown> shown;
    if (rh > 0) g_toast_rh.store(rh, std::memory_order_relaxed);
    if (g_toast_posted.load(std::memory_order_acquire) > 0) {
        std::deque<ToastImages> in;
        {
            std::lock_guard<std::mutex> lock(g_toast_mu);
            in.swap(g_toast_in);
            g_toast_posted.store(0, std::memory_order_relaxed);
        }
        for (const ToastImages& im : in)
            shown.push_back({upload(im.title), upload(im.name), im.title.w, im.title.h, im.name.w, im.name.h, -1});
    }
    if (shown.empty() || win_w <= 0 || win_h <= 0 || rw <= 0 || rh <= 0) return false;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (shown.front().start_ms < 0) shown.front().start_ms = now;
    if (now - shown.front().start_ms >= kToastMs) {
        glDeleteTextures(1, &shown.front().title);
        glDeleteTextures(1, &shown.front().name);
        shown.pop_front();
        if (shown.empty()) return false;
        shown.front().start_ms = now;
    }
    const ToastShown& s = shown.front();
    const float a = psp_toast_alpha(now - s.start_ms);
    static const PromptColors col = menu_prompt_colors(std::getenv("PSPRECOMP_UI_COLORS"));
    auto faded = [a](const float c[4], float out[4]) { out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[3] * a; };
    float panel[4], accent[4], hint[4], text[4];
    faded(col.panel, panel);
    faded(col.accent, accent);
    faded(col.hint, hint);
    faded(col.text, text);
    const float gx = static_cast<float>(rx), gy = static_cast<float>(win_h - (ry + rh));
    const float pad = rh * 0.022f, margin = rh * 0.03f;
    const float pw = std::max(rw * 0.30f, std::max(static_cast<float>(s.tw), static_cast<float>(s.nw)) + pad * 3.0f);
    const float ph = s.th + s.nh + pad * 2.5f;
    const float px = gx + rw - pw - margin, py = gy + margin;
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
    rect(win_w, win_h, px, py, pw, ph, panel, 0, pad * 0.6f);
    rect(win_w, win_h, px, py, rh * 0.008f, ph, accent, 0);
    rect(win_w, win_h, px + pad * 1.5f, py + pad, static_cast<float>(s.tw), static_cast<float>(s.th), hint, s.title);
    rect(win_w, win_h, px + pad * 1.5f, py + pad * 1.5f + s.th, static_cast<float>(s.nw), static_cast<float>(s.nh),
         text, s.name);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    return true;
}
```

  Add `#include <atomic>` and `#include <chrono>` if missing. Check the real signature of `rect` (its last
  parameter is the corner radius, default 0) and adapt the two calls without a radius if needed; say so.
  In `runtime/src/psp_ge_draw.cpp`, after `const bool prompt_drew = …;` add
  `const bool toast_drew = psp_toast_overlay_draw(win_w, win_h, r.x, r.y, r.w, r.h);` and change the condition to
  `if (osk_drew || prompt_drew || toast_drew) {`.

- [ ] **Step 6: Gates.** Build `build/rtw` without warnings in the touched files; ctest 80/80 (`toast_tests` new).
  Visual check is Task 8's.

- [ ] **Step 7: Commit** — subject `feat(runtime): achievement toast - texts, fade, worker-side text, themed overlay`,
  `Verified: test_toast, ctest 80/80, build/rtw clean`, `Agent: <builder>`.

---

### Task 6: Wiring — vblank hook, worker, start/stop, kill switch

**Files:**
- Create: `runtime/include/psp_vblank_hook.h`, `runtime/src/psp_vblank_hook.cpp`, `runtime/include/psp_achievements_run.h`,
  `runtime/src/psp_achievements_run.cpp`, `runtime/tests/test_achievements_run.cpp`
- Modify: `runtime/src/hle/psp_hle_display.cpp`, `runtime/src/main.cpp`, `runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 engine, Task 3 store, Task 5 `psp_toasts_for` / `psp_toast_post`.
- Produces: `using PspVblankHook = void (*)(uint8_t* rdram); void psp_vblank_hook_set(PspVblankHook); void
  psp_vblank_hook_run(uint8_t* rdram);` and `struct PspAchRunOptions { std::string exe_dir; void
  (*post_toast)(const ToastText&); }`, `void psp_achievements_register(const PspAchievementSource* sources, size_t n)`,
  `bool psp_achievements_start(const PspAchRunOptions&)`, `void psp_achievements_stop()`.

- [ ] **Step 1: Headers.** `runtime/include/psp_vblank_hook.h`:

```cpp
#pragma once
// One optional callback run on the game thread at every vblank wait (sceDisplayWaitVblankStart*),
// after the vblank interrupt handlers. Set once at boot; empty = one relaxed atomic load per vblank.
#include <cstdint>

using PspVblankHook = void (*)(uint8_t* rdram);
void psp_vblank_hook_set(PspVblankHook hook);
void psp_vblank_hook_run(uint8_t* rdram);
```

  `runtime/include/psp_achievements_run.h`:

```cpp
#pragma once
// The achievements system in a running game (spec 2026-10-18 part 5b §2, §5). The game module registers
// its sources (one per release) in register_hooks; main() starts the system after that and stops it at
// shutdown. Off unless PSPRECOMP_ACHIEVEMENTS_FILE is set; PSPRECOMP_ACHIEVEMENTS=0 is the kill switch.
#include "psp_achievements.h"
#include "psp_toast.h"

#include <cstddef>
#include <string>

struct PspAchRunOptions {
    std::string exe_dir;                       // holds achievements.json and release.json
    void (*post_toast)(const ToastText&);      // psp_toast_post in the game; a recorder in tests
};

void psp_achievements_register(const PspAchievementSource* sources, size_t n);
// Reads the catalogue, the release id and unlocked.json, compiles the pending checks, starts the
// worker and installs the vblank hook. Returns false (one [ACH] line) when achievements stay off.
bool psp_achievements_start(const PspAchRunOptions& opt);
// Uninstalls the hook, lets the worker write what is queued, joins it. Safe to call when not started.
void psp_achievements_stop();
```

- [ ] **Step 2: Failing tests** `runtime/tests/test_achievements_run.cpp`:

```cpp
// The running system (psp_achievements_run.h) with a fake source over a 16 MB guest RAM: grants reach
// unlocked.json and the toast poster; off paths install nothing.
#include "psp_achievements_run.h"
#include "psp_ach_store.h"
#include "psp_vblank_hook.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::vector<uint8_t> g_ram(0x01000000, 0);
constexpr uint32_t kPtr = 0x08AA0000u, kBase = 0x08C00000u;
static void w32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) g_ram[(a + i) & 0x07FFFFFFu] = (v >> (8 * i)) & 0xFF; }
static uint32_t r32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(ram[(a + i) & 0x07FFFFFFu]) << (8 * i);
    return v;
}
static const PspFact kFacts[] = {{"missions", 0x10, 4, false, -1, 0, 0, 100000}};
static uint32_t fbase(const uint8_t* ram) { return r32(ram, kPtr); }
static bool fsane(const uint8_t*, uint32_t) { return true; }
static void fhero(const uint8_t*, uint32_t, char16_t out[17]) {
    const char16_t* n = u"Edson";
    for (int i = 0; i < 6; i++) out[i] = n[i];
}
static const PspAchievementSource kSrc[] = {{"test-1", fbase, fsane, fhero, 0x100, kFacts, 1}};

static std::vector<ToastText> g_toasts;
static void record(const ToastText& t) { g_toasts.push_back(t); }

static void setenv_(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static fs::path g_dir;
static void write(const fs::path& p, const std::string& t) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << t; }

static void prepare(int entries, const char* release = "test-1") {
    g_dir = fs::temp_directory_path() / "psp_ach_run_test";
    fs::remove_all(g_dir);
    std::string cat = R"({"version":1,"achievements":[)";
    for (int i = 0; i < entries; i++)
        cat += (i ? "," : "") + std::string(R"({"id":"m)") + std::to_string(i) + R"(","icon":"🥁","name":{"es":"Misión )" +
               std::to_string(i) + R"(","en":"Mission )" + std::to_string(i) +
               R"("},"when":{"fact":"missions","op":">=","value":)" + std::to_string(i + 1) + "}}";
    cat += "]}";
    write(g_dir / "exe" / "achievements.json", cat);
    write(g_dir / "exe" / "release.json", std::string(R"({"id":")") + release + R"("})");
    setenv_("PSPRECOMP_ACHIEVEMENTS_FILE", (g_dir / "root" / "achievements" / "unlocked.json").string().c_str());
    setenv_("PSPRECOMP_ACHIEVEMENTS", "");
    setenv_("PSPRECOMP_ACHIEVEMENT_TOASTS", "1");
    setenv_("PSPRECOMP_UI_LANG", "es");
    setenv_("PSPRECOMP_SAVE_SLOT", "_DATA04");
    std::fill(g_ram.begin(), g_ram.end(), 0);
    w32(kPtr, kBase);
    g_toasts.clear();
}
static PspAchRunOptions opts() { return {(g_dir / "exe").string(), record}; }
static void vblanks(int n) { for (int i = 0; i < n; i++) psp_vblank_hook_run(g_ram.data()); }
static std::map<std::string, PspEarned> earned() {
    PspStoreState st;
    return psp_ach_store_read((g_dir / "root" / "achievements" / "unlocked.json").string(), &st);
}

static void grant_reaches_file_and_toast() {
    prepare(2);
    psp_achievements_register(kSrc, 1);
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 1);
    vblanks(119);
    psp_achievements_stop();
    CHECK(earned().empty(), "one check only after 119 vblanks: nothing yet");
    CHECK(psp_achievements_start(opts()), "starts again");
    vblanks(120);
    psp_achievements_stop();
    const auto e = earned();
    CHECK(e.size() == 1 && e.count("m0"), "m0 earned after two checks");
    CHECK(e.at("m0").release == "test-1" && e.at("m0").save == "DATA04" && e.at("m0").hero == "Edson", "fields");
    CHECK(e.at("m0").date.size() == 20 && e.at("m0").date.back() == 'Z', "ISO date");
    CHECK(g_toasts.size() == 1 && g_toasts[0].title == "Logro desbloqueado" && g_toasts[0].name == "🥁 Misión 0", "toast");
}

static void earned_at_start_is_not_pending() {
    // continues from the previous test's file: m0 is earned
    g_toasts.clear();
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 1);
    vblanks(240);
    psp_achievements_stop();
    CHECK(g_toasts.empty(), "no second toast for m0");
}

static void a_big_batch_is_one_write_and_one_toast() {
    prepare(12);
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 100);
    vblanks(120);
    psp_achievements_stop();
    CHECK(earned().size() == 12, "all 12 recorded");
    CHECK(g_toasts.size() == 1 && g_toasts[0].name == "🏆 12 logros", "one summary toast");
}

static void toasts_off_still_records() {
    prepare(1);
    setenv_("PSPRECOMP_ACHIEVEMENT_TOASTS", "0");
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 5);
    vblanks(120);
    psp_achievements_stop();
    CHECK(earned().size() == 1 && g_toasts.empty(), "recorded, no toast");
}

static void stop_drains_the_ring() {
    prepare(1);
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 5);
    vblanks(120);
    psp_achievements_stop();  // immediately: the worker has not woken yet
    CHECK(earned().size() == 1, "written by stop()");
}

static void off_without_file_or_with_kill_switch() {
    prepare(1);
    setenv_("PSPRECOMP_ACHIEVEMENTS", "0");
    CHECK(!psp_achievements_start(opts()), "kill switch");
    w32(kBase + 0x10, 5);
    vblanks(240);
    CHECK(earned().empty(), "no hook ran");
    prepare(1);
    setenv_("PSPRECOMP_ACHIEVEMENTS_FILE", "");
    CHECK(!psp_achievements_start(opts()), "no settings.ini: off");
    prepare(1, "other-release");
    CHECK(!psp_achievements_start(opts()), "release without a field map: off");
    prepare(1);
    fs::remove(g_dir / "exe" / "achievements.json");
    CHECK(!psp_achievements_start(opts()), "no catalogue: off");
    psp_achievements_stop();  // safe when not started
}

int main() {
    grant_reaches_file_and_toast();
    earned_at_start_is_not_pending();
    a_big_batch_is_one_write_and_one_toast();
    toasts_off_still_records();
    stop_drains_the_ring();
    off_without_file_or_with_kill_switch();
    if (failures == 0) std::printf("test_achievements_run: all passed\n");
    return failures == 0 ? 0 : 1;
}
```

  CMake (after `toast_tests`):

```cmake
# Achievements in a running game: hook, worker, unlocked.json, toasts (fake source, no GL).
add_executable(test_achievements_run tests/test_achievements_run.cpp src/psp_achievements_run.cpp
               src/psp_achievements.cpp src/psp_ach_store.cpp src/psp_toast.cpp src/psp_vblank_hook.cpp)
target_include_directories(test_achievements_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
target_compile_options(test_achievements_run PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME achievements_run_tests COMMAND test_achievements_run)
```

  Run → link error.

- [ ] **Step 3: Implement** `runtime/src/psp_vblank_hook.cpp`:

```cpp
#include "psp_vblank_hook.h"

#include <atomic>

namespace {
std::atomic<PspVblankHook> g_hook{nullptr};
}

void psp_vblank_hook_set(PspVblankHook hook) { g_hook.store(hook, std::memory_order_release); }

void psp_vblank_hook_run(uint8_t* rdram) {
    if (PspVblankHook h = g_hook.load(std::memory_order_relaxed)) h(rdram);
}
```

  and `runtime/src/psp_achievements_run.cpp`:

```cpp
#include "psp_achievements_run.h"

#include "psp_ach_store.h"
#include "psp_vblank_hook.h"

#include "json.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int kCheckEvery = 60;  // vblanks between two checks (~1 s)

const PspAchievementSource* g_sources = nullptr;
size_t g_source_count = 0;

struct Run {
    const PspAchievementSource* src = nullptr;
    PspAchCatalogue cat;
    std::string file, save, lang;
    bool toasts = true;
    void (*post)(const ToastText&) = nullptr;
    PspAchRing<16> ring;   // batches; one per check that granted something
    std::atomic<bool> stop{false};
    std::thread worker;
    int countdown = kCheckEvery;
};
// Published before the hook is installed. Never deleted: at shutdown a game thread may still be inside
// on_vblank after the hook is removed (a few KB left to the process exit).
std::atomic<Run*> g_run{nullptr};

bool slurp(const std::string& path, std::string* out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream s;
    s << f.rdbuf();
    *out = s.str();
    return true;
}

// Game thread, every vblank: one decrement and one compare on 59 of 60.
void on_vblank(uint8_t* rdram) {
    Run* r = g_run.load(std::memory_order_acquire);
    if (!r || --r->countdown > 0) return;
    r->countdown = kCheckEvery;
    PspAchBatch b{};
    b.n = static_cast<uint16_t>(psp_ach_evaluate(r->cat.checks, *r->src, rdram, b.index, kAchBatchMax));
    if (b.n == 0) return;
    r->src->hero(rdram, r->src->base(rdram), b.hero);
    b.hero[16] = 0;
    if (!r->ring.push(b)) std::fprintf(stderr, "[ACH] queue full: %u achievements wait for the next session\n", b.n);
}

// Worker: everything earned since the last pass, in one file write and one batch of toasts.
void drain(Run& r) {
    std::vector<std::pair<std::string, PspEarned>> add;
    std::vector<std::pair<std::string, std::string>> icon_name;
    PspAchBatch b;
    const std::string date = psp_ach_iso_utc(std::time(nullptr));
    while (r.ring.pop(&b)) {
        const std::string hero = psp_utf16_to_utf8(b.hero, 16);
        for (uint16_t i = 0; i < b.n; i++) {
            const PspAchEntry& e = r.cat.entries[b.index[i]];
            add.push_back({e.id, {date, r.save, hero, r.src->release}});
            icon_name.push_back({e.icon, r.lang == "en" ? e.name_en : e.name_es});
        }
    }
    if (add.empty()) return;
    std::string err;
    if (!psp_ach_store_add(r.file, add, &err)) std::fprintf(stderr, "[ACH] %s\n", err.c_str());
    for (const auto& a : add) std::fprintf(stderr, "[ACH] earned %s\n", a.first.c_str());
    if (r.toasts && r.post)
        for (const ToastText& t : psp_toasts_for(icon_name, r.lang.c_str())) r.post(t);
}

void worker_main(Run* r) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    while (!r->stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        drain(*r);
    }
    drain(*r);
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

}  // namespace

void psp_achievements_register(const PspAchievementSource* sources, size_t n) {
    g_sources = sources;
    g_source_count = n;
}

bool psp_achievements_start(const PspAchRunOptions& opt) {
    if (env("PSPRECOMP_ACHIEVEMENTS") == "0") { std::fprintf(stderr, "[ACH] off (PSPRECOMP_ACHIEVEMENTS=0)\n"); return false; }
    const std::string file = env("PSPRECOMP_ACHIEVEMENTS_FILE");
    if (file.empty()) { std::fprintf(stderr, "[ACH] off: no settings.ini (PSPRECOMP_ACHIEVEMENTS_FILE unset)\n"); return false; }
    std::string text;
    if (!slurp(opt.exe_dir + "/release.json", &text)) { std::fprintf(stderr, "[ACH] off: no release.json\n"); return false; }
    const nlohmann::json rel = nlohmann::json::parse(text, nullptr, false);
    const std::string release = rel.is_object() ? rel.value("id", "") : "";
    const PspAchievementSource* src = nullptr;
    for (size_t i = 0; i < g_source_count; i++)
        if (release == g_sources[i].release) src = &g_sources[i];
    if (!src) { std::fprintf(stderr, "[ACH] off: no field map for release \"%s\"\n", release.c_str()); return false; }
    if (!slurp(opt.exe_dir + "/achievements.json", &text)) { std::fprintf(stderr, "[ACH] off: no achievements.json\n"); return false; }
    PspStoreState st;
    std::vector<std::string> earned;
    for (const auto& kv : psp_ach_store_read(file, &st)) earned.push_back(kv.first);
    if (st == PspStoreState::Corrupt) std::fprintf(stderr, "[ACH] unlocked.json is corrupt: it is kept aside on the next write\n");
    if (st == PspStoreState::Newer) { std::fprintf(stderr, "[ACH] off: unlocked.json is from a newer build\n"); return false; }
    Run* r = new Run;
    r->src = src;
    r->cat = psp_ach_compile(text, *src, earned);
    for (const std::string& l : r->cat.log) std::fprintf(stderr, "%s\n", l.c_str());
    r->file = file;
    const std::string slot = env("PSPRECOMP_SAVE_SLOT");
    r->save = slot.size() > 1 && slot[0] == '_' ? slot.substr(1) : slot;
    r->lang = env("PSPRECOMP_UI_LANG") == "en" ? "en" : "es";
    r->toasts = env("PSPRECOMP_ACHIEVEMENT_TOASTS") != "0";
    r->post = opt.post_toast;
    std::fprintf(stderr, "[ACH] %s: %zu achievements, %zu earned, %zu checks pending\n", release.c_str(),
                 r->cat.entries.size(), earned.size(), r->cat.checks.pending.size());
    r->worker = std::thread(worker_main, r);
    g_run.store(r, std::memory_order_release);
    psp_vblank_hook_set(on_vblank);
    return true;
}

void psp_achievements_stop() {
    psp_vblank_hook_set(nullptr);
    Run* r = g_run.exchange(nullptr, std::memory_order_acq_rel);
    if (!r) return;
    r->stop.store(true, std::memory_order_release);
    r->worker.join();  // its last drain() writes what is queued
}
```

  Note on `g_run`: the hook is removed first and the Run is never freed, so a game thread still inside
  `on_vblank` at shutdown touches valid memory; what it pushes after the join is re-detected next session.

- [ ] **Step 4: Hook it into the runtime.** Add `src/psp_vblank_hook.cpp` and `src/psp_achievements_run.cpp` to the
  runtime sources (after `src/psp_toast.cpp`). In `runtime/src/hle/psp_hle_display.cpp` add
  `#include "psp_vblank_hook.h"` and, in `hle_sceDisplayWaitVblankStart`, right after
  `psp_intr_dispatch_vblank(rdram, ctx);`:

```cpp
    psp_vblank_hook_run(rdram);  // achievements check (psp_achievements_run.cpp), when enabled
```

  In `runtime/src/main.cpp`: `#include "psp_achievements_run.h"` and `#include "psp_osk_overlay.h"` (if not
  included); right after `game->register_hooks(rdram);`:

```cpp
    {
        // Achievements (spec 2026-10-18 part 5b): off without settings.ini or with PSPRECOMP_ACHIEVEMENTS=0.
        char* base = SDL_GetBasePath();
        psp_achievements_start({base ? base : "", psp_toast_post});
        SDL_free(base);
    }
```

  and, in the shutdown sequence, right before `psp_audio_out_shutdown();` (the line after `stage("host writes idle");`):

```cpp
    psp_achievements_stop();  // writes what is still queued
```

  `SDL_GetBasePath()` ends with a separator; `exe_dir + "/release.json"` with a doubled separator is fine on
  Windows and Linux. If `SDL_GetBasePath` needs `SDL_Init` first in this file's order, check how the settings
  block at the top of `main` uses it (it already does before SDL init) and do the same.

- [ ] **Step 5: Gates.** `test_achievements_run: all passed`; full build clean; ctest 81/81. Also run the game once
  without `settings.ini` the way `jugar.bat` does is NOT needed here — Task 8 checks the logs.

- [ ] **Step 6: Commit** — subject `feat(runtime): achievements run once a second from the vblank hook`,
  `Verified: test_achievements_run, ctest 81/81`, `Agent: <builder>`.

---

### Task 7: Patapon 3 field map, final catalogue

**Files:**
- Create: `games/patapon3/runtime/p3_achievements.h`, `games/patapon3/runtime/p3_achievements.cpp`,
  `runtime/tests/test_p3_achievements.cpp`
- Modify: `games/patapon3/runtime/hooks_main.cpp`, `games/patapon3/achievements.json`,
  `docs/contracts/achievements-v1.md`, `runtime/CMakeLists.txt`; launcher `src-tauri/src/achievements.rs`
  (and, only if Task 1 dropped `story_prologue`, `themes/arena/theme.json`, `scripts/themes.test.mjs`, `README.md`)

**Interfaces:**
- Consumes: `docs/p3_achievement_facts.md` (Task 1: chains, fact table, keep/drop decisions, DATA04/DATA08 earned
  lists); Task 2 `PspFact`, `PspAchievementSource`; Task 6 `psp_achievements_register`.
- Produces: `const PspAchievementSource* p3_achievement_sources(size_t* n)`.

- [ ] **Step 1: Header** `games/patapon3/runtime/p3_achievements.h`:

```cpp
#pragma once
// Patapon 3 progress struct for achievements (docs/p3_achievement_facts.md): the pointer chain per
// release, the plausibility check and the named facts. Pure: no runtime state.
#include "psp_achievements.h"

#include <cstddef>

const PspAchievementSource* p3_achievement_sources(size_t* n);
```

- [ ] **Step 2: Implementation** `games/patapon3/runtime/p3_achievements.cpp` — fixed parts below; the `kChain*`
  constants, the `kFacts` rows and the `kSources` rows are copied from `docs/p3_achievement_facts.md` (one
  `follow<global, offsets…>` per release the doc lists as found; the example is the RichPresence chain):

```cpp
#include "p3_achievements.h"

namespace {

constexpr uint32_t kUserLo = 0x08800000u, kUserHi = 0x0A000000u;
constexpr uint32_t kStructSize = 178848;      // the decrypted SECURE.BIN size == the struct
constexpr uint32_t kHeroName = 0xF1C8;        // UTF-16, up to 16 units
constexpr uint32_t kHeroClass = 0x9520;       // 1..34 (PataponRichPressence class list)

uint32_t rd32(const uint8_t* rdram, uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(rdram[(a + i) & 0x07FFFFFFu]) << (8 * i);
    return v;
}

// base = rd32(... rd32(rd32(Global) + O1) ... + On); 0 when a link leaves user RAM.
template <uint32_t Global, uint32_t... Offs>
uint32_t follow(const uint8_t* rdram) {
    uint32_t p = rd32(rdram, Global);
    for (uint32_t o : {Offs...}) {
        if (p < kUserLo || p >= kUserHi) return 0;
        p = rd32(rdram, p + o);
    }
    return p;
}

// Hero name printable UTF-16 (1..16 units, then 0) and class 1..34.
bool sane(const uint8_t* rdram, uint32_t base) {
    const uint32_t cls = rd32(rdram, base + kHeroClass);
    if (cls < 1 || cls > 34) return false;
    int len = 0;
    for (; len < 16; len++) {
        const uint32_t a = base + kHeroName + 2 * len;
        const uint16_t u = static_cast<uint16_t>(rdram[a & 0x07FFFFFFu] | (rdram[(a + 1) & 0x07FFFFFFu] << 8));
        if (u == 0) break;
        if (u < 0x20) return false;
    }
    return len >= 1 && len < 16;
}

void hero(const uint8_t* rdram, uint32_t base, char16_t out[17]) {
    for (int i = 0; i < 16; i++) {
        const uint32_t a = base + kHeroName + 2 * i;
        out[i] = static_cast<char16_t>(rdram[a & 0x07FFFFFFu] | (rdram[(a + 1) & 0x07FFFFFFu] << 8));
        if (!out[i]) break;
    }
    out[16] = 0;
}

// Facts (docs/p3_achievement_facts.md): {name, offset, size, signed, bit, count, min, max}.
const PspFact kFacts[] = {
    // copied row by row from the doc's table, e.g.
    // {"hero.level", 0x????, 1, false, -1, 0, 1, 99},
};

const PspAchievementSource kSources[] = {
    {"dxd-1.1.4", follow<0x08AABD94u, 0x50u>, sane, hero, kStructSize, kFacts, sizeof kFacts / sizeof kFacts[0]},
    // vanilla-eu / dxd-1.1.5b rows only when the doc lists their chain as found
};

}  // namespace

const PspAchievementSource* p3_achievement_sources(size_t* n) {
    *n = sizeof kSources / sizeof kSources[0];
    return kSources;
}
```

  The commented example rows are replaced by the doc's real rows; no row may be left commented or guessed.

- [ ] **Step 3: Register.** In `games/patapon3/runtime/hooks_main.cpp` add `#include "p3_achievements.h"` and
  `#include "psp_achievements_run.h"`, and as the first lines of `patapon3_register_hooks`:

```cpp
    size_t ach_sources = 0;
    const PspAchievementSource* ach = p3_achievement_sources(&ach_sources);
    psp_achievements_register(ach, ach_sources);  // main() starts the system (psp_achievements_run.h)
```

- [ ] **Step 4: Final catalogue.** In `games/patapon3/achievements.json`: remove `"draft": true`; for every `keep`
  id add its `when` from the doc (same line layout, `"when": {...}` appended after `"desc"`); delete every `drop`
  entry. Keep the remaining order. In `docs/contracts/achievements-v1.md` replace the `draft: true` paragraph with:

```markdown
- Every entry has a `when` verified against the user's saves and the game (`docs/p3_achievement_facts.md`);
  the game evaluates them (psp_achievements). An id published with a `when` never changes; a dropped id is
  never reused.
```

- [ ] **Step 5: Failing test** `runtime/tests/test_p3_achievements.cpp` — the expected lists are the doc's
  "DATA04 earns" / "DATA08 earns" lists, copied verbatim into `kWant04` / `kWant08`:

```cpp
// Patapon 3 field map + the official catalogue over the user's decrypted saves (skipped when absent;
// the saves are never committed): DATA04 earns exactly the documented list, DATA08 its own.
#include "p3_achievements.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static const std::vector<std::string> kWant04 = {/* the doc's DATA04 list, sorted */};
static const std::vector<std::string> kWant08 = {/* the doc's DATA08 list, sorted */};

// Puts the save at a heap address and the dxd-1.1.4 chain (from the doc) in front of it.
static std::vector<std::string> earned_by(const std::string& save, const PspAchievementSource& src) {
    std::vector<uint8_t> ram(0x01000000, 0);
    const uint32_t base = 0x08C00000u;
    const std::string bytes = slurp(save);
    std::copy(bytes.begin(), bytes.end(), ram.begin() + (base & 0x07FFFFFFu));
    auto w32 = [&ram](uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) ram[(a + i) & 0x07FFFFFFu] = (v >> (8 * i)) & 0xFF; };
    // Write the chain so that src.base(ram) == base (adapt to the doc's chain: global -> object, object + off -> base).
    const uint32_t object = 0x08B00000u;
    w32(0x08AABD94u, object);
    w32(object + 0x50u, base);
    CHECK(src.base(ram.data()) == base, "chain leads to the struct");
    CHECK(src.sane(ram.data(), base), "the save passes the sanity check");
    PspAchCatalogue c = psp_ach_compile(slurp(PSP_P3_CATALOGUE), src, {});
    CHECK(c.log.empty(), "every catalogue entry compiles");
    uint16_t g[256];
    std::vector<std::string> ids;
    for (int i = 0; i < 2; i++) {
        const size_t n = psp_ach_evaluate(c.checks, src, ram.data(), g, 256);
        for (size_t k = 0; k < n; k++) ids.push_back(c.entries[g[k]].id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

int main() {
    size_t n = 0;
    const PspAchievementSource* s = p3_achievement_sources(&n);
    CHECK(n >= 1 && std::string(s[0].release) == "dxd-1.1.4", "dxd-1.1.4 has a field map");
    std::vector<uint8_t> zero(0x01000000, 0);
    CHECK(s[0].base(zero.data()) == 0, "no struct in empty RAM");
    const std::string dir = PSP_P3_SAVES_DIR;
    if (std::ifstream(dir + "/plain_DATA04.bin").good() && std::ifstream(dir + "/plain_DATA08.bin").good()) {
        CHECK(earned_by(dir + "/plain_DATA04.bin", s[0]) == kWant04, "DATA04 earns the documented list");
        CHECK(earned_by(dir + "/plain_DATA08.bin", s[0]) == kWant08, "DATA08 earns the documented list");
    } else {
        std::printf("test_p3_achievements: user saves not on this PC, save checks skipped\n");
    }
    if (failures == 0) std::printf("test_p3_achievements: all passed\n");
    return failures == 0 ? 0 : 1;
}
```

  If the doc's chain for dxd-1.1.4 is not the RichPresence one, write the chain into `ram` the same way the doc
  describes it (the two `w32` lines) — that is the only adaptation allowed. CMake, guarded by the game:

```cmake
# Patapon 3 field map + official catalogue over the user's decrypted saves (skipped when absent).
if(PSPRECOMP_GAME STREQUAL "patapon3")
    add_executable(test_p3_achievements tests/test_p3_achievements.cpp src/psp_achievements.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime/p3_achievements.cpp)
    target_include_directories(test_p3_achievements PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
                               ${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/runtime)
    target_compile_definitions(test_p3_achievements PRIVATE
        PSP_P3_CATALOGUE="${CMAKE_CURRENT_SOURCE_DIR}/../games/patapon3/achievements.json"
        PSP_P3_SAVES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../build/save_main")
    target_compile_options(test_p3_achievements PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME p3_achievements_tests COMMAND test_p3_achievements)
endif()
```

  Build it right after writing it with Steps 1–2 still empty: it does not link (`p3_achievement_sources`) —
  that is the failing state. With Steps 2–4 done (rows from the doc, final catalogue) it must PASS; a `when` that
  names a fact missing from `kFacts` shows up as `every catalogue entry compiles` failing.

- [ ] **Step 6: Launcher catalogue test.** In `launcher/src-tauri/src/achievements.rs` test
  `the_official_catalogue_is_valid`, change `assert!(n >= 20, "{n}");` to `assert!(n >= <kept count from the doc>, "{n}");`
  (the exact number of kept entries). Only if `story_prologue` was dropped: set `themes/arena/theme.json` `unlock` to
  the doc's replacement id, the same id in `scripts/themes.test.mjs` (`.unlock, "story_prologue"`) and in the
  `the_official_catalogue_is_valid` `any(|a| a.id == "story_prologue")` line, and the achievement name in
  `README.md` («El despertar» / "The Awakening").

- [ ] **Step 7: Gates.** psprecomp: build clean, ctest 82/82 (`p3_achievements_tests` with the save checks run, not
  skipped, on this PC). Launcher: `cd /c/Users/torso/Documents/deco/launcher/src-tauri && cargo test -q 2>&1 | grep -E "^test result|FAILED|panicked"`
  (102 passed + 2 ignored) and `cd .. && node --test scripts/*.test.mjs 2>&1 | tail -8` (fail 0).

- [ ] **Step 8: Commit (two repos)** — psprecomp subject `feat(p3): achievements field map and the verified official catalogue`
  (`Verified: test_p3_achievements with the user's DATA04/DATA08, ctest 82/82`); launcher subject
  `test(launcher): the catalogue test follows the verified catalogue` (`Verified: cargo test 102 passed`); both
  `Agent: <builder>`.

---

### Task 8: End to end, performance gate, Findings (Claude)

- [ ] **Step 1: Builds.** `cmake --build` for `build/rtw`, `build/v115b/rtw`, `build/vanilla/rtw` (each copies
  `achievements.json`); ctest in `build/rtw`. Launcher `npm run tauri build -- --no-bundle`, make-portable into
  `build/e2e_portable3` and `build/e2e_portable` (the three `--game` pairs of Plan 1's Findings). **Back up the user's
  `e2e_portable3/settings.ini` first** (they use that folder) and restore it at the end; keep a copy of any
  `e2e_portable3/achievements/unlocked.json` the user has and restore it too.
- [ ] **Step 2: Regression + performance.** `rt_regress.py` (baseline `build/tour/relevo_base` vs a new tour on 9998,
  commands in its docstring) → PASS. `perf_hideout.py` two samples with the baseline exe saved in Task 2 Step 0
  (`P3DRIVE_EXE=psprecomp_runtime_pre_ach.exe`) and two with the new exe, `--diff`: fps, ge_ms/frame and CPU
  within noise, with `PSPRECOMP_ACHIEVEMENTS_FILE` set (achievements running, everything pending). Plus one sample
  started while a toast is on screen (write a fresh `unlocked.json` without `story_prologue` so it is granted on
  arrival): frames within the 60 fps budget.
- [ ] **Step 3: E2E (port 9998, launcher CDP like Plan 1).** From `e2e_portable3` with an empty
  `achievements/unlocked.json`: Partidas → choose DATA04 → Iniciar DxD 1.1.4 → `e2e3.py hideout`. Within ~2 s of the
  hideout: one grouped toast "🏆 N logros" (N = the doc's DATA04 count) — capture it windowed (F11, then PIL
  `ImageGrab.grab(bbox=<window rect>)`; the debug-socket shot has no overlays). `unlocked.json` holds exactly the
  DATA04 list, release `dxd-1.1.4`, save `DATA04`, hero `Edson`. `logs/game.log`: `[ACH] dxd-1.1.4: …` and one
  `[ACH] earned` per id. F1 → Sí → the launcher's Logros shows "N / total", dates, Edson; Arena is unlocked in Temas.
  Restart the game: no toast (nothing pending is met). DATA08: only the DATA08 list. Toasts off (Options › General ›
  Avisos de logros = No): recorded, no toast. Without `settings.ini` (run `build/rtw/psprecomp_runtime.exe` directly on
  9998 like `jugar.bat`): `[ACH] off: no settings.ini` and nothing written. Vanilla / 1.1.5b: their toast if the doc
  has their chain, else `[ACH] off: no field map …`.
- [ ] **Step 4: Findings** at the end of this plan (commits, gates, perf table, E2E table, plan defects, what is left
  for the user), commit `docs(plan): launcher part 5b plan 2 findings`; restore the user's `settings.ini` and
  `unlocked.json` in `e2e_portable3`; ask the user to play with their save and approve; push launcher + psprecomp only
  on their OK (psprecomp by cherry-pick onto `p3/main` in the `push-p3-6` worktree, never f639180).
