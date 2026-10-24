# Direct P2P, Part 1 — Measure and Reproduce (Phase 0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get real numbers for the online lag before building the direct path:
- relay-link telemetry from every player, stored by the server without addresses;
- a lab impairment mode for the local server that reproduces an international link on one PC;
- a recorded baseline that Part 2's acceptance compares against.

**Architecture:**
- A pure `infra::LinkStats` accumulator in the runtime collects ping RTTs and receive gaps. `infra::Client` feeds it
  and sends a `link_stats` request before it disconnects.
- `tools/infra_server.py` stores those numbers in the `sessions` table (new columns, migrated in place).
- A new `--impair` option delays, jitters and stalls (TCP head-of-line emulation) every frame the server forwards
  to a connection, in order.

**Tech Stack:** C++20 runtime (ctest in `build/rtw`), Python 3 server (`--selftest`), the existing lab tools:
`build/infra/two.py`, `tools/infra_lagjoin.py`, `PSPRECOMP_INFRA_STATS`, `PSPRECOMP_FRAMELOG`.

**Spec:** `docs/superpowers/specs/2026-10-24-p3-direct-p2p-design.md` (Phase 0). Part 2
(`docs/superpowers/plans/2026-10-24-p3-direct-p2p-part2-direct.md`) builds the direct path and uses this baseline.

## Global Constraints

- Branch: create local `direct-p2p` from `beta-1` (HEAD `fa5ca66` or later). **Nothing is pushed**; ask before any
  push.
- Commits:
  - author `Patapon Community <rexgamer1560@gmail.com>`;
  - no Claude co-author trailer;
  - the message ends with `Verified: ...`.
- Native execution: inline, no subagents, no helper models.
- No address of any kind in the new telemetry: not in the client log line, not in the request, not in the DB.
  Numbers only.
- Telemetry must never delay the game's exit: the `link_stats` request waits at most 1000 ms, and only on the
  network worker (never on a guest thread).
- `--impair` is a test option of the local server. The public server never runs it. `servidor_beta.bat` is
  unchanged.
- An old server (no `op_link_stats`) answers `BAD_REQUEST`; the client ignores that answer. An old client never
  sends the op. Both keep working.
- Test instances never use port 9999 (use 9997/9998 via `two.py`). `SHOTS_DIR=E:/p3_tmp_shots`. Run
  `python -I build/infra/diskquiet.py 60 900` before any timing measurement.
- Gates for runtime tasks:
  - `ninja && ctest` in `build/rtw`;
  - 30 fps rt_regress PASS (`PSPRECOMP_RTC_FIXED=2026-10-22T08:48:00 TOUR_RT_PORT=9998 python -I
    build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/<tag>`, then
    `python -I build/tools/rt_regress.py build/tour/wait30 build/tour/<tag>`).
- Gate for server tasks: `python -I tools/infra_server.py --selftest` → `selftest OK`, plus
  `sh build/infra/regress_vs.sh`.

## Review Focus

1. **A session that ends by connection loss** (Wi-Fi drop, server restart) has no chance to send `link_stats`; the
   row keeps NULLs. Nothing may block or retry. Covered in Task 1 (`lost()` does not send).
2. **Absurd or hostile numbers** in `link_stats` (negative, huge, floats, strings, missing keys) are refused with
   `BAD_REQUEST` and nothing is stored. Covered in Task 2's selftest.
3. **A second `link_stats` in one session** overwrites the first. It must not add a row or fail. Covered in Task 2.
4. **An old database file** from beta night 1/2 (no new columns) opens and gets the columns added once; an already
   migrated file is left alone. Covered in Task 2.
5. **`--impair` keeps per-connection order** under jitter: frames to one connection never overtake each other, like
   TCP. Covered in Task 3.

---

### Task 1: Relay-link telemetry in the runtime

**Files:**
- Create: `runtime/include/net/link_stats.h`, `runtime/src/net/link_stats.cpp`
- Modify: `runtime/include/net/infra_client.h`, `runtime/src/net/infra_client.cpp` (ticker, reader, disconnect)
- Modify: `runtime/CMakeLists.txt` (new test; add `src/net/link_stats.cpp` to the runtime sources and to
  `test_infra_client`)
- Test: `runtime/tests/test_link_stats.cpp`, `runtime/tests/test_infra_client.cpp` (new case)

