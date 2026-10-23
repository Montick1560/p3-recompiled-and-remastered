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

## Out of scope
Direct UDP between players (hole punching) or a UDP relay: the Cloudflare tunnel carries only HTTP/WebSocket, so
UDP would need a forwarded port on the server machine. That is the user's decision, to be asked with the
measurements in hand. Changing the game's own netcode is also out of scope.
