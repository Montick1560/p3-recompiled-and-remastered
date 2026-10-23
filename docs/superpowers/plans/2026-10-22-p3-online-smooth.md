# Smooth online co-op (lag and desync) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An infrastructure-mode co-op mission plays evenly on both screens: no visitor freezes, and network delivery
measured and bounded, first on one PC with a local server, then over the public server.

**Architecture:**
1. Measure before touching anything: per-instance network event CSVs on one shared clock, joined with the frame logs
   by a script.
2. Then take the network off the guest threads: an outbox drained by a writer thread.
3. Then, only where the numbers say so: bound how long a datagram may wait, hunt the remaining stalls, and fix how
   the WebSocket transport sends.

**Tech Stack:** C++20 runtime (`psprecomp/runtime`, ctest in `build/rtw`), Python 3 stdlib (`tools/`), e2e driver
`build/infra/two.py` with `host_join.sh` / `mission.sh` / `go2.sh` (outside the repo).

**Spec:** `docs/superpowers/specs/2026-10-22-p3-online-smooth-design.md`. Read its Evidence section first.

## Global Constraints

- **Branch:** new local branch `online-smooth` from `patapon3-windows` (3def926, which already holds infrastructure
  mode). Nothing is pushed without the user's OK; the next push branch is `push-p3-32`.
- **Commits:** authored as configured ("Patapon Community"), no Claude co-author trailer, message ends with
  `Verified: ...`.
- **Native execution:** the user wants the work done inline ("native"), no helper models, no subagents unless they
  ask.
- **Processes and ports:** never touch the user's game on port 9999. Test instances use 9996-9998 and are killed by
  path (`*\build\rtw\*`), never by image name. Saves come only from copies (two.py already copies `build/save_weather`).
- **Gates for every task that touches the runtime:** ctest green, and 30 fps `rt_regress.py` vs `build/tour/wait30`
  PASS, run with `PSPRECOMP_RTC_FIXED=2026-10-22T08:48:00` (the hideout sky follows the clock):
  `PSPRECOMP_RTC_FIXED=2026-10-22T08:48:00 TOUR_RT_PORT=9998 python -I build/tools/gfx_tour.py --only rt --out C:/Users/torso/Documents/deco/build/tour/<tag>`, then
  `python -I build/tools/rt_regress.py build/tour/wait30 build/tour/<tag>`. A marginal fail (<1 % on animated
  checkpoints) gets one rerun before blaming the change.
- **Clean measurements:** nothing else heavy runs on the PC while measuring (no builds, no tours). Stalls caused by
  load are not findings (lesson from [p3-60fps-hitches]).
- **Docs in the same change:** `docs/ENV_FLAGS.md` for every new env var, `ARCHITECTURE.md` (repo root) when the
  client's threading changes (Task 2).
- **Reporting to the user:** in Spanish, with numbers before and after.

## Review Focus

1. **Connection lost while the outbox is full**: the writer stops, nothing deadlocks, `lost()` runs once, a later
   `connect()` works. Test `outbox_reconnect_after_loss` (Task 2).
2. **Disconnect while the writer is blocked** in a slow transport send: `disconnect()` returns within 1 s, because
   `close()` unblocks the transport. Test `disconnect_unblocks_writer` (Task 2).
3. **Stats file cannot be opened** (bad path): the game runs, stats stay off, one log line. Test
   `stats_bad_path_is_off` (Task 1).
4. **A stream segment never overtakes an earlier one or a request** after the outbox: the hideout state the host
   sends over TCP P2P must arrive in order. Test `outbox_keeps_order` (Task 2).
5. **Datagram age drop never touches stream segments** (the stream must stay reliable). Test
   `stale_drop_datagrams_only` (Task 3).

---

### Task 1: Network telemetry and the first measurement

**Files:**
- Create: `runtime/include/net/infra_stats.h`, `runtime/src/net/infra_stats.cpp`, `runtime/tests/test_infra_stats.cpp`
- Modify: `runtime/include/net/infra_client.h` (`Datagram::rx_us`), `runtime/src/net/infra_client.cpp` (hooks),
  `runtime/src/psp_perf.cpp` (one stderr anchor line), `runtime/CMakeLists.txt` (sources + test)
- Create: `tools/infra_lagjoin.py` (with `--selftest`)
- Modify (outside the repo): `build/infra/two.py` (env `LAG_DIR`, `B_TRANSPORT`).
  Create `build/infra/lag.sh` (the scenario).
  `two.py` and `lag.sh` live outside the repo; nothing to commit for them.
