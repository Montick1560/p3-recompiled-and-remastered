# Ping under the party masks + real route ping + first own asset — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In online missions each partner's party mask (top-left HUD) shows the real ping of our connection to that
partner (direct or through the server) as a small signal icon + number; our own mask and badge show none; the icon
tells direct from server routes and is the port's first own image asset, a PNG next to the exe.

**Architecture:** Same seam as u15 (`psp_ge_observer.h`): the P3 game module classifies the mask PRIMs, maps them to
party slots, and calls `psp_ge_draw_tag` right after each mask. `WorldTag` grows two generic fields (text size, icon
frame) and the overlay draws an optional icon frame from one PNG strip the game module registers. The client
answers a per-partner `{ms, via_server}` from the direct link's route (u15 rule) and nothing for our own id.

**Tech Stack:** C++17 runtime (GL 3.3 / ES 3.0), stb_truetype text, `ge_texrep_decode_png`, ctest, Python/PIL for
the icon, the two.py/go2.sh harness in `build/infra`.

**Spec:** `docs/superpowers/specs/2026-11-02-ping-under-masks-design.md`

## Global Constraints

- Branch `ping-masks` in `psprecomp/`, from `ping-names` (ef58ef6). Local commits only; no push without the user's OK.
- Commits: author as configured ("Patapon Community"), no Claude co-author trailer; end with `Verified: ...` and
  `Agent: Claude Opus 5.5`.
- Colours / thresholds unchanged: < 80 (80,220,90), < 150 (255,210,40), else (240,70,60). Bars 3 / 2 / 1.
- Partner number = route RTT: direct p50 when the direct path is used; through the server the measured relay p50
  when known, else our server RTT + theirs; -1 when unknown. Own id: no tag, own badge untouched.
- Unknown partner ping (-1): grey icon (frame 0 of its route set, colour (150,150,150)), no number. Missing PNG:
  number only.
- Mask tag: number without "ms", text 7.5 PSP px tall (minimum 8 device px), icon 7×6 PSP px left of the number,
  the pair centred under the mask's health bar.
- Icon strip: 8 frames, `frame = bars + (via_server ? 4 : 0)`.
- `PSPRECOMP_P3_SHOW_PING=0` still turns everything off.
- Lab ports 9995-9998 only, never 9999. Nothing destructive on the PC.
- Recon / live checks in **"Shooshookle, la insaciable"** (Finca de la Impetuosidad, Free). **Never open the iron
  door** in that mission.
- Evidence before "done": ninja + ctest in `build/rtw`, live 2-instance shots, `rt_regress.py` (30 fps, same-session
  baseline) and `perf_hideout.py` (60 fps) unchanged.

## Review Focus

1. A player leaves mid-mission (one mask fewer): the remaining tags stay on the right masks or none are drawn. →
   Task 3 test `mask_count_mismatch_draws_nothing`.
2. Route flips mid-game (direct drops back to the server and returns): number and icon set follow within ~1 s, no
   stale direct number. → Task 2 test `route_reports_via_server`, Task 5 live step "kill direct".
3. Pause menu / results window over the HUD: the tag is covered like the mask. → Task 5 live shot "pause".
4. `assets/ping_signal.png` missing or broken: number only, one log line, no crash. → Task 4 test
   `icon_missing_file_is_refused`.
5. Render scale 1 (480×272 target): text readable, icon not blurred. → Task 5 live shots at scale 1 and 3.

---

### Task 1: Mask recon + the u15 visitor check (spike, timebox 90 min / 3 attempts)

**Files:**
- Create: `build/tools/ping_recon/masks.py`, `build/ping_recon/MASKS.md`, `build/infra/shooshookle.sh` (outside the repo)

**Interfaces:**
- Produces (MASKS.md, consumed by Task 3): the mask PRIM signature (prim, count, vtype, tex_addr / clut / tsz /
  tf), the health-bar PRIM signature + its box relative to the mask, the draw order → slot rule, on 1.1.4 and 1.1.5b;
  the verdict on the u15 hideout numbers through the server.

- [ ] **Step 1:** u15 visitor check first (hideout): two.py run with `ENV_B="PSPRECOMP_INFRA_DIRECT=0"` and server
  `--delay-ms 50`, `host_join.sh`; shots `j1_A`/`j1_B` + `badges.py` on both. Expected: each screen shows the
  partner's number on the partner's badge (≈ both server RTTs summed). A swap → note it; Task 3 fixes it.
- [ ] **Step 2:** New run (direct on), `host_join.sh`, then walk A to Finca de la Impetuosidad → "Shooshookle, la
  insaciable" with a shot per map step; save the key list as `shooshookle.sh` (pattern: `mission.sh`). Both deploy.
  Expected: `ms_A`/`ms_B` show two masks top-left.
