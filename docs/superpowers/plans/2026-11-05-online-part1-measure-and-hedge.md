# Online Part 1: Measure, Harden, Send on Both Routes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (the user chose native: no subagents, no
> helper models). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the first online update (u18) tells us what the network really does to players, makes the server safe to
update, explains the crashes seen in online sessions, and delivers every mission datagram on the first route that
gets it there.

**Architecture:** new telemetry goes server first, stays backward compatible, and carries no addresses. While a pair
is Up, each game datagram is sealed once and goes out on both routes; the replay window the two routes already share
keeps the first copy and drops the second. Relay copies are droppable, so a stalled server link can never cost the
session. A server switch turns hedging off without a client release.

**Tech Stack:** C++17 runtime (`runtime/src/net`, ctest), Python 3 stdlib server (`tools/infra_server.py --selftest`),
lab kit `build/infra`.

**Spec:** `build/autopilot/PLAN_ONLINE_2026-11-05.md` (items A1, A3, B1, C2, C3, C4, C5, approved by the user 5 Nov
2026) and `docs/superpowers/specs/2026-10-24-p3-direct-p2p-design.md` (security model, still binding).

**Later parts, separate plans:** A2 desync meter + A4 international test; B2 TURN spike; C1 mission survives a server
loss; D1/D2 lobby. Each one is decided with the numbers this part produces.

## Global Constraints

- Branch `online-power` from `graphics-options` (84cb714). Commit author "Patapon Community", no Co-Authored-By. **No
  push, no deploy to `tester`, no R2 upload without the user's explicit OK.**
- Lab on ports 9995-9998, never 9999. Before any lab run list running game and launcher processes; if one is not mine,
  stop and ask. Run `build/infra/diskquiet.py` before measuring.
- Nothing destructive on any disk. Delete only files this work created inside its own run folders. Never move through
  junctions (`robocopy /XJ`).
- **No IP address or key** in a log line, report or database row. No per-packet log lines.
- The live server accepts `min_proto` 2: every new field is optional, u17 clients keep working unchanged.
- Beta builds never honour lab hooks (`PSPRECOMP_BETA_UNTIL`).
- Gate of every task: `ninja`, `ctest`, `python -I tools/infra_server.py --selftest`. Tasks touching the game path also
  run `rt_regress` (with `PSPRECOMP_RTC_FIXED`) and one two-instance infra smoke (`build/infra/direct_smoke.sh`).
- Deploy order: server first, then the client.

## Review Focus

1. **A u17 client on the new server:** old `link_stats` and `direct_stats` forms, no `bye`, no `nat`: accepted, stored,
   no error rows. Pinned in Task 1.
2. **Client input the server stores:** a hostile `prev_lost`, `nat`, or counters: refused or ignored, never logged raw.
   Pinned in Task 1.
3. **A relay stall while hedging:** copies pile up in the outbox and are dropped; the session stays up, game datagrams
   still flow by the direct path. Pinned in Task 6.
4. **The relay copy arriving first:** one delivery, and the later direct copy neither delivers again nor marks the
   path dead. Pinned in Task 6.
5. **Drain with solo players and VS rooms:** solo hideouts do not block the stop; a join during drain gets `ROOM_FULL`;
   a server already empty stops at once. Pinned in Task 3.

---

### Task 1: Server telemetry and session end reasons

**Files:**
- Modify: `tools/infra_server.py`, `tools/beta_summary.py`

**Interfaces (the wire, for Task 2):**
- `link_stats` also accepts optional ints `frames`, `slow_frames`, `frame_max_ms`.
- `direct_stats` entries also accept optional ints `host_link`, `peer_nat`, `direct_wins`, `relay_wins`.
- `direct_offer` accepts optional int `nat` (0 unknown, 1 stable mapping, 2 mapping differs per server).
- `direct_peer` event adds `host_link` (0/1: one of the two owns a room they share) and `peer_nat`.
- Login accepts optional `prev_lost` (a string from `LOST_REASONS`); login answer's `direct` adds `hedge` (bool).
- New op `bye` (no fields): the client is quitting on purpose. Its session ends as `quit`; a socket that closes without
  it stays `closed`.

