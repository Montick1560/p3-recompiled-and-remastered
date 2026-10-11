# Ad hoc multiplayer over the Internet (PRO ad hoc server) — design

Status: written 2026-10-10 by Claude while the user was away; the user delegated the decisions
("Va, pues será el ADHOC … dale con todo, confío en ti"). Every choice the user did not state is
marked **[assumption]** so it can be corrected on review.

## Goal

Patapon 3 DxD (UCES01421) multiplayer in the native port, the way the community plays it today:
the game's **ad hoc** mode (PSP local wireless) carried over the Internet by a **PRO ad hoc server**
— PPSSPP's protocol, used by Paragate / patapon.net (Madwig). Success:

1. Two instances of the port (same PC, then two PCs) see each other in the game's ad hoc lobby,
   form a party and play a mission together.
2. The port and a PPSSPP player play together through the same server (interoperability).
3. Nothing changes for single player: with networking unconfigured the game behaves as today.

Out of scope: infrastructure mode / PSN (the Sony game servers no longer exist; reviving them would
mean reverse-engineering and re-implementing a server — see "Later"), voice chat, game sharing.

## What the user said vs. assumptions

- Said: ad hoc, not infrastructure for now; Paragate is the community path; helpers (Fable 5.1,
  Grok, DeepSeek) are available if needed.
- **[assumption]** Target protocol = PPSSPP's (PRO ad hoc server, TCP 27312; peer-to-peer UDP/TCP
  data with PPSSPP's port offset), so the port is interoperable with PPSSPP and Paragate.
- **[assumption]** Development and tests use a local server: PPSSPP's built-in one (the user's
  PPSSPP already listens on 27312) and a small Python test server in `tools/`. The port is
  **not** pointed at patapon.net until the user talks to Madwig (Paragate has anti-cheat and the
  DxD anti-tamper checks already fail in the port).
- **[assumption]** Configuration through `jugar.bat` environment variables first (server host,
  nickname, MAC, port offset); an in-game/settings UI is not part of this design.

## How PSP ad hoc maps to the Internet (PPSSPP model)

- **adhocctl** (control): the PSP joins a "group" (a named wireless network). PPSSPP replaces the
  radio with a TCP connection to the PRO server: login (MAC, nickname, game product code), then
  create/connect/scan/disconnect a group; the server pushes peer join/leave notices with each
  peer's IP. The game sees adhocctl handler events (CONNECT, SCAN, DISCONNECT, ...).
- **PDP** (datagrams) = UDP and **PTP** (streams) = TCP sent **directly between peers' IPs**; the
  PSP port number plus a configurable *port offset* (PPSSPP default 10000) gives the host port.
  A later phase can add PPSSPP's server relay (`aemu_postoffice`) for players behind NAT.
- **Matching** (lobby protocol on top of PDP): hello/join/accept/cancel/bye messages between peers
  with callbacks to the game. The packet formats must match PPSSPP's so mixed sessions work.
- **Netconf** utility dialog (`sceUtilityNetconf*`, adhoc connect/create/game-mode): a system
  dialog games use to join a group; PPSSPP drives adhocctl from it.

## Architecture

New runtime unit `runtime/src/net/` (generic: no Patapon addresses; per-game nothing expected).

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `adhoc_config` (pure) | server host/port, nickname, MAC (generated once, persisted next to the saves), port offset, product code | env, savedata dir |
| `pro_protocol` (pure) | encode/decode PRO server packets (login, connect, scan, disconnect, chat, peer notifications), byte-exact with PPSSPP `proAdhoc.h` | — |
| `adhoc_server_client` | TCP connection to the server on a host thread: login, keepalive, group ops, peer table (MAC→IP), event queue | pro_protocol, host sockets |
| `adhocctl_hle` | `sceNetAdhocctl*`: state machine, handler registry, events delivered to guest handlers | server client, guest-callback runner |
| `pdp` / `ptp` | `sceNetAdhocPdp*` / `sceNetAdhocPtp*` over host UDP/TCP sockets; blocking calls release the guest CPU lock while waiting | peer table, port offset |
| `matching` | `sceNetAdhocMatching*`: PPSSPP-compatible message formats and state machine over PDP; callbacks to the game | pdp, guest-callback runner |
| `netconf_dialog` | `sceUtilityNetconf*` ad hoc modes: drives adhocctl, status machine like the other utility dialogs (no UI needed beyond what the game draws) | adhocctl |
| `guest_callback_runner` | runs guest handler functions on a dedicated runtime-created PSP thread (own stack, `psp_thread_create`), so game callbacks execute with the CPU lock like any guest code | scheduler |
| `tools/adhoc_server.py` | minimal PRO ad hoc server for tests (login/connect/scan/peers) | Python stdlib |

Existing stubs (`sceNetInit`, `sceWlanGetEtherAddr`, `sceWlanGetSwitchState`, …) move into this
unit and return real values (MAC from config, switch on).

Pure units (`adhoc_config`, `pro_protocol`, matching message codec, port mapping) get standalone
ctest tests. Socket units get loopback tests (two endpoints in one test process). Guest-facing
behaviour is verified with the game.

## Phases (each ends with a verifiable state)

0. **Probe** — with HLE trace on, enter the DxD "Ad-hoc / Multi" menu in the port and record the
   exact net/utility call sequence Patapon 3 makes (which of the imported functions it really uses,
   with which modes). This fixes the subset and order of the next phases.
1. **Control plane** — config, PRO protocol codec, server client, adhocctl, net/wlan basics.
   Done when: two port instances against the local server both reach adhocctl CONNECTED in the
   same group and each lists the other in `sceNetAdhocctlGetPeerList`.
2. **PDP + matching** — done when the two instances see each other in the game's ad hoc lobby.
3. **PTP + whatever the game uses for mission sync** — done when two instances play a mission.
4. **Interop** — the port and PPSSPP (same server) see each other and play.
5. **Internet** — two PCs through a public server; relay if NAT blocks P2P.

## Error handling

- Server unreachable / dropped: adhocctl reports the PSP error codes PPSSPP uses (connection
  failure → disconnected event); the game shows its own "connection lost" flow. Never block the
  game thread indefinitely: every wait has a timeout and releases the CPU lock.
- Networking unconfigured: behave like a PSP with Wi-Fi off? **[assumption]** No — keep the switch
  "on" and let connection attempts fail fast against an unreachable server, so the game's own
  error path runs (closest to PPSSPP with no server).
- Logs: one `[ADHOC]` line per state change and per server packet type (rate-limited), so a
  session can be diagnosed from `jugar.log`.

## Testing

- Unit: PRO codec round-trips against byte layouts copied from PPSSPP; matching message codec;
  config persistence; port mapping.
- Loopback: PDP send/recv and PTP connect/accept/send/recv between two sockets in one process.
- Integration: `tools/adhoc_server.py` + two port instances (distinct MAC and port offset via
  env), scripted to the ad hoc menu with existing debug-socket tools; then the user's PPSSPP.

## Later (not in this design)

- Infrastructure mode: first measure what the game sends in infrastructure mode (it imports
  sceNetInet/Http/Ssl/Np) to judge feasibility; reviving it needs a replacement game server.
- A settings UI for server/nickname; pointing at patapon.net once agreed with Madwig.