**Interfaces:**
- Produces:
  ```cpp
  namespace infra {
  constexpr int kSlowPingMs = 250;
  struct LinkSummary {
      int pings = 0, slow_pings = 0;           // answered pings; those with RTT >= kSlowPingMs
      int rtt_p50 = -1, rtt_p95 = -1, rtt_max = -1;  // -1 without pings
      int max_rx_gap_ms = 0;                   // longest gap between server frames while a ping was outstanding
  };
  class LinkStats {  // thread-safe
  public:
      void ping_rtt(int ms);       // keeps the most recent 4096
      void rx_gap(int ms);
      LinkSummary summary() const;
      void reset();
  };
  nlohmann::json to_json(const LinkSummary& s);  // {"pings","slow_pings","rtt_p50","rtt_p95","rtt_max","max_rx_gap_ms"}
  }
  ```
- `Client` gets a `LinkStats link_` member and `LinkSummary link_summary() const`.
- Percentiles are nearest-rank over the stored RTTs (p50 = element at index `ceil(0.50·n) − 1` of the sorted
  copy, p95 the same with 0.95).

- [ ] **Step 1: Write the failing tests** in `runtime/tests/test_link_stats.cpp` (same CHECK macro style as
  `test_paced_step.cpp`):
  - `empty_summary`: `pings==0`, `rtt_p50==-1`, `rtt_max==-1`, `max_rx_gap_ms==0`.
  - `percentiles_nearest_rank`: RTTs 1..100 → `rtt_p50==50`, `rtt_p95==95`, `rtt_max==100`, `pings==100`.
  - `slow_pings_counted`: RTTs {100, 249, 250, 900} → `slow_pings==2`.
  - `gap_is_max`: `rx_gap(30); rx_gap(700); rx_gap(10)` → `max_rx_gap_ms==700`.
  - `keeps_most_recent_4096`: 5000 RTTs where the first 904 are 10000 and the rest 50 → `rtt_max==50`,
    `pings==5000` (the count is of all answered pings, the window is for the percentiles).
  - `json_fields`: `to_json` has exactly the six keys, all integers.
- [ ] **Step 2: Register `test_link_stats` in `runtime/CMakeLists.txt`** next to `test_paced_step`. Build:
  `ninja test_link_stats` in `build/rtw`. Expected: fails to compile (`net/link_stats.h` missing).
- [ ] **Step 3: Implement `net/link_stats.h/.cpp`** per the interface (mutex inside; `std::deque<int>` window).
- [ ] **Step 4: Run** `ninja test_link_stats && ./test_link_stats.exe`. Expected: `link_stats: all tests passed`.
- [ ] **Step 5: Write the failing client test** `link_stats_sent_on_disconnect` in `test_infra_client.cpp`, with
  the existing `FakeInfraServer`:
  - Connect, let three pings be answered (`ping_interval_ms = 50`), then `disconnect()`.
  - Assert `fake.got` holds a kind-1 frame with `op == "link_stats"`, `pings >= 3`, all six keys, and no key named
    `addr`/`ip`.
  - Assert `disconnect()` returned within 1500 ms even with `answer_pings` off for the last request (the server
    never answers `link_stats`).
  - Second case `no_link_stats_after_loss`: `fake.close_client()`, wait for `connected()==false`; no `link_stats`
    frame.
- [ ] **Step 6: Run** `ninja test_infra_client && ./test_infra_client.exe`. Expected: the two new cases FAIL, the
  others pass.
- [ ] **Step 7: Wire it in `infra_client.cpp`:**
  1. In `ticker()`'s ping answer, call `link_.ping_rtt(...)` with the same value pushed to `rtts_`.
  2. Remember the send time of the oldest unanswered ping (`ping_out_ms_`, 0 when none).
  3. In `reader()`, before `last_rx_ms_ = now_ms()`, if `ping_out_ms_ != 0` call
     `link_.rx_gap(now − previous last_rx_ms_)`.
  4. In `connect()`, `link_.reset()`.
  5. In `disconnect()`, while still connected, send `{"op":"link_stats", ...to_json(link_.summary())}` through
     `request()` and wait for the answer or 1000 ms, whichever comes first, before `lost("disconnect")`. Log one
     line: `[INFRA] link: N pings, p50 X ms, p95 Y ms, max Z ms, S slow, longest gap G ms`.
  6. `lost()` sends nothing.
- [ ] **Step 8: Run** `ninja && ctest` in `build/rtw`. Expected: all tests pass, including `link_stats_tests` and
  `infra_client_tests`.
- [ ] **Step 9: Gate:** rt_regress 30 fps PASS (tag `p2p_p1t1`).
- [ ] **Step 10: Commit** `feat(infra): relay-link telemetry sent at session end (link_stats)`. The `Verified:` line
  lists ctest and rt_regress.

### Task 2: Server stores `link_stats`

**Files:**
- Modify: `tools/infra_server.py`:
  - `SCHEMA` and `MetricsDb`: migration;
  - new `op_link_stats`;
  - `selftest`.