- [ ] **Step 3:** `masks.py PORT` (pattern: `badges.py`): one display-list dump, every PRIM whose box lies in
  x < 120, y < 50 with signature, box, v0 colour. Run on A and B. Expected: one signature per mask part, a texture or
  CLUT that differs by hero class.
- [ ] **Step 4:** Order rule: compare the mask order on A's and B's screens with the party slots (`findtab.py`).
  Expected: a rule that holds on both screens.
- [ ] **Step 5:** Phases: shots walking, at the plant, with START (pause) open. Never touch the iron door.
  Expected: masks drawn in every in-mission phase; pause window drawn after them.
- [ ] **Step 6:** DxD 1.1.5b (`build/v115b/rtw`): Steps 2-4 again. Vanilla only if the lab save reaches the mission.
  One try with a 3rd instance (`join_only.sh`); if it does not join, note it and move on.
- [ ] **Step 7:** Write `MASKS.md`. If no rule maps masks to slots on both screens, stop and ask the user.

### Task 2: The partner ping carries its route (client)

**Files:**
- Modify: `runtime/include/net/direct_link.h`, `runtime/src/net/direct_link.cpp` (`route_rtt_ms` → `route`),
  `runtime/include/net/infra_client.h`, `runtime/src/net/infra_client.cpp` (`ping_ms` → `peer_ping`),
  `games/patapon3/runtime/p3_ping_run.cpp` (caller)
- Test: `runtime/tests/test_infra_client.cpp`

**Interfaces:**
- Produces: `struct Route { int rtt_ms = -1; bool direct = false; }; Route Link::route(uint32_t vip) const;`
  (direct = the path is in use; rtt = direct p50, or relay p50 while the relay is preferred, else -1).
  `struct PeerPing { int ms = -1; bool via_server = false; }; PeerPing Client::peer_ping(const std::string& id) const;`
  — our own id and non-members → `{-1, false}`; members → `{peer_ping_ms(route.rtt_ms, own, their), !route.direct}`.

- [ ] **Step 1: Failing tests.** `ping_per_member` → uses `peer_ping`: the relayed member shows ours + theirs with
  `via_server == true`; `CHECK(c.peer_ping("P3A").ms == -1, "own id: no number")`.
