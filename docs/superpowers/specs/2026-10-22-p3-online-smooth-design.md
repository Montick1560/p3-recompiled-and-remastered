# Smooth online co-op (lag and desync) — design

## Goal
An infrastructure-mode co-op mission plays evenly on both screens: no freezes on the visitor, units and enemies in
the same place on both screens within what the network allows. First on one PC with a local server (network
≈ 0 ms, so any unevenness there is ours), then over the public server (`wss://online.montick.app/infra`, RTT ~63 ms).

## Evidence (22 Oct 2026)
- User: "even on the same PC with the local server the game went uneven", video `build/infra/e2e12/rec/session.mp4`
  (40 s of a co-op mission, 8 fps screenshots via the debug socket, A = host over tcp://, B = visitor over ws://).
- In the recording's frames (`e2e12/rec/9997_*.tga`, `9998_*.tga`), B shows **runs of up to 7 identical screenshots**
  (~0.9 s, six runs of 3 or more in 40 s), while A never has more than 3. A whole identical frame, sky included,
  means B's game thread did not produce a frame. Also B answered half as many screenshot requests as A (159 vs 320).
  In the same frames the boss is in a different place and pose on each screen.
- Idle WebSocket RTT against a local server is 0 ms (`test_infra_client` with `INFRA_WS_URL`, 5 runs), so the
  transport is not slow when it is idle. That leaves its behaviour under load, the delivery path and the game's own
  waiting.
- The code paths that can stall or delay:
  1. `sceNetInetSendto` and the stream flush call `Client::send` synchronously. `TcpTransport` uses `send()` and
     `WinHttpWebSocketSend` blocks. Both run under `PspCpuReleaseScope`, so every send is two CPU-lock transitions
     and the calling guest thread waits for the network.
  2. Datagram queues hold up to 256 datagrams per port (`kQueueCap`) and 1 MiB in total. If the game reads fewer
     datagrams per frame than arrive, delivery delay grows to seconds. A real socket buffer is small and drops instead.
  3. WinHTTP gives no control over Nagle. Back-to-back small frames from the client can wait for the server's
     delayed ACK, which is up to 200 ms on Windows.
  4. The game may wait for its peer (lockstep). Then any late packet freezes it, and the fix is lower delivery
     latency, not anything in the game.

## Design
1. **Measure first, on both ends, with one clock.**
   - `PSPRECOMP_INFRA_STATS=<file>` writes a CSV of network events with `steady_clock` microseconds. That clock is
     QueryPerformanceCounter, which is system-wide, so two instances on one PC can be joined.
   - Each row carries an FNV-1a 32 hash of the payload, so a datagram can be matched across the two logs.
   - `build/infra/lagjoin.py` turns the two CSVs plus the two `PSPRECOMP_FRAMELOG`s into delay distributions per hop
     (send block, wire, queue wait) and lists the stalls.
2. **Never wait for the network on a guest thread.** The client gets an outbox and a writer thread:
   - Sends only enqueue; the writer thread does the actual sending.
   - Order between datagrams, stream segments and requests is kept.
   - When the outbox is over its byte cap, the oldest datagrams are dropped. Stream segments and requests are never
     dropped; a stream backlog over its cap closes the connection, as today.
   - The `PspCpuReleaseScope`s around sends go away, since a send no longer blocks.
3. **Datagrams stay fresh, like a real socket buffer.** A datagram older than 200 ms when the game reads it is
   dropped and counted. The game already lives with Wi-Fi loss, but not with seconds of backlog.
   This applies only if the measurement shows queue waits (gate in the plan).
4. **Stalls with no delivery gap** are hunted with `PSPRECOMP_CPU_TRACE` and fixed at their root.
5. **Internet path:** the same measurement against the public server. If WebSocket frames arrive in bursts
   (Nagle/delayed ACK) where TCP does not, the WebSocket transport gets its own framing over a socket with
   `TCP_NODELAY`. For `ws://` that is a plain socket; for `wss://` it adds Schannel TLS.

## Acceptance
On one PC (local server, A tcp://, B ws://), in a 60 s co-op mission with drums on both instances:
- Frames over 100 ms (FRAMELOG): at most 1 on each instance.
- Datagram delivery A→B and B→A, from the send call to the game's read: p99 under 50 ms.
- No datagram waits longer than 200 ms in a queue.

Over the public server: the same frame criterion, and delivery p99 under RTT/2 + 50 ms.

## Result (23 Oct 2026, one PC, local server)
Measured with `PSPRECOMP_INFRA_STATS`, the frame logs and `tools/infra_lagjoin.py` over a ~60-70 s marked window
of a co-op mission with drums on both instances (A tcp://, B ws://; reports in `build/infra/lag_*/REPORT.md`,
findings in `build/infra/lag_tcpws/FINDINGS.md`):

| run | frames > 100 ms A / B | send_block p99 | wire p99 | queue p99 / max | total p99 A->B / B->A | lost / dropped |
|---|---|---|---|---|---|---|
| before (lag_tcpws) | 3 / 2 | 0.2-0.3 ms | 0.4-0.5 ms | 34.2 / 74.9 ms | 34.5 / 34.1 ms | 0 / 0 |
| after the outbox (lag_tcpws2) | 1 / 0 | 0.2-0.3 ms | 0.5-0.6 ms | 36.9 / 66.2 ms | 37.2 / 35.9 ms | 0 / 0 |
| final, no screenshot polling in the window (lag_tcpws_final) | 0 / 0 | 0.2 ms | 0.4-0.5 ms | 21.2 / 21.3 ms | 21.5 / 16.1 ms | 0 / 0 |
| final build, while recording both windows at 60 fps (lag_final) | 0 / 0 | 0.2 ms | 0.4-0.5 ms | 30.7 / 31.4 ms | 31.1 / 21.3 ms | 0 / 0 |

Final windows: 30.0 fps on both instances, longest frame 37-57 ms. Acceptance on one PC: met.

What the evidence turned out to be:
- The four suspects were not the cause locally: sends returned in 0.1-0.3 ms, the wire took under 1 ms on both
  transports, a datagram waited at most one game frame in its queue, and nothing was lost or dropped.
- Every frame over 100 ms in the earlier windows overlapped the test driver's beat sync, which takes a full
  debug-socket screenshot (a 522 KB TGA file) about 80 times a second for 4 s on each instance.
- The PC's system disk (a DRAM-less QLC SSD, 89 % full) stalls writes for up to 42 s under that load or while
  Windows Search indexes the test output; every thread that writes a file stops meanwhile (frame log on the game
  thread, the stats writers, the server log). Two discarded runs froze all three processes together for 12-36 s.
  The e2e driver now probes the disk and waits for it to be quiet, and syncs to the beat once before the window.
- The stats log itself made that worse: it wrote on the client's reader and writer threads, so a stalled disk
  stopped them, pings stayed queued and the client dropped a live server after 10 s. Its rows now go to memory and
  a stats thread writes the file. The local test server also stops while its log write is stalled (48 s once);
  the public server runs on another machine.
- The e2e12 "before" video: B's runs of identical shots were the recorder's 5 s socket timeouts filled with the
  previous frame, plus one 1.5 s gap that did not reproduce.
- The stale-datagram drop (design item 3) was not needed: queue waits stayed far under 200 ms on one PC.

- Outside this design, found on the way: with the launcher, stderr goes to `logs/game.log` and the game thread logs
  thousands of lines, so on a disk like this one a log line can stall the game. A log writer off the game thread
  would remove that.

## Result over the public server (23 Oct 2026, 03:30-04:15)
Both instances on this PC, both over `wss://online.montick.app/infra` (ping RTT 62 ms). From this PC a datagram
crosses the internet twice (to the server and back), so the delivery bound is RTT + 50 = 112 ms, not RTT/2 + 50
(for players on two machines it is the mean of their RTTs + 50 ms). `infra_lagjoin.py --total-ms` sets it.

| run | send_block p99 | wire p50 / p99 (dgram) | wire p99 (stream) | total p99 (dgram) |
|---|---|---|---|---|
| hideout together (lag_pub_hideout) | 0.2-0.3 ms | 62-64 / 64-75 ms | 65-70 ms | 108 ms A->B (PASS) |
| co-op mission (lag_pub) | 0.2-0.3 ms | 62-63 / 64-65 ms | 66-82 ms | 127-148 ms (FAIL, see below) |

- The WebSocket path adds nothing visible on top of the RTT: datagram wire p99 is 2-13 ms over the ping, no bursts in
  multiples of 200 ms (one 202 ms stream segment in 3749). The own WebSocket transport (design item 5) is not needed.
- Sends never wait for the internet: send_block p99 0.3 ms over TLS.
- The mission run's delivery tail and its end (the visitor gave up on the host and left the room) come from this
  PC's disk: both games froze together for 47, 11, 11 and 6 s while the datagrams kept arriving, which the queue
  waits (up to 37 s) show. That is not the network and would not happen on two healthy machines.

## Out of scope
Direct UDP between players (hole punching) or a UDP relay: the Cloudflare tunnel carries only HTTP/WebSocket, so
UDP would need a forwarded port on the server machine. That is the user's decision, to be asked with the
measurements in hand. Changing the game's own netcode is also out of scope.