- [ ] **Step 1: Write failing selftests** in the `link_stats` and direct selftests:
  old forms accepted and stored with NULL in the new columns; new forms stored; `nat=7`, `frames=-1`, `host_link="x"`
  answer `BAD_REQUEST`; `prev_lost="x\n[fake log]"` is not logged and adds no event, `prev_lost="server silent"` adds
  event `client_lost`; session with `bye` ends `quit`, without it `closed`; `direct_peer` carries `host_link` 1 between
  owner and visitor, 0 between two visitors, and `peer_nat`.
- [ ] **Step 2: Run** `python -I tools/infra_server.py --selftest`. Expected: FAIL on the first new assertion.
- [ ] **Step 3: Implement.** `LINK_EXTRA = ("frames", "slow_frames", "frame_max_ms")`, `PAIR_EXTRA = ("host_link",
  "peer_nat", "direct_wins", "relay_wins")`; `LOST_REASONS` = the five strings of `Client::lost` callers; new columns
  added by the existing `ALTER TABLE` loops; `sessions.nat` written at `direct_offer`.
- [ ] **Step 4: Add to `beta_summary.py`** a "Conexiones" section: % direct over host links and over all links, links by
  NAT pair, end reasons, `client_lost` counts, slow frames per session, wins per route. Its `--selftest` covers it.
- [ ] **Step 5: Run** both selftests. Expected: `OK`.
- [ ] **Step 6: Commit** `feat(infra-server): host links, NAT kind, frame numbers and real session end reasons`.

### Task 2: Client telemetry

**Files:**
- Modify: `runtime/include/net/link_stats.h`, `runtime/src/net/link_stats.cpp`, `runtime/include/net/direct_link.h`,
  `runtime/src/net/direct_link.cpp`, `runtime/src/net/infra_client.cpp`, `runtime/include/net/infra_client.h`,
  `runtime/src/psp_ge_draw.cpp`
- Test: `runtime/tests/test_link_stats.cpp`, `runtime/tests/test_direct_link.cpp`, `runtime/tests/test_infra_client.cpp`

**Interfaces:**
- Produces `void infra::frame_presented(int64_t now_us)` (lock-free, called once per present) and
  `FrameSummary { int frames, slow_frames, frame_max_ms; }` inside `LinkSummary`; slow is 100 ms or more.
- Produces `int direct::Link::nat_kind() const` (0, 1, 2 as above; 2 when STUN gave two different mappings, 0 with fewer
  than two answers).
- Produces `add_peer(..., int host_link, int peer_nat)`; `entry()` echoes them.
- `Client::lost(why)` stores `why` in `last_lost_` when it is not "disconnect"; the next `connect` sends it once as
  `prev_lost`. `close_session` sends `bye` after the stats.

- [ ] **Step 1: Write failing tests:** `frame_presented` at 16 ms steps then one 150 ms gap → `frames` counted,
  `slow_frames == 1`, `frame_max_ms == 150`; reset clears them. `nat_kind` from two fake STUN answers, same → 1,
  different port → 2, one answer → 0. Fake server sees `bye` on `disconnect()`, and `prev_lost` on the connect after a
  loss, and only on that one.
- [ ] **Step 2: Run** ctest on those tests. Expected: FAIL (not defined).
- [ ] **Step 3: Implement,** call `frame_presented` next to `perf_frame_end()` (`psp_ge_draw.cpp`). No per-frame lock, no
  allocation.
- [ ] **Step 4: Run** the gate plus `direct_smoke.sh`. Expected: PASS, and a local `--db` shows the new columns filled
  with no address (`build/infra/ipgrep.py`).
- [ ] **Step 5: Commit** `feat(infra): report frame times, NAT kind, host links and why a connection was lost`.

### Task 3: Server restarts without cutting a game

**Files:**
- Modify: `tools/infra_server.py`; rename `tools/run_infra_server.bat` to `tools/run_infra_server_local.bat`; update
  `build/autopilot/SERVIDOR_ONLINE.md`, `build/beta/servidor_beta.bat`

**Interfaces:**
- `--drain-file PATH`: while the file exists `op_search` answers no rooms and `op_join` answers `ROOM_FULL`; the server
  stops by itself when no room has 2 or more members. Solo sessions are cut by that stop.
- Log lines start with a full date. `/beta` adds `"metrics": bool` (db and log on), the first check of every review.
- The `.bat` loops: on a drain stop it deletes the flag and starts again.