- [ ] **Step 2:** Build `test_infra_client` → FAIL (no `peer_ping`).
- [ ] **Step 3:** Implement; replace `route_rtt_ms` and `ping_ms` (no other callers: grep).
- [ ] **Step 4:** `ctest` → PASS (direct-route cases stay covered by `test_infra_signal`'s `peer_ping_ms` checks).
- [ ] **Step 5:** Commit `feat(infra): partner ping reports its route; no number for our own id`.

### Task 3: Mask anchors (pure game module)

**Files:**
- Modify: `games/patapon3/runtime/p3_ping.h`, `games/patapon3/runtime/p3_ping.cpp`, `runtime/include/psp_osk_overlay.h`
  (`WorldTag` fields only)
- Test: `runtime/tests/test_p3_ping.cpp`

**Interfaces:**
- Consumes: MASKS.md (Task 1); `PeerPing` (Task 2).
- Produces: `P3Prim::Mask` (replaces `P3Prim::Label`; delete Label if nothing else uses it); `p3_ping_classify`
  returns `Mask` for the recon signature; `p3_ping_assign(P3Prim::Mask, n, slots, own)` follows the recon rule,
  empty when `n` ≠ occupied slots; `p3_ping_tag(P3Prim kind, const GePrimBox& box, int ms, bool via_server)`: Mask →
  centred under the health bar, `text` digits only, `size_px` 7.5, `icon` = bars + 4·via_server (grey, no text when
  unknown); Badge → u15 tag, `icon` -1. `WorldTag` gains `float size_px = 0.0f; // 0 = default (u15 size)` and
  `int icon = -1; // frame of the registered icon strip, -1 none`. Own slot → no tag (caller skips -1 own).

- [ ] **Step 1: Failing tests:** `classify_mask` (recon signature → Mask; a near miss → None), `assign_mask_order`
  (2 and 3 players per the rule), `mask_count_mismatch_draws_nothing`, `mask_tag_layout` (digits only, size 7.5,
  icon = bars, +4 via server, centred x, y below the bar), `unknown_ping_grey_icon`.
- [ ] **Step 2:** Build `test_p3_ping` → FAIL. **Step 3:** Implement (+ the Task 1 swap fix if one was found, with a test).
- [ ] **Step 4:** `ctest` → PASS. **Step 5:** Commit `feat(p3-online): ping tags anchored on the party masks`.

### Task 4: The icon asset + drawing it

**Files:**
- Create: `games/patapon3/assets/ping_signal.png` (drawn by `build/tools/ping_icon/make_ping_icon.py`, outside the
  repo), `runtime/tests/test_world_tag_icon.cpp`
- Modify: `runtime/include/psp_osk_overlay.h`, `runtime/src/psp_osk_overlay.cpp`, `runtime/CMakeLists.txt`
  (copy `assets/ping_signal.png` next to the exe like `game_options.json`; test target)

**Interfaces:**
- Produces: `bool psp_world_tag_set_icon(const std::string& png_path, int frames);` — any thread; decodes with
  `ge_texrep_decode_png`, keeps the RGBA until the GL thread uploads it on the next tag draw; false (no icon) when
  the file is missing, undecodable, or its width is not a multiple of `frames`. `psp_world_tag_draw_target` draws
  `t.icon`'s frame tinted by (r,g,b) left of the text, the pair centred on `t.x`; `t.size_px` > 0 sets the text
  height (`max(8, fb_h * size_px / 272)`).

- [ ] **Step 1:** Draw the PNG: 8 frames side by side, each 56×48 px (8× the 7×6 PSP size); 3 rising bars, lit =
  white, unlit = (70,70,70), 4 px black outline; frames 0-3 = k bars lit; frames 4-7 = the same with a small server
  mark (a tiny stacked box) in the top-left. Look at it (Read the PNG) next to a mask crop from Task 1.
- [ ] **Step 2: Failing tests** `icon_missing_file_is_refused`, `icon_bad_width_is_refused` (frames = 3 → false),
  `icon_shipped_png_loads` (repo PNG, 8 frames → true).
- [ ] **Step 3:** Build → FAIL. **Step 4:** Implement (upload + tinted quad via the existing `rect` shader).
- [ ] **Step 5:** `ctest` → PASS; `build/rtw/assets/ping_signal.png` exists after `ninja`.
- [ ] **Step 6:** Commit `feat(overlay): world tags can carry an icon from a PNG strip` (PNG in the same commit).

### Task 5: Wire it in + live check

**Files:**
- Modify: `games/patapon3/runtime/p3_ping_run.cpp` (Mask anchors instead of labels; tag right after the mask,
  `on_top = false`; own slot skipped), `games/patapon3/runtime/p3_ping.cpp` (`p3_ping_register` registers
  `SDL_GetBasePath() + "assets/ping_signal.png"`, 8 frames; logs `[P3] ping icon: on` or `... missing, number only`)

- [ ] **Step 1:** Implement; `ninja` + `ctest` PASS.
- [ ] **Step 2: Live direct (1.1.4):** A + B, `host_join.sh`, `shooshookle.sh`, B with
  `PSPRECOMP_INFRA_DIRECT_IMPAIR=delay=100`. Shots `mk_A`/`mk_B`: each partner mask ≈ the same number on both
  screens, direct icon, own mask bare; START open → tags covered; render scale 1 and 3. Hideout before deploying:
  partner badges match, own badge bare.
- [ ] **Step 3: Live through the server:** `ENV_B="PSPRECOMP_INFRA_DIRECT=0"`: server icon, ≈ sum of server RTTs.
  Kill direct mid-mission (`kill_direct.sh`) on a direct run: icon flips to the server set within ~1 s.
- [ ] **Step 4: 1.1.5b:** the same with 2 instances, one shot per screen.
- [ ] **Step 5:** Missing asset: rename `build/rtw/assets` for one run → number only, log line; rename back.
- [ ] **Step 6:** `rt_regress.py` 30 fps (baseline made the same session) PASS; `perf_hideout.py` 60 fps within noise.
- [ ] **Step 7:** Commit `feat(p3-online): ping under each partner's party mask in missions`.

### Task 6: Package + docs

**Files:**
- Modify: `build/beta/make_beta_exes.sh` (copy `$B/assets` into the game folder; outside the repo),
  `ARCHITECTURE.md` (overlay row: tag icon), `README.md` (ping line), `docs/ENV_FLAGS.md` if a flag's text
  changes, `build/beta/CHANGELOG.md` ("Actualización 16 (en preparación)")

- [ ] **Step 1:** Edit. Expected: `grep -n assets build/beta/make_beta_exes.sh` shows the copy of `$B/assets`, and
  `build/rtw/assets/ping_signal.png` exists (no package build until the user asks for u16).
- [ ] **Step 2:** Commit `docs: ping tags under the party masks` (repo files only).