- Modify: `tools/beta_summary.py`: per-session network columns.
- Test: `python -I tools/infra_server.py --selftest`

**Interfaces:**
- Consumes: Task 1's request `{"op":"link_stats","pings","slow_pings","rtt_p50","rtt_p95","rtt_max","max_rx_gap_ms"}`.
- Produces: `sessions` columns `pings INT, slow_pings INT, rtt_p50 INT, rtt_p95 INT, rtt_max INT, max_rx_gap_ms
  INT` (NULL until reported). Answer `{}` on success.

- [ ] **Step 1: Write the failing selftest section** `link_stats` (a block in `selftest()`, using the `--db` temp
  file pattern already in the selftest):
  - A logged-in client sends valid stats → answer ok, and the session row holds exactly those six values.
  - A second send overwrites the row.
  - Each invalid form answers `BAD_REQUEST` (0x80550D01) and leaves the row unchanged: value −1, 600001, 1.5,
    `"10"`, `true`, a missing key, an extra key `ip`.
  - Before login → `FORBIDDEN`.
  - Migration: create a DB with the old `SCHEMA` (copy the current `CREATE TABLE sessions` text into the test),
    start a server on it → the columns exist; start again → no error and no duplicate columns.
- [ ] **Step 2: Run** `python -I tools/infra_server.py --selftest`. Expected: FAIL in the new block (unknown op →
  `BAD_REQUEST` on the valid send).
- [ ] **Step 3: Implement:**
  - `op_link_stats(self, conn, msg)` checks every value with `type(v) is int and 0 <= v <= 600000` and the exact key
    set, then `UPDATE sessions SET ... WHERE id=?`.
  - The migration (`ALTER TABLE sessions ADD COLUMN ...` for each missing column, found with
    `PRAGMA table_info(sessions)`) runs in the `MetricsDb` writer thread before the first batch.
  - Log line: `"%s link: p50 %d p95 %d max %d slow %d/%d gap %d"` using `conn.label()`, which is today's operator
    line format.
- [ ] **Step 4: Run** the selftest. Expected: `selftest OK`.
- [ ] **Step 5: `beta_summary.py`:** add the six columns to the per-session table. Run it on a copy of a DB with
  NULLs and check that it prints `-`.
- [ ] **Step 6: Gate:** `sh build/infra/regress_vs.sh` passes.
- [ ] **Step 7: Commit** `feat(infra-server): store relay-link telemetry per session (no addresses)`.

### Task 3: `--impair` for lab runs

**Files:**
- Modify: `tools/infra_server.py`:
  - `Server.__init__` gets an `impair` parameter;
  - a new `impaired_send(conn, data)` used by `on_datagram`, `on_stream` and the delayed ping answer;
  - `main()` gets the `--impair` argument;
  - `selftest`.
- Modify: `docs/` server notes (`build/autopilot/SERVIDOR_ONLINE.md` is user-side; add the option to the server
  guide inside the repo where `--delay-ms` is documented, `grep -rn "delay-ms" docs/`).

**Interfaces:**
- Produces: `--impair "delay=MS,jitter=MS,stall=PCT,stall_ms=MS"`. All keys are optional integers; PCT may be a
  float.
  - Every frame forwarded to a connection gets the due time `max(now + delay + U(−jitter, +jitter),
    conn.last_due)`, so per-connection order is kept, like TCP.
  - With probability `stall/100` per frame, the connection's due time first jumps by `stall_ms`. This is
    head-of-line blocking after a lost TCP segment, so every later frame of that connection waits too.
  - The RNG is seeded (`random.Random(2)`).
  - `--delay-ms` keeps its current meaning.

- [ ] **Step 1: Write the failing selftest block** `impair`:
  - A server with `impair={"delay":50}` delivers a datagram A→B ≥ 45 ms after the send.
  - With `{"delay":40,"jitter":30}`, 200 datagrams A→B with sequence numbers in the payload arrive in send order.
  - With `{"stall":100,"stall_ms":200}`, a datagram arrives ≥ 190 ms late.
  - A malformed `--impair` string (`"delay=x"`) makes `main()` exit with an argparse error. Test the parser
    function `parse_impair(text) -> dict` directly.
- [ ] **Step 2: Run** the selftest. Expected: FAIL.
- [ ] **Step 3: Implement** `parse_impair(text)`, `conn.last_due`, and `impaired_send`. It reuses `self.delayed` and
  the existing heap pump, and never blocks the select loop.
- [ ] **Step 4: Run** the selftest → `selftest OK`, and `sh build/infra/regress_vs.sh` → passes.
- [ ] **Step 5: Commit** `feat(infra-server): --impair lab option (delay, jitter, TCP stall emulation)`.

