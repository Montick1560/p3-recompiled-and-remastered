# Direct player-to-player path for infrastructure mode — design

## Intent

On beta night 1 (23 Oct 2026) four players in different countries (host in Mexico, one in Venezuela) played
"[Multi] Torre de la Nevisca de la Legión". Each screen showed a different game:
- a boss that hides under the floor almost never came out on the host's screen while its HP went down;
- a player was hit on one screen but flew away before the attack on his own.

Video: `E:\psprecomp_runtime 2026-10-23 10-12-25 PM.mp4`. The user wants this fixed fast. **Security is above the
feature** (user's words: "seguridad por encima del feature"). A direct path that is not safe must not ship. The
relay path that works today always stays as the fallback.

## Why it happens (what we know, what we guess)

Known:
- On PSN, the game's infrastructure mode used the network only for rooms (Matching2). In play, consoles talked
  directly: PSP P2P UDP (`sceNetInet` type 6) and P2P TCP streams (type 10). The game was written for internet
  latency over direct paths.
- In the port, every byte between players goes through `tools/infra_server.py`:
  - over WebSocket/TLS behind a Cloudflare Tunnel, on the user's home PC (`tester`);
  - kind 2 frames carry datagrams, kind 3 frames carry stream segments (`net/infra_protocol.h`);
  - path from Venezuela to the host: Venezuela → Cloudflare → host's server → Cloudflare → host.
- Everything rides one TCP connection per player. A lost packet holds every later byte of that player, datagrams
  included, until TCP retransmits it (Windows min RTO ≈ 300 ms, more on long paths). Then everything arrives in a
  burst. A UDP game sees that as stalls and bursts, not as loss.
- In a co-op mission most sync traffic is **stream** traffic (`build/infra/lag_final`, 232 s, one PC):
  - the host sent 7.5 stream segments/s of ~880 B (max 1025) and 3.2 datagrams/s of ~83 B (max 364);
  - the visitor sent 1.8 segments/s and 2.8 datagrams/s.
  So moving only datagrams would not fix the desync. Streams must use the direct path too.
- The server stores no RTT or loss per player, so beta night 1 has no network numbers.

Guess (to confirm in Phase 0): with a long relayed path and TCP head-of-line stalls, state messages reach each
console late and in bursts, so the consoles drift apart. The boss's "come out" state is the visible case.

## Revision (29 Oct 2026): what Phase 0 found and how this now ships

Phase 0 (Part 1) ran on 24 Oct; numbers in `build/infra/p2p_baseline/REPORT.md`.
- **The guess holds in the lab.** With the "international" relay profile (90 ms each way, 2 % TCP stalls of 400 ms),
  mission datagrams arrive at **p99 473 / 329 ms** instead of the ~140 ms the delay alone explains; 30 packets were
  held ≥ 250 ms in 98 s. The game itself does not freeze: what the other player sends arrives late.
- **Inside a mission the game sends only datagrams** (~5/s per player). Stream segments (~20/s from the host) flow in
  the hideout, at mission choice and at deploy. This contradicts the 23 Oct `lag_final` reading above ("most sync
  traffic is stream traffic"), which counted the whole session. It held in 4 runs of one co-op mission; it is not yet
  checked on a boss mission or with 4 players.
- **Screenshots cannot measure the drift** (each camera follows its own hero). Acceptance needs the game state read
  from both instances' RAM at the same time.

Consequences (they replace the matching lines of "Phases" and "Calls made by Claude"):
1. If a boss mission with 4 players also shows datagrams only, **Phase 1 alone carries the mission traffic** and
   ships first, as its own update. Phase 2 (streams, `reliable_channel`, kind 4) ships in the next update. If the
   boss mission shows streams, Phases 1 and 2 ship together.
2. Updates reach players through the launcher's auto-updater (launcher 0.3.0-beta.1+, `PUBLICAR_ACTUALIZACION.md`).
   A pair goes direct only when both players run the new version; old clients keep the relay (protocol 2 is still
   accepted).
3. The telemetry stays in the server's SQLite `beta.db` (29 Oct: a PostgreSQL move was weighed and declined: a few
   dozen players, two writer processes, WAL is enough; it would add a driver and credentials to the live server).
4. Before Phase 1, the real beta's `link_stats` (stored since update 3) are analysed to state how many sessions
   suffer from the relay, so the field acceptance can be compared with real numbers, not only the lab.

## Goals

1. Players in the same room exchange game traffic (datagrams **and** stream segments) over a direct, encrypted,
   authenticated UDP path when their networks allow it. No open port on any router, no server cost: NAT traversal
   uses free public STUN servers, and the room server keeps doing the signaling over its TLS channel.
2. Stream segments over the direct path use our own ACK/retransmit layer, tuned for game traffic (RTO from the
   measured RTT, not TCP's 300 ms floor). Bytes reach the game in order, exactly once, also across a path switch.
3. Any pair that cannot go direct, any player who opts out, any old client, and any failure mid-game falls back to
   today's relay without breaking the game's sockets.
4. Measure before and after, in a lab (impaired links on one PC) and in the field (beta telemetry without IPs).
5. A player's real IP never leaves the set of people they are playing with at that moment.

## Non-goals

- A UDP port on the user's server, or a VPS. The user can't pay for a VPS now. Opening a port exposes their home
  IP to everyone; it may come later as an extra relay.
- IPv6 direct paths (later; IPv4 only now).
- TURN or any third-party relay.
- Changing the game's own sync logic.
- Ad hoc mode (it has its own transport).

## Anti-goals (a failure even if it "works")

- Any address this feature handles (candidates, peer addresses, STUN results) in a log, crash/beta report, the
  server database or the launcher UI, or sent to a player outside the room. The server's operator log keeps
  today's line with each connection's own address. That line is outside this rule, but direct-path lines never add
  an address.
- Accepting any packet on the direct socket that is not authenticated with the pair's current key, or replying to
  one. A reply to an unauthenticated packet is an amplification/reflection vector.
- Sending game traffic to an address that has not proven it holds the pair key.
- Nonce reuse, home-made crypto primitives, or a key that outlives its room pairing.
- A direct-path failure that breaks or duplicates a game stream, or freezes the game.
- Shipping test hooks (impairment, insecure dev mode) that a player can turn on by accident in a beta build.

## Security model

**Threats considered:**
- a malicious player in the room;
- a malicious player in another room;
- an on-path attacker between two players;
- anyone on the internet who learns a player's UDP port;
- a hostile STUN server.

**Trusted:** our room server, over TLS (`wss://`, Cloudflare). It already sees all relayed traffic today.

Rules:
1. **Keys from the server, over TLS only.**
   - For each pair of room members that both opted in, the server makes a fresh 32-byte key and a random 32-bit
     pair id (`secrets`). It sends both to the two members in a `direct_peer` event.
   - The key never appears in any log.
   - The client accepts `direct_peer` only on a `wss://` connection, or on a loopback `tcp://`/`ws://` connection
     (local tests). Any other plain-text connection keeps the client relay-only, with a log line.
2. **Per-direction keys.** `k_ab = BLAKE2b-256(key = pair_key, msg = "p3d1 dir" || vip_a || vip_b)`, where `a` is
   the lower vip, and `k_ba` the same way with `vip_b || vip_a`.
3. **AEAD on every packet.**
   - XChaCha20-Poly1305 from **Monocypher 4.x**, vendored under `runtime/third_party/monocypher/` (single C file,
     CC0/BSD-2, pinned version + SHA-256 in a README).
   - Nonce = 8-byte packet counter + 16 zero bytes. One counter per direction key, starting at 1, never reused.
     A key is never kept across pairings.
   - The clear header (magic `P3D1`, pair id, counter) is the associated data.
4. **Replay protection.** A 2048-packet sliding window per direction. Duplicate or too-old counters are dropped
   before the payload is used.
5. **Silence to the unauthenticated.**
   - Anything on the direct socket that fails parsing, the pair-id lookup, the tag check or the replay check is
     dropped and counted, with no reply and no log line per packet.
   - STUN answers are accepted only from the STUN server addresses we asked, with the 96-bit transaction id we
     sent.
6. **Path validation before traffic.**
   - A candidate address becomes a path only after an authenticated `PROBE_ACK` from it echoes the random 64-bit
     challenge of our `PROBE`. Game traffic goes only to a validated path.
   - Probing is bounded: at most 4 candidates per peer, 10 probes per candidate per attempt, probes ≤ 96 B, at most
     3 attempts per pair per room pairing. A lying peer can make us send at most ~4 KB to a third party per pairing
     (reflection bound).
   - An authenticated packet from a new address (NAT rebinding) only starts a validation of that address.
7. **Privacy of addresses.**
   - Clients send their candidates to the server in `direct_offer`. The server keeps them in memory only, never
     logs or stores them, and forwards them only to room peers that also opted in.
   - Host candidates (LAN IPs) are forwarded only when both peers' reflexive IPs are equal (same home network).
   - On room leave/destroy the server sends `direct_drop` and both clients wipe the key and the peer addresses.
   - The client logs only the candidate type (`srflx`/`host`), the vip, and the RTT.
8. **Rate limits.**
   - Per pair: ≤ 600 packets/s and ≤ 1 MiB/s inbound on the direct socket. That is far above the game's measured
     rate (~12 packets/s, ~10 KB/s).
   - Over the limit, the extra packets are dropped. A pair over the limit for 3 s in a row goes back to relay for
     that pairing.
   - The existing queue byte caps of `infra::Client` still apply.
9. **Opt-out and kill switches.**
   - Launcher setting `[online] direct = true|false` (default true) → `PSPRECOMP_INFRA_DIRECT=0` when false. An
     opted-out client sends no `direct_offer`, never contacts STUN, and is never paired.
   - Server config `direct: false` turns the whole feature off for everyone, without a client release.
10. **Test hooks are not in beta builds.** Link impairment and the loopback/insecure exceptions exist only in
    builds without `PSPRECOMP_BETA_UNTIL`. A beta exe ignores those env vars and logs that it did.
11. **Hardening tests.**
    - Unit tests: tamper (one bit flip anywhere), wrong key, wrong pair id, replay, counter wrap refusal, truncated
      and oversized packets.
    - A ctest fuzz loop runs the packet parser and the STUN parser over random and mutated inputs: no crash, no
      acceptance.
    - STUN parsing is checked against the RFC 5769 test vectors.

## How it works

### Components (client, `runtime/src/net/`, pure where possible)

| Unit | Role | Pure? |
|---|---|---|
| `third_party/monocypher` | AEAD, BLAKE2b, constant-time compare | — |
| `secure_random` | OS CSPRNG: BCryptGenRandom / getrandom / arc4random. Fails closed: no randomness → no direct path | no |
| `direct_crypto` | key derivation, `seal()` / `open()`, replay window | yes |
| `stun` | encode a Binding Request, parse a Binding Success (XOR-MAPPED-ADDRESS) | yes |
| `direct_path` | per-peer state machine: candidates → probing → validated → keepalive → dead; outputs "send these bytes to this address" | yes (clock injected) |
| `reliable_channel` | per-pair ordered, exactly-once delivery of stream segments over an unreliable, reorderable link: seq numbers, cumulative ACK + SACK, RTO = max(40 ms, SRTT + 4·RTTVAR), fast retransmit, bounded window | yes |
| `direct_link` | I/O shell: one UDP socket and a thread; STUN gathering; owns the per-peer `direct_path` and `reliable_channel`; feeds received game data into `infra::Client`'s existing queues | no |

### Wire format of the direct path

```
clear:   "P3D1" | u32 pair_id | u64 counter                     (16 B, AEAD associated data)
sealed:  u8 type | type body                                    (XChaCha20-Poly1305)
tag:     16 B
```

| Type | Body |
|---|---|
| `PROBE` | u64 challenge |
| `PROBE_ACK` | u64 challenge echo + u64 own challenge |
| `KEEPALIVE` | empty |
| `DGRAM` | u16 dst_port, u16 src_port, payload |
| `SEG` | u32 seq, u16 dst_port, u16 src_port, segment bytes (today's kind 3 payload) |
| `ACK` | u32 cumulative seq, u32 SACK bitmap |
| `CLOSE` | the sender wipes the pair |

A sealed packet over 1400 B goes through the relay instead (no IP fragmentation). The game's largest datagram is
0x574 B; stream segments are ≤ 1025 B.

### Stream segments across paths (the hard part)

Today the relay is reliable and ordered, so `p2p::Streams` has no ACKs. Once a pair is `direct`, every stream
segment between the two goes through the pair's `reliable_channel`:
- On the direct path it is a `SEG` packet.
- When the direct path dies, the unacknowledged segments and all new ones are sent through the relay with the same
  seq numbers, inside a new **kind 4** frame: an opaque sealed packet that the server forwards to the peer vip like
  a datagram but never drops.
- The receiver deduplicates and orders by seq whichever path the packet came on, so a path switch neither loses
  nor duplicates bytes.
- A pair that never went direct keeps using kind 3 frames exactly as today. Old clients see no change.

Datagrams have UDP semantics (they may be lost or reordered), so they go on whichever path is up, with no channel.

Ordering at the switch-over:
- From the moment a client handles `direct_peer` for a vip, every stream segment to that vip enters the channel.
  Until a path is validated, the channel's packets travel as kind 4 over the relay.
- Kind 3 frames sent before that moment and kind 4 frames sent after it share the sender's single ordered
  connection to the server. The server forwards frames in arrival order.
- The server sends `direct_peer` to the receiver before it can forward any kind 4 frame of that pairing.
- So the receiver always delivers the last kind 3 segment before the first channel segment.

On `direct_drop`, unacknowledged channel segments are discarded and the pair's game streams get a reset. That only
happens when one side leaves the room or disconnects, when the game is closing those streams anyway.

### Signaling (server, `tools/infra_server.py`, protocol 3)

- Login: the client sends `"caps": {"direct": 1}` when it supports the feature and is not opted out. The answer
  adds `"direct": {"on": bool, "stun": ["stun.cloudflare.com:3478", "stun.l.google.com:19302"]}` from the server
  config, so the STUN list and the kill switch need no client release.
- `direct_offer {cands: [{"t": "srflx"|"host", "a": "ip:port"}]}`:
  - at most 4 candidates, IPv4 only, validated;
  - stored per connection in memory, replaced on each offer;
  - rate-limited to 4 per minute.
- When two connections share a room, both have caps and an offer, and the server's `direct` is on: the server
  makes `{pair_id, key}` and sends each side `direct_peer {vip, pair, key, cands}`. `cands` are the peer's
  candidates, after the host-candidate rule.
- Room leave/destroy, or disconnect of either side: `direct_drop {vip, pair}`.
- Kind 4 frames: forwarded only between connections that share a room and have a live pairing. Size ≤ 1500 B, never
  dropped, under the stream byte budget.
- Metrics (SQLite, no addresses): pairings offered, pairs that went direct, fallbacks, and per-session summaries
  from the client's `direct_stats` at session end (path type, RTT p50/p95, loss %, time direct, fallback count).

### Client behaviour

1. At login, if enabled: the client opens the direct socket (an ephemeral UDP port) and asks the STUN servers for
   its reflexive address from that same socket. That is up to 2 servers × 3 tries, 1.5 s total, in the background.
   Then it sends `direct_offer`.
2. On `direct_peer`:
   - it creates `direct_path` (probing all candidates every 100 ms for ≤ 5 s) and `reliable_channel`;
   - the first validated candidate becomes the path;
   - a log line `[INFRA] direct: vip 0a500003 up (srflx, rtt 48 ms)` records it.
3. While direct:
   - `send_datagram` / stream sends to that vip go direct;
   - the relay connection stays open for requests and as the fallback;
   - there is a KEEPALIVE when the pair has been silent for 2 s;
   - RTT is measured from PROBE/ACK timing.
4. Path dead (nothing authenticated for 4 s): back to relay, with the stream channel continuing over kind 4.
   Retry punching after 30 s, at most 3 attempts per pairing.
5. On `direct_drop`, or at the end of the session: send CLOSE, wipe keys and addresses, send `direct_stats`.

## Phases

The order is fixed by the user's priority (fast) and the security constraint (no half-safe intermediate ships):

- **Phase 0, measure and reproduce (ships first, small, safe):**
  - Client telemetry of the relay link: ping RTT p50/p95/max, the number of pings answered after ≥ 250 ms, and the
    longest gap between two frames received from the server while a ping was outstanding. It is sent as
    `link_stats` at session end and stored by the server (session row, no address). The next beta night then
    gives real numbers per player, even before any direct path exists.
  - A lab impairment mode for the local server's relay (delay, jitter, TCP-stall emulation) and, in non-beta builds,
    for the client's direct socket (delay, jitter, loss, reorder).
  - Reproduce the drift on one PC with an "international" profile (180 ms RTT, 30 ms jitter, 2 % loss) and record the
    baseline numbers: stream delivery p50/p99, stalls, frames over 100 ms.
- **Phase 1, secure direct path for datagrams:** Monocypher, secure_random, direct_crypto, STUN, server signaling
  (protocol 3), direct_path, direct_link, opt-out + kill switch, telemetry. It is useful by itself and proves the
  security layer in the field.
- **Phase 2, streams over the direct path:** reliable_channel, kind 4 frames, path switch, and the lab acceptance
  with the impairment profile.
- **Phase 3, field:** beta build, telemetry review (direct success rate, RTT vs relay), the user's international
  playtest.

## Acceptance

Lab (one PC, 2 to 4 instances, local server, "international" impairment on the direct path and the matching stall
emulation on the relay baseline):
- Stream segment delivery (send → game read):
  - direct p99 ≤ one-way delay + 1 RTT + 50 ms;
  - the relay baseline's p99 is recorded next to it, and direct must be lower.
- A checksummed stream test (both directions, 10 MB, 5 % loss, 20 % reorder): every byte arrives in order, exactly
  once, also with the direct path killed and restored three times during the transfer.
- A co-op mission start→results with the direct path killed mid-mission: the game goes on over the relay with no
  disconnect; frames over 100 ms ≤ 1 per instance outside the 4 s detection window.
- Opt-out client, old client (protocol 2), server `direct: false`: everything over the relay as today; 30 fps
  rt_regress PASS; ad hoc smoke OK.
- Security tests (rule 11) green. A grep of every client log, report and the server DB written during the lab
  run finds no IPv4 address except 127.0.0.1 and 0.0.0.0. The server log's direct-path lines carry none.

Field (beta, Phase 3):
- Telemetry shows the direct success rate per night and RTT direct vs relay.
- The user's international playtest: the boss comes out on every screen and hits land on every screen.

### The launcher setting (decided by the user)

**On by default.** The option must explain what turning it on or off means and how the player's IP is protected.
The text sits next to the switch in the online options. Final Spanish copy (user-facing):

> **Conexión directa entre jugadores** (recomendada)
>
> **Activada:** tu juego se conecta directo con los jugadores de tu sala, como en PSN y en la mayoría de los juegos
> en línea. Hay menos retraso y todos ven la misma partida.
>
> **Desactivada:** todo pasa por nuestro servidor. Se puede jugar igual, pero con jugadores lejanos puede haber más
> retraso y diferencias entre pantallas.
>
> **Privacidad:** para conectarse directo, los juegos de una sala intercambian su dirección de conexión, como
> cualquier juego en línea. Solo se comparte con tu sala mientras juegan juntos, la conexión va cifrada y no se
> muestra en pantalla ni se guarda en ningún registro.

The copy stays honest (it says the connection address is shared and with whom) without alarming detail. Disclosure
is a requirement: the paragraph may be reworded later, but it can't drop the fact that the address is shared with
the room.

Whether the switch can change while the game runs: no. It takes effect at the next game start, like the other
online options, and the help text says so if the options screen shows that note for others.

## Settled details (builder check)

- Keys and peer addresses live in memory only and are wiped with `crypto_wipe` on drop/exit. A direction counter
  that reaches 2^62 drops the pairing (it is never reached in practice).
- STUN server names are resolved (IPv4) on the `direct_link` thread. Resolution failure means no srflx candidate,
  so that client stays relay-only.
- The beta/non-beta distinction for test hooks is the existing `PSPRECOMP_BETA_UNTIL` build define (`psp_beta.h`).
- Launcher: `[online] direct` follows the pattern of `[online] relay` (`options.rs`, `ini.rs`, `launch.rs`, a
  Spanish label in the options screen).

## Left to the builder

- Exact constants within the stated bounds: window sizes, keepalive, probe timing.
- Thread layout of `direct_link`. It must not take the guest CPU lock or block a guest thread, same as the outbox
  rule of `infra::Client`.
- Whether `direct_link` lives inside `infra::Client` or beside it, as long as `psp_hle_inet.cpp` keeps calling the
  same `send_*` / `pop_*` API.
- The lab harness details. It extends `build/infra/two.py`, `tools/infra_lagjoin.py` and `PSPRECOMP_INFRA_STATS`.

## Calls made by Claude (not discussed with the user)

- Monocypher (vendored) over Windows BCrypt or libsodium: portable, auditable single file, no install.
- XChaCha20-Poly1305 with counter nonces, and server-generated pair keys over TLS (the server is already trusted
  with the relayed traffic). We do not do a peer-to-peer key agreement: it adds code without changing who must be
  trusted.
- Public STUN servers (Cloudflare, Google), set from the server config.
- IPv4 only.
- Streams over the direct path need our own reliability layer (Phase 2). (29 Oct: missions turned out to be
  datagram-only in Phase 0; streams still matter for the hideout and deploy. See "Revision".)