- [ ] **Step 1: Write failing selftests:** with a two-member room and the flag present, search is empty and a third
  player gets `ROOM_FULL`, the server still runs; after one leaves, it stops; flag on an empty server stops it within
  one tick; a VS room plus a hideout on one connection counts each room once; flag removed before the stop returns to
  normal; `/beta` reports `metrics`.
- [ ] **Step 2: Run** selftest. Expected: FAIL.
- [ ] **Step 3: Implement** in `tick` (checked once per second, like `beta_reload`).
- [ ] **Step 4: Run** selftest. Expected: `OK`. Check no tool parses the log's time prefix:
  `grep -rn "infra_server.log" tools build/infra`.
- [ ] **Step 5: Commit** `feat(infra-server): drain before a restart; dated log; metrics flag in /beta`.

### Task 4: Crash lines that can be located

**Files:**
- Modify: `runtime/src/main.cpp:83-99`, `runtime/src/psp_backtrace.cpp`
- Test: new `runtime/tests/test_backtrace.cpp`, `runtime/CMakeLists.txt`

**Interfaces:**
- Produces `std::string psp_module_of(uintptr_t addr)` (`"ig9icd64.dll+0x1234"`, the exe as `"exe+0x..."`, `"?"` when
  no module holds it).

- [ ] **Step 1: Write failing test:** the address of a test function → starts with `exe+`; `GetProcAddress(kernel32,
  "Sleep")` → contains `kernel32` (case-insensitive); address 1 → `?`.
- [ ] **Step 2: Run** ctest. Expected: FAIL.
- [ ] **Step 3: Implement** with `GetModuleHandleExW(FROM_ADDRESS | UNCHANGED_REFCOUNT)`; the filter prints
  `module=<...>` and calls the existing `psp_print_host_backtrace("crash")`. Non-Windows returns `"?"`.
- [ ] **Step 4: Lab check on a lab instance only:** a ctypes `CreateRemoteThread` with start address 1 reproduces #90's
  signature; the log has `[CRASH] ... module=?` and a `[BT] crash:` line, and the launcher still classifies the exit.
- [ ] **Step 5: Commit** `feat(runtime): crash line names the module and prints the stack`.

### Task 5: Deferred minors

**Files:**
- Modify: `runtime/src/net/direct_link.cpp`, `runtime/src/net/infra_client.cpp`, `runtime/include/net/infra_client.h`
- Test: `runtime/tests/test_direct_path.cpp`, `runtime/tests/test_infra_client.cpp`

- [ ] **Step 1: `thread_id_`** is set as the first line of `Link::run()`, not after the thread starts, so a stop from
  the Link's own thread can never read a stale id.
- [ ] **Step 2: Write the missing test:** `flaps_` doubles the hold on a quick return, and resets after a return longer
  than `route_flap_ms`.
- [ ] **Step 3: `lost()` transport race:** a lost connection racing a new `connect` must close the transport of its own
  connection, never the new one. Failing test first with an injected transport; fix by holding it as `shared_ptr`,
  copied under `mu_`.
- [ ] **Step 4: FIN on `sceNetInetTerm`:** read what the game sees today. If a stream peer already gets its reset from
  `direct_drop` or room leave, close this item in the ledger with the evidence; only if not, send FIN at Term.
- [ ] **Step 5: Run** the gate. Expected: PASS.
- [ ] **Step 6: Commit** one commit per fix.

### Task 6: Send every datagram on both routes

**Files:**
- Modify: `runtime/include/net/direct_path.h`, `runtime/src/net/direct_path.cpp`, `runtime/include/net/direct_link.h`,
  `runtime/src/net/direct_link.cpp`, `runtime/include/net/infra_client.h`, `runtime/src/net/infra_client.cpp`,
  `tools/infra_server.py`, `docs/superpowers/specs/2026-10-24-p3-direct-p2p-design.md`
- Test: `runtime/tests/test_direct_path.cpp`, `runtime/tests/test_direct_link.cpp`, `runtime/tests/test_infra_client.cpp`

**Interfaces:**
- `Path::send(type, body, len, now_ms, out, std::vector<std::vector<uint8_t>>* relay_copy = nullptr)`: with `relay_copy`
  and state Up, seals once and sends the same bytes on both routes, whatever the route choice prefers. Without it,
  today's behaviour.
- `PathStats` adds `direct_wins`, `relay_wins`: game datagrams accepted first on each route. These prove or disprove
  the feature in the field.