### Task 4: Baseline on one PC ("international" profile)

**Files:**
- Create: `build/infra/p2p_baseline.sh`. It is a local tool and is not committed to psprecomp, like the other
  `build/infra` drivers.
- Create: `build/infra/p2p_baseline/REPORT.md` (outside the repo).
- Modify: the ledger only (`.superpowers/sdd/<plan>/progress.md`).

**Interfaces:**
- Consumes: Task 3's `--impair`, `two.py` (`INFRA_SERVER_ARGS`, `LAG_DIR`), `tools/infra_lagjoin.py`, the drum
  driver `D <seq> <reps>` of `two.py`, and `build/infra/mission.sh` (co-op mission start → results).
- Produces: `build/infra/p2p_baseline/REPORT.md` with, for each profile:
  - stream segment delivery A→B and B→A, p50/p95/p99/max (send call → game read);
  - datagram delivery, same percentiles;
  - number of gaps ≥ 250 ms;
  - frames over 100 ms per instance;
  - the `[INFRA] link:` lines.

  Part 2's acceptance reads these numbers.

- [ ] **Step 1: Write `p2p_baseline.sh PROFILE OUTDIR`.** It runs `diskquiet.py`, then a 2-instance `two.py`
  (`INFRA_ONLY=AB`, `SHOTS_DIR=E:/p3_tmp_shots`, `LAG_DIR=OUTDIR`). Then `mission.sh`, 90 s of drums on both,
  results, and `tools/infra_lagjoin.py OUTDIR > OUTDIR/lagjoin.txt`. Profiles:
  - `clean`: no impairment;
  - `intl`: `--impair delay=90,jitter=15,stall=2,stall_ms=400` (≈180 ms RTT, 2 % loss with Windows-like RTO);
  - `bad`: `--impair delay=150,jitter=40,stall=5,stall_ms=600`.
- [ ] **Step 2: Run** the three profiles into `build/infra/p2p_baseline/{clean,intl,bad}`. Expected: `clean`
  matches `lag_final` (stream p99 < 50 ms). `intl` and `bad` show gaps ≥ 250 ms and stream p99 far above
  RTT/2 + 50 ms. If `intl` shows no gaps at all, record that: it means the TCP-stall hypothesis is weak, and Part 2
  Task 10's acceptance must be re-read before building streams.
- [ ] **Step 3: Visual check of the symptom.** During the `intl` run, take 10 `* S d<n>` shot pairs (A and B at the
  same instant) while a boss or enemy moves. Record in the report whether positions or states differ between A and
  B, and by how much, next to the `clean` run's pairs.
- [ ] **Step 4: Write `REPORT.md`** (Spanish summary first, then the tables) and add the headline numbers to the
  ledger.
- [ ] **Step 5: No commit in psprecomp** (all outputs are in `build/`). The ledger line records the numbers.

### Task 5: Ship the telemetry in the next beta update

**Files:**
- Modify: launcher version: the `chore(launcher)` bump pattern of update 2 (`0.2.0-beta.4`).
- Create: `build/beta/ACTUALIZACION_3.md`.
- User-side: copy `tools/infra_server.py` to `tester` (SMB `\\192.168.194.59\c$\Users\tester\Documents\patapon-server`,
  after backing up the live copy as `infra_server_u2.py`), then the user restarts `servidor_beta.bat`.

**Interfaces:**
- Consumes: Tasks 1–2.

- [ ] **Step 1:** Rebuild the beta exes and the portable folder exactly as in update 2's Task 4 Steps 1–3
  (`docs/superpowers/plans/2026-10-24-p3-beta-1-update-2.md`). Zip name `patapon3-beta1-u3.zip`.
- [ ] **Step 2:** Check the package:
  - the zip holds no `versions/`, `settings.ini`, `logs/` or `psprecomp_adhoc_mac.txt`;
  - each beta exe logs the beta date and `debug socket off`.
- [ ] **Step 3:** Copy the server to `tester` (backup first) only after asking the user. Ask them to restart it.
  `curl https://online.montick.app/beta` still answers.
- [ ] **Step 4:** Write `ACTUALIZACION_3.md` (the shape of `ACTUALIZACION_2.md`). It says the update adds anonymous
  connection measurements (retraso y cortes, sin direcciones) to understand the lag between countries.
- [ ] **Step 5:** After the user uploads to R2: `curl` the public URL and compare the SHA-256.
- [ ] **Step 6:** After the next beta night: copy `beta.db` over SMB (read-only copy) and run `beta_summary.py`.
  Write the per-player RTT/stall table (labels only, no addresses) into the ledger and into Part 2's ledger as its
  field baseline.
