# Partner Ping Strip Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Partner pings in missions and VS drawn as a fixed strip (mission: top centre; VS: under the minimap) instead of under the party masks, so a dead hero, a reversed mission or the VS HUD no longer hides them.

**Architecture:** The pure half (`p3_ping.cpp`) gains the strip layout and hero-name/number reading; the GE observer (`p3_ping_run.cpp`) keeps using the mask outline (and a new VS HUD prim) only to know *which screen* is on and *when* in the draw order to draw, never to map slots. The hideout badge path is untouched.

**Tech Stack:** C++17 runtime, GE observer seam (`psp_ge_observer.h`), `psp_ge_draw_tag` / `WorldTag`, CTest.

**Spec:** `docs/superpowers/specs/2026-11-07-ping-strip-design.md`

## Global Constraints

- Branch `ping-strip` in `psprecomp/` from `return-handoff` HEAD (u20 build). Local only: never push without the user's OK.
- Lab only: ports 9998+ (never 9999, the user's game). Mission recon/tests ONLY in "Shooshookle, la insaciable" (`build/infra/shooshookle.sh`) — NEVER open the iron door. VS: `build/infra/vs.sh`. Run `build/infra/diskquiet.py` first.
- Colours, icon frames, thresholds unchanged: green < 80, yellow < 150, red; icon frame = bars (+4 via server); unknown = grey icon, no number.
- Mission entry text `N.Name` (name cut to 6 chars) + icon + ms digits, no "ms"; VS entry `N` + icon + digits. Own hero never listed. Sorted by N.
- Mission zone x 125..300, y ~12; VS zone x 145..365, y ~51 (PSP px) — confirmed or corrected in Task 1, then fixed as constants.
- Non-ASCII name characters become `?` (tag font is ASCII).
- Author "Patapon Community" (repo-local config), NO Claude Co-Authored-By trailer (user rule).
- Evidence before "done": `ninja` + `ctest` + `rt_regress.py` + lab screenshots.

## Review Focus

1. Own hero dead in a mission (own mask gone, others drawn) -> strip still shows every partner. `p3_ping_strip` takes no mask count by design (Task 2); lab check in Task 4 Step 5 (kill the host's own hero too).
2. A partner leaves mid-mission (name cleared, stale online id kept) -> that entry disappears, the rest keep their cells. Test `strip_skips_left_player` in Task 2.
3. VS opponents the server does not report a ping for -> grey icon with the number, never a missing entry. Test `strip_unknown_ping_grey` in Task 2.
4. Base table not found yet on entering a mission straight from a fresh boot (no hideout first) -> scan by own id with `anchors = 0`; Task 3 test `pick_base_without_anchors`.
5. Strip entries colliding with each other and getting pushed down a line by the tag unstacker -> equal cells sized so 3 (mission) / 7 (VS) entries fit; Task 2 test `cells_do_not_overlap`.

---

### Task 1: Recon — player number, VS table, VS HUD anchor, zones

No code commit; writes `build/ping_recon/STRIP.md`.

**Files:**
- Create: `build/ping_recon/STRIP.md`
- Reuse: `build/tools/ping_recon/badges.py`, `masks.py`, `findtab.py`, `name_copies.py`, `wincap.py`, `build/infra/two.py`, `vs.sh`

- [ ] **Step 1:** Mission, 3 instances (two.py `mk3`, Shooshookle): confirm the label number "N." == party slot + 1 for every hero on every screen (compare `p3_ping_read_slots` order with the labels in `wincap.py` shots). Record the rule.
- [ ] **Step 2:** Same mission: confirm the mask outline (`P3Prim::Mask`) is still drawn for the living heroes after one hero dies (kill a visitor), and note whether the remaining masks shift left. Only "at least one Mask prim per mission frame" matters now.
- [ ] **Step 3:** VS with the most instances the lab runs reliably (`vs.sh`, 2v2 if possible): find where the players' online ids and hero names live (`findtab.py` / `name_copies.py` with each id). Record: same table as missions (base, stride 0x118 / names 0x23C, 0x1128 below)? slot count? label number rule? If the VS slots exceed the name table (0x1128 / 0x23C = 7.8 slots), find the real table.
- [ ] **Step 4:** VS: pick one HUD PRIM drawn every VS frame and nowhere else (the minimap frame or the score box) with `masks.py`-style dump: record tex_addr, clut_addr, tex_fmt, vtype, count, bbox. That is `P3Prim::VsHud`.
- [ ] **Step 5:** With `wincap.py`, measure in PSP px the free zones: mission gap (last mask's right edge -> first loot icon) and VS gap (FEVER worm right edge -> score box; minimap's lowest point incl. the yellow selection frame). Write the final `kMissionZone` / `kVsZone` {x0, x1, y}.
- [ ] **Step 6:** Check whether `infra::Client::peer_ping(id)` returns a ping for VS opponents (other party) or only for own party. Record it; nothing to change if unknown -> grey.

Decision gate: if Step 3 finds no readable VS player table, stop VS (Task 4 Step 4-5 skipped), write it in STRIP.md and tell the user ("VS sin ping").

---

### Task 2: Pure strip layout

**Files:**
- Modify: `games/patapon3/runtime/p3_ping.h`, `games/patapon3/runtime/p3_ping.cpp`
- Test: `runtime/tests/test_p3_ping.cpp`

**Interfaces:**
- Produces:
  - `struct P3Partner { int number; std::string name; int ms; bool via_server; };` (number = slot + 1 per Task 1; name already cut/ASCII)
  - `struct P3Zone { float x0, x1, y; };` and `extern const P3Zone kMissionZone, kVsZone;` (Task 1 values)
  - `std::vector<WorldTag> p3_ping_strip(const std::vector<P3Partner>& partners, const P3Zone& zone, bool names);` — sorted by number, one tag per partner, equal cells `(x1-x0)/partners.size()`, tag x = cell centre, y = zone.y, size_px 7.5, icon = bars (+4 via server), colour as today, text `"N.Name 85"` (names) or `"N 85"`; unknown ms -> text without the digits, grey, icon 0 (+4 via server if set).
  - `std::string p3_ping_short_name(const char16_t* utf16, size_t max_chars = 6);` — stops at NUL, non-ASCII -> `?`.

- [ ] **Step 1: Write failing tests** in `test_p3_ping.cpp`:

```cpp
const P3Zone z{125, 300, 12};
auto s = p3_ping_strip({{4, "Fuego", 197, false}, {2, "Blue", 10, false}, {3, "Krish2", 59, true}}, z, true);
CHECK(s.size() == 3 && s[0].text == "2.Blue 10" && s[1].text == "3.Krish2 59" && s[2].text == "4.Fuego 197", "sorted_by_number");
CHECK(std::fabs(s[0].x - (125 + 175.0f / 6)) < 0.01f && std::fabs(s[2].x - (300 - 175.0f / 6)) < 0.01f && s[0].y == 12, "equal_cells");
CHECK(s[0].icon == 3 && s[1].icon == 2 + 4 && s[2].icon == 1, "icons");
CHECK(p3_ping_strip({{2, "Blue", 85, false}}, kVsZone, false)[0].text == "2 85", "vs_number_only");
auto u = p3_ping_strip({{5, "Max", -1, false}}, kVsZone, false)[0];
CHECK(u.text == "5" && u.icon == 0 && u.r == 150, "strip_unknown_ping_grey");
CHECK(p3_ping_strip({}, z, true).empty(), "no_partners_no_tags");
CHECK(p3_ping_short_name(u"Krish2long") == "Krish2" && p3_ping_short_name(u"Ñandú") == "?and?", "short_name");
```

  Plus `cells_do_not_overlap`: for 3 partners in `kMissionZone` and 7 in `kVsZone`, cell width >= the widest expected entry (mission `"4.Krish2 197"` / VS `"8 197"`) at 7.5 px using the tag font's advance (read it from `psp_osk_overlay` / the world-tag code; if it is not exposed, assert cell >= 56 px mission / 30 px VS and note the source of those numbers in a comment).

- [ ] **Step 2:** `cmake --build runtime/build --target test_p3_ping && ctest --test-dir runtime/build -R p3_ping` -> FAIL (undefined `p3_ping_strip`).
- [ ] **Step 3:** Implement the two functions in `p3_ping.cpp`; reuse `p3_ping_bars` and move the colour choice out of `p3_ping_tag` into a shared helper so both use it.
- [ ] **Step 4:** Same command -> PASS.
- [ ] **Step 5:** Commit `feat(p3): pure layout for the partner ping strip`.

---

### Task 3: Partners from guest RAM, drop the mask mapping

**Files:**
- Modify: `games/patapon3/runtime/p3_ping.h`, `games/patapon3/runtime/p3_ping.cpp`
- Test: `runtime/tests/test_p3_ping.cpp`

**Interfaces:**
- Consumes: `P3Partner`, `p3_ping_short_name` (Task 2).
- Produces:
  - `std::vector<std::string> p3_ping_read_names(const uint8_t* rdram, uint32_t id_base);` — per slot the name (`p3_ping_short_name` of the UTF-16 name at `id_base - 0x1128 + slot*0x23C`), "" for a free slot; same slot count as `p3_ping_read_slots`.
  - `p3_ping_pick_base(rdram, own, kind, anchors)` accepts `kind == P3Prim::None, anchors == 0`: any table (known bases first, then the scan) whose occupied slots contain `own`.
  - `p3_ping_assign` loses the Mask path: `kMaskOrder` and the `own_anchor` parameter go. `p3_ping_tag` loses the Mask branch (badge only).
  - If Task 1 found more VS slots: `kSlots` or a slot-count parameter per Task 1's table (record which in the commit).

- [ ] **Step 1: Tests.** Replace the mask tests (`masks 2p/3p/4p`, `own gold mask*`, `mask_count_mismatch_draws_nothing`, the Mask `p3_ping_tag` checks, `masks too`) with:
  - `pick_base_without_anchors`: table with own id in slot 2 at the 1.1.5b base -> `p3_ping_pick_base(ram, "P3C", P3Prim::None, 0) == b115`; with `"NOPE"` -> 0.
  - `read_names`: `put_name(base, 0, "Edson"); put_name(base, 2, "Krish2long")` -> `{"Edson", "", "Krish2", ""}`.
  - `strip_skips_left_player`: slot 1 name cleared, stale id kept -> `p3_ping_read_slots(...)[1].empty()` and `p3_ping_read_names(...)[1].empty()`.
  - Keep every hideout badge test unchanged.
- [ ] **Step 2:** Build + `ctest -R p3_ping` -> FAIL.
- [ ] **Step 3:** Implement; delete the now-dead mask code (`kMaskOrder`, `own_anchor`, mask branch of `p3_ping_tag`). Update the header comments (masks are now only a "mission screen" marker).
- [ ] **Step 4:** Build + `ctest -R p3_ping` -> PASS.
- [ ] **Step 5:** Commit `refactor(p3): partner names from the party table, masks no longer map slots`.

---

### Task 4: Draw the strip (mission, then VS)

**Files:**
- Modify: `games/patapon3/runtime/p3_ping_run.cpp`, `games/patapon3/runtime/p3_ping.cpp` (`p3_ping_classify` gets `P3Prim::VsHud` from Task 1 Step 4), `p3_ping.h`
- Test: `runtime/tests/test_p3_ping.cpp` (classify only)

**Interfaces:**
- Consumes: `p3_ping_strip`, `kMissionZone`, `kVsZone`, `p3_ping_read_names`, `p3_ping_pick_base(..., P3Prim::None, 0)`.
- Produces: no new API. Runtime state: `g_strip_tags` (std::vector<WorldTag>, GL thread) + `g_strip_kind` (Mask or VsHud) from the last presented frame.

- [ ] **Step 1:** Test `classify_vs_hud` with the PRIM recorded in Task 1 Step 4 (and one near-miss -> None).
- [ ] **Step 2:** Build + ctest -> FAIL; implement the `VsHud` case in `p3_ping_classify`; -> PASS.
- [ ] **Step 3:** `p3_ping_run.cpp`:
  - `on_prim`: on the **first** `Mask` or `VsHud` PRIM of a frame, draw every tag in `g_strip_tags` (if `g_strip_kind` matches) with `psp_ge_draw_tag(t, false)` — same layer as the HUD, so game windows drawn later still cover it. Remove the per-mask `tag()` path and `g_own_mask`/`g_masks`.
  - `on_frame`: screen = Mask seen ? mission : VsHud seen ? VS : badges ? hideout (unchanged path) : none. For mission/VS: keep `g_base` if its slots still hold `own`, else rescan every 2 s with `p3_ping_pick_base(rdram, own, P3Prim::None, 0)`; build `P3Partner{slot + 1 /* Task 1 rule */, names[slot], ping.ms, ping.via_server}` for every occupied slot != own (pings cached 1 s as today); `g_strip_tags = p3_ping_strip(partners, zone, kind == Mask)`.
  - Log once `[P3] ping strip: mission|vs` on the first strip.
- [ ] **Step 4:** `ninja` (full runtime) + `ctest --test-dir runtime/build` -> all pass.
- [ ] **Step 5: Lab mission** (diskquiet.py, two.py `mk3` 1.1.5b, Shooshookle, ports 9998+): screenshots on each screen show the strip with 2 entries, correct names/numbers, no overlap with masks/loot/FEVER worm; kill one visitor -> its entry stays (it is still in the party), nothing else vanishes; START dialog covers the strip. Save shots to `build/ping_recon/strip/`.
- [ ] **Step 6: Lab VS** (`vs.sh`): strip under the minimap with every other player's number, nothing under the yellow minimap frame or the score box. Skip if Task 1's gate stopped VS.
- [ ] **Step 7:** `rt_regress.py` (same time of day as its base) -> PASS.
- [ ] **Step 8:** Commit `feat(p3): partner pings in a fixed strip in missions and VS`.

---

### Task 5: Hand-off

**Files:**
- Modify: `build/beta/CHANGELOG.md` (current "en preparación" section): "El ping de los compañeros ahora sale arriba en el centro en las misiones (ya no desaparece si cae un héroe ni en las misiones al revés) y debajo del mapa en VS."
- Create: `build/plata/probar_ping_tira.bat` (copy `probar_ping.bat`, point at the rebuilt 1.1.5b exe).
- Update: memory `p3-ping-masks.md` -> superseded note + new memory for the strip.

- [ ] **Step 1:** Rebuild the 1.1.5b exe the .bat uses; run the .bat once in the lab port to see it boots.
- [ ] **Step 2:** Commit `docs: ping strip changelog and test script` (CHANGELOG only if it lives in the repo; else no commit).
- [ ] **Step 3:** Tell the user (Spanish): what to check (mission both directions, a hero dying, VS), the .bat path, branch name, not pushed.