- `LinkOptions::hedge` (from the login answer `direct.hedge`). `Link::send_dgram` calls `relay_` outside `mu_`, because
  the callback can lose the connection, and that stops the Link.
- `Client::Outgoing` adds `bool droppable`, true for datagrams and hedge copies; the outbox cap drops the oldest
  droppable frame. Hedge copies have `kind == 0`, so no stats rows.
- Server: `--no-hedge`, beta config `"hedge": false`. Both default to on, and `direct.hedge` is reported at login.

- [ ] **Step 1: Write failing `Path` tests:**
  - Up with `relay_copy` → one `Out`, one copy, equal bytes.
  - Up, relay preferred → still both.
  - Probing → `false`, nothing sealed.
  - Receiver: relay copy then direct copy → one app delivery, `relay_wins == 1`; direct then relay → one delivery,
    `direct_wins == 1`; direct copy lost → relay copy delivers.
  - Relay copy first for 10 s with keepalives flowing → still Up.
- [ ] **Step 2: Write failing `Link` and `Client` tests:**
  - Two Links on loopback with `hedge`: one delivery per datagram, and `relay_` got each copy.
  - Injected transport blocked for 2 s while 2000 hedge copies queue: no `lost`, the outbox stays under
    `kMaxOutboxDgramBytes`, and direct delivery goes on.
  - `hedge` off → relay gets no game datagram.
- [ ] **Step 3: Run** ctest. Expected: FAIL.
- [ ] **Step 4: Implement** as the interfaces say. `shortcut:` comment on the route-choice score: with hedging on it
  only feeds the ping shown to players; delete its hold and flap logic once the field numbers confirm hedging.
- [ ] **Step 5: Run** the gate. Expected: PASS.
- [ ] **Step 6: Lab acceptance** (`p2p_baseline.sh`, `kill_direct.sh`, both impaired routes: client
  `PSPRECOMP_INFRA_DIRECT_IMPAIR`, server `--impair`), hedge on against off, same profiles as
  `build/infra/p2p_direct/REPORT.md`:
  - Total p99 and datagrams at 250 ms or more: not worse than `intl3` (161/172 ms, 2 of 819) and `bad` (353/280 ms, 30
    of 775).
  - Killing the direct path mid-mission loses at most 2 datagrams per direction (today ~20), no frames over 100 ms, no
    disconnect.
  - Server stalls with direct clean: no disconnect, and the direct numbers are unchanged.
  - `ipgrep.py` finds no address. Report: `build/infra/hedge/REPORT.md`.
- [ ] **Step 7: Add a "Send on both routes" section to the design spec,** with the numbers and the kill switch.
- [ ] **Step 8: Commit** `feat(direct): send each game datagram on both routes, first copy wins`.

### Task 7: Mixed 30/60 fps room (recon)

**Files:**
- Create: `build/infra/mixed_fps.sh`, `build/infra/mixed_fps/REPORT.md`

- [ ] **Step 1: Measure** A at 60 fps + smooth and B at 30 fps, then swapped, then both at 60 as the baseline, on a
  mission without early enemies (the gargoyle separates the armies with no network at all). Use `desync_meter.py`,
  comparing **A own vs B own** views, the mistake of 4 Nov.
- [ ] **Step 2: Rule.** Mixed no worse than the baseline → write it in the report and close. Worse → write the numbers
  in the ledger and raise it in the Part 2 plan; do not fix it here.
- [ ] **Step 3: Commit nothing in psprecomp.** The kit lives in `build/infra`.

### Task 8: Package u18

**Files:**
- Modify: `build/beta/CHANGELOG.md`; create `build/beta/ACTUALIZACION_18.md`, `build/beta/ANUNCIO_U18_DISCORD.md`
- Modify: `DOWNLOAD_URL` in `tools/beta_report_server.py`, launcher version

- [ ] **Step 1: Run** the whole gate on the 3 release trees, plus an infra smoke and an ad hoc smoke.
- [ ] **Step 2: Build** with `build/beta/make_beta_exes.sh`, signed `latest_u18.json`, mandatory. Smoke on `E:/p3_u18_smoke`.
- [ ] **Step 3: Write** the user steps in order: drain-capable server on `tester` first, new `.bat`, report server, R2
  zip, `latest.json`. **Stop there: the user publishes.** Ask before any push.