- Docs: `docs/ENV_FLAGS.md` (`PSPRECOMP_INFRA_STATS`)

**Interfaces:**
- Produces, in `net/infra_stats.h`, namespace `infra::stats`:
  - `enum class Ev { TxCall, TxDone, Rx, Pop, Drop };` (CSV names `tx_call,tx_done,rx,pop,drop`)
  - `bool open(const char* path);`: false and one `[INFRA] stats: cannot open <path>` line if the file fails.
    The first call from `enabled()` reads `PSPRECOMP_INFRA_STATS`.
  - `bool enabled();`
  - `int64_t now_us();`: `steady_clock` microseconds since its epoch. That is QueryPerformanceCounter on Windows,
    which is the same for every process on the PC.
  - `uint32_t hash(const uint8_t* data, size_t len);`: FNV-1a 32.
  - `void record(Ev ev, uint8_t kind, uint32_t vip, uint16_t src, uint16_t dst, const uint8_t* data, size_t len, int64_t extra_us = 0);`:
    - Writes the row `t_us,ev,kind,vip,src,dst,len,hash,extra_us`. vip is the destination for `tx_*` and the source
      for `rx`/`pop`/`drop`.
    - One mutex and a 64 KiB `setvbuf` buffer; `fflush` at most once a second and at `close()`.
  - `void close();`
  - `Client::Datagram` gains `int64_t rx_us = 0;`, set by the reader when the frame arrives.
- Hooks in `infra_client.cpp`:
  - `send_datagram` / `send_stream`: `TxCall` before the send, `TxDone` after the transport returns.
  - Reader, for frame kinds 2 and 3: `Rx`.
  - `pop_datagram` / `pop_stream`: `Pop` with `extra_us = now_us() - rx_us`.
  - Every queue-full drop: `Drop`.
- `psp_perf.cpp`: the first time the frame log opens, print `[FRAMELOG] start steady_us=<now_us at start>`. Same
  clock as `t_ms = 0`, so `infra_lagjoin.py` can align FRAMELOG rows; the CSV format does not change.
- `tools/infra_lagjoin.py A_net.csv B_net.csv [--frames A_frames.csv:A_log B_frames.csv:B_log] [--md OUT]`:
  - **Matching:** each `tx_call` on one side is matched to the `rx` and `pop` on the other side with the same
    `(kind, src, dst, len, hash)`, in order.
  - **Report per direction (A→B, B→A) and kind (dgram, stream):** count, unmatched (lost or dropped), and
    p50/p95/p99/max of:
    - `send_block` = tx_done − tx_call
    - `wire` = rx − tx_call
    - `queue` = pop − rx
    - `total` = pop − tx_call
  - **Per instance:** frames with dt over 100 ms, each with its time and the A→B/B→A `wire` and `queue` maxima in
    the 2 s before it.
  - The verdict lines are the spec's acceptance numbers: PASS/FAIL per criterion.
- `two.py`:
  - `LAG_DIR=<dir>` sets per instance `PSPRECOMP_INFRA_STATS=<dir>/net_<X>.csv` and `PSPRECOMP_FRAMELOG=<dir>/frames_<X>.csv`.
  - `B_TRANSPORT=tcp` gives B `tcp://127.0.0.1` instead of `ws://` (the tcp/tcp control).
- `lag.sh RUN`: with two.py READY in RUN, runs `host_join.sh RUN`, then `mission.sh RUN`, then on both instances
  march `* D PPPO---- 8` and attack `* D OOPO---- 8` (gfx_tour drum syntax), then `* S end`.

- [ ] **Step 1: Failing tests** `test_infra_stats.cpp`:
  - `stats_rows_and_hash`: open a temp file, record one `Rx`, close. The file has the header and one row with
    `ev == "rx"` and hash `== hash(data)`. `hash("a",1) == 0xE40C292C` (FNV-1a 32 of "a").
  - `stats_bad_path_is_off`: `open("no_such_dir/x/y.csv")` is false, `enabled()` is false, and `record` does
    nothing (no crash).
  - `pop_reports_queue_age`: a `Client` against the existing fake server (copy the `FakeInfraServer` helper or move
    it to a shared test header) receives one datagram, waits 30 ms, then pops. The stats row `pop` has
    `extra_us >= 30000`.
  - `infra_lagjoin.py --selftest`: two synthetic CSVs with 3 datagrams (one lost) give matched 2, unmatched 1 and
    the expected `wire`/`queue` values; `selftest OK`.
- [ ] **Step 2:** `ninja test_infra_stats && ctest -R infra_stats` fails (no header); `python -I tools/infra_lagjoin.py --selftest` fails (no file).
- [ ] **Step 3: Implement** as in Interfaces; register `infra_stats.cpp` in the runtime and in `test_infra_client`'s
  sources.
- [ ] **Step 4:** both pass; `ninja && ctest` green.
- [ ] **Step 5: Baseline measurements.** One per run, nothing else running.
  - Run `rm -rf build/infra/lag_tcpws && LAG_DIR=$PWD/build/infra/lag_tcpws python -I build/infra/two.py build/infra/lag_tcpws`
    in the background, then `lag.sh lag_tcpws`, then `Q`.
  - Repeat with `B_TRANSPORT=tcp` into `lag_tcptcp`.
  - Then `infra_lagjoin.py ... --md build/infra/lag_tcpws/REPORT.md` (and the same for tcptcp).
  - Expected: the report reproduces B's stalls (FRAMELOG frames > 100 ms on B) and says, for each stall, whether
    delivery was late just before it.
- [ ] **Step 6: Write the findings** in `build/infra/lag_tcpws/FINDINGS.md`:
  - Which of the spec's four suspects the numbers support, with the numbers.
  - Record the gate decisions in the ledger as rulings:
    - Task 3 runs if any `queue` p99 > 50 ms or max > 200 ms.
    - Task 4 runs if stalls > 1 remain after Task 2/3.
    - Task 5's WS fix runs if ws `wire` p99 exceeds tcp's by more than 20 ms in the public run (Task 5 Step 1).
- [ ] **Step 7:** 30 fps rt_regress PASS. **Commit** (`feat(net): infrastructure network telemetry (PSPRECOMP_INFRA_STATS) and lag report`), with the baseline numbers in `Verified:`.

### Task 2: The network never blocks a guest thread

**Files:**
- Modify: `runtime/include/net/infra_client.h`, `runtime/src/net/infra_client.cpp` (outbox + writer thread)
- Modify: `runtime/src/hle/psp_hle_inet.cpp` (drop `PspCpuReleaseScope` in `hle_sceNetInetSendto`,
  `flush_streams`, `report_bound`)
- Test: `runtime/tests/test_infra_client.cpp`
- Docs: `ARCHITECTURE.md` (the Infrastructure mode row: the client's reader, ticker and writer threads)

**Interfaces:**
- `Client::Options` gains `std::function<std::unique_ptr<Transport>(Address::Kind)> transport_factory;` (empty means
  `make_transport`). Tests inject a `SlowTransport`, whose `send_frame` can block on a latch, plus a fake peer.
- `static constexpr size_t kMaxOutboxDgramBytes = 256 * 1024;`
- `static constexpr size_t kMaxOutboxStreamBytes = 8 * 1024 * 1024;`
- `Client::send(frame)` becomes enqueue-only and returns at once:
  - One FIFO `std::deque<Outgoing>` with `{bytes, is_dgram}`, guarded by `out_mu_` / `out_cv_`.
  - Over `kMaxOutboxDgramBytes` of queued datagrams, the oldest datagram frames are erased (a `Drop` stats row
    each).
  - Over `kMaxOutboxStreamBytes` of queued stream and request frames: `lost("send backlog")`.
- `void writer();` is a thread started in `connect()`, next to the reader and ticker, and joined where they are.
  - It pops in FIFO order and calls `transport_->send_frame`.
  - A false return calls `lost("send failed")`.
  - `TxDone` moves here.
- `lost()` / `disconnect()` wake the writer and clear the outbox. `transport_->close()` already unblocks a blocked
  send (WinHTTP handle close; `shutdown` on the TCP socket — add it to `TcpTransport::close` if it is missing).
- Login keeps working: `connect()` sends the login through the same outbox after the writer starts.

- [ ] **Step 1: Failing tests:**
  - `send_never_blocks`: SlowTransport blocks every send for 300 ms. 50 `send_datagram` calls return in < 20 ms in
    total.
  - `outbox_keeps_order`: interleave datagrams, stream segments and a request. The fake peer sees them in call
    order.
  - `outbox_drops_oldest_datagrams`: SlowTransport is blocked; queue 300 KiB of datagrams and one stream segment,
    then release. The first datagrams are missing, the last are present, and the stream segment is present.
  - `disconnect_unblocks_writer`: a send is blocked forever; `disconnect()` returns in < 1000 ms.
  - `outbox_reconnect_after_loss`: SlowTransport fails its 3rd send. `connected()` becomes false, `lost` runs once
    (count the `room_destroyed` events), and a second `connect()` succeeds and sends.
- [ ] **Step 2:** `ninja test_infra_client && ctest -R infra_client` fails (no `transport_factory`).
- [ ] **Step 3: Implement** as in Interfaces; remove the three `PspCpuReleaseScope`s in `psp_hle_inet.cpp`.
- [ ] **Step 4:** tests pass; `ninja && ctest` green; the infra server selftest still `selftest OK`.
- [ ] **Step 5: Re-measure** both scenarios exactly as in Task 1 Step 5 (`lag_tcpws2`, `lag_tcptcp2`):
  - Expected: `send_block` p99 < 1 ms on both instances.
  - Write the before/after table in `build/infra/lag_tcpws2/REPORT.md`.
  - Run the co-op e2e sanity: hideout together, mission start, results screen (`mission.sh`, then `* P X` until
    results), screenshots.
- [ ] **Step 6:** 30 fps rt_regress PASS. **Commit** (`fix(net): infrastructure sends never block a guest thread (outbox + writer thread)`).

### Task 3: Datagrams stay fresh (gated by Task 1)

Skip with a ledger ruling if Task 1/2's reports show `queue` p99 ≤ 50 ms and max ≤ 200 ms.

**Files:**
- Modify: `runtime/include/net/infra_client.h`, `runtime/src/net/infra_client.cpp` (`pop_datagram`, `has_datagram`)
- Test: `runtime/tests/test_infra_client.cpp`
- Docs: `docs/ENV_FLAGS.md` (`PSPRECOMP_INFRA_DGRAM_MAX_AGE_MS`)

**Interfaces:**
- `Client::Options::dgram_max_age_ms = 200`; the env var `PSPRECOMP_INFRA_DGRAM_MAX_AGE_MS` overrides it, and `0`
  means never drop.
- `pop_datagram` and `has_datagram` first erase, from the front of that port's queue, datagrams with
  `now_us() - rx_us > max_age`, each counted (`Drop` row, extra = age).
- Every 5 s with drops, log `[INFRA] dropped N stale datagrams (oldest <ms> ms)` (rate-limited).
- Stream queues are untouched.

- [ ] **Step 1: Failing tests:**
  - `stale_datagrams_dropped`: max age 50 ms. Two datagrams arrive, wait 80 ms, a third arrives. `pop_datagram`
    returns the third.
  - `stale_drop_datagrams_only`: a stream segment that waited 300 ms is still popped.
  - `max_age_zero_keeps_all`.
- [ ] **Step 2:** `ctest -R infra_client` fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** green; re-measure (`lag_tcpws3`). Expected: `queue` max ≤ 200 ms. Check that the co-op mission
  still reaches results (the game tolerates the drops).
- [ ] **Step 5:** 30 fps rt_regress PASS. **Commit** (`fix(net): stale infrastructure datagrams are dropped like a full socket buffer`).

### Task 4: Remaining stalls, at their root (gated)

Skip with a ruling if `lag_tcpws2`/`lag_tcpws3` show at most 1 frame over 100 ms on each instance.

**Files:** decided by the finding. Evidence goes to `build/infra/stall/notes.md`.

- [ ] **Step 1:** Add `CPU_TRACE_B=<file>` to two.py (sets `PSPRECOMP_CPU_TRACE` on B only), and repeat the tcp/ws
  scenario with `CPU_TRACE_B=$PWD/build/infra/stall/cpu_B.txt`. For the two longest B stalls in the report, read
  the trace window:
  - Which guest thread held the CPU lock.
  - What it waited on (HLE call, delay, semaphore).
  - Whether the game's network thread was polling `sceNetInetRecvfrom` in a loop (a lockstep wait on its peer).
- [ ] **Step 2:** follow superpowers:systematic-debugging.
  - If it is a lockstep wait whose peer data was late, the answer is in delivery (back to the Task 1 numbers for
    that window).
  - If it is a runtime wait (lock, scheduler, I/O), write the failing test that reproduces it where a unit test can.
    Otherwise the reproduction is the scenario.
- [ ] **Step 3:** fix, re-measure (`lag_tcpws4`); expected ≤ 1 stall over 100 ms per instance. ctest green, 30 fps
  rt_regress PASS. **Commit** with the before/after numbers.

### Task 5: The internet path (public server)

**Files:** `build/infra/two.py` (`INFRA_PUBLIC=1` already exists). If the WS fix is gated in:
- Create: `runtime/src/net/ws_socket_transport.cpp` (`WsSocketTransport`, its own WebSocket framing over a socket
  with `TCP_NODELAY`)
- Modify: `make_transport`
- Test: `runtime/tests/test_infra_client.cpp`

- [ ] **Step 1: Measure** `INFRA_PUBLIC=1 LAG_DIR=$PWD/build/infra/lag_pub python -I build/infra/two.py build/infra/lag_pub`
  plus `lag.sh lag_pub`. Both instances use `wss://online.montick.app/infra`, which needs the user's tunnel up; if
  it is down, ledger it and skip to Task 6.
  - Compare `wire` p99 against RTT/2 (RTT from the instance logs' ping, ~63 ms).
  - Compare ws `wire` against the tcp/ws run's jitter.
- [ ] **Step 2 (gated: ws `wire` p99 more than 20 ms above tcp's in the same conditions, or bursts in multiples of
  ~200 ms):** implement `WsSocketTransport`.
  - For `ws://`: RFC 6455 client handshake and framing (masked binary frames, ping/pong, close) over a plain socket
    with `TCP_NODELAY`.
  - For `wss://`: the same over Schannel (`InitializeSecurityContextW` with `SCH_CREDENTIALS`, SNI = host); link
    `secur32`.
  - Selected by `PSPRECOMP_INFRA_WS=socket|winhttp`; default `socket` once it passes the e2e, `winhttp` kept as the
    fallback.
  - Tests:
    - `ws_socket_against_real_server`: starts `tools/infra_server.py` on free ports (as the selftest does), logs in,
      gets the search answer and the ping RTT.
    - `ws_socket_frames_roundtrip`: frames of 0, 125, 126, 65535 and 65536 bytes.
  - wss is verified against `wss://online.montick.app/infra` in the e2e only, when the tunnel is up.
- [ ] **Step 3:** re-measure `lag_pub2`. Expected: frames over 100 ms ≤ 1 per instance, and `total` p99 < RTT/2 + 50 ms.
  ctest green; 30 fps rt_regress PASS. **Commit**.

### Task 6: Prove it, show it, deploy

**Files:**
- Docs: `README.md` (one line: smoother online play, what was measured), the spec (a "Result" section with the
  before/after table).
- Hand-off (Spanish): `build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-10-23.md`. Check the name is free; if not, use the
  next letter.

- [ ] **Step 1: Final numbers.** Re-run `lag_tcpws_final` (and `lag_pub_final` if the tunnel is up). The
  before/after table covers frames > 100 ms per instance, `total` p99/max per direction, `send_block` p99 and
  stale drops.
- [ ] **Step 2: Video for the user.**
  - Record both windows of a final co-op mission side by side at 60 fps with the [p3-video-recording] recipe
    (`build/rec60/rec.py`, one window at a time is fine: two parallel recordings, then ffmpeg `hstack`).
  - Do not use recsnap: its debug-socket screenshots stall the instance being captured.
  - Publish it as a private Artifact next to the before video (`build/infra/e2e12/rec/session.mp4`) and the table.
- [ ] **Step 3: Regression.**
  - Ad hoc smoke: `ADHOC=1`, recipe in the previous plan's Task 3.
  - The identity e2e (`MAC_FILE_TEST=1` id1: B is "2.Edson").
  - `ninja && ctest` green; 30 fps rt_regress PASS.
- [ ] **Step 4: Deploy.**
  - Back up the six `psprecomp_runtime.exe` of `build/e2e_portable` and `build/e2e_portable3` (dxd-1.1.4 = `build/rtw`,
    dxd-1.1.5b = `build/v115b/rtw`, vanilla-eu = `build/vanilla/rtw`) into `build/backup_pre_smooth_<date>/`.
  - Build the variants, copy, `cmp`.
  - If `tools/infra_server.py` changed, say so in the hand-off: the user copies it to the `tester` machine.
- [ ] **Step 5: Commit** docs (`docs: smoother online co-op, measured`), merge `online-smooth` into
  `patapon3-windows` locally (fast-forward; ctest on the result), and ask the user before any push.
