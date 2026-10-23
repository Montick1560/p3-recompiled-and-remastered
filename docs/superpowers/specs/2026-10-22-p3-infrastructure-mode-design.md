# Patapon 3 infrastructure mode (online rooms without Sony) — design

Date: 2026-10-22. Status: recon done (local commit df1d73f on branch `infra-recon`), implementation planned in
`docs/superpowers/plans/2026-10-22-p3-infrastructure-mode.md`.

## Goal

The game's own "Modo Infraestructura" works again in the port: a player opens a hideout ("¿Para qué quieres abrir
un escondite?", level, comment), other players find it in the game's own room list ("¿Para qué quieres buscar un
escondite?" → "Seleccionar Escondite donde entrar": online id, hero, comment, n/4), join it, play together, and the
signal meter under each hero in the team totem shows the real connection quality. No Sony server is involved: the
runtime answers the PSN/NP library calls itself and talks to our own server (`tools/infra_server.py`), which later
can run next to patapon.net.

User request (2026-10-22): "Escribe el plan, vamos con todo con este feature" after the recon showed the room list;
earlier: the per-player signal meter of the infrastructure mode ("aparece debajo del heroe en el totem de equipo,
va disminuyendo a menor señal") and the lobby selector of "Para charlar y matar el tiempo".

## What the game does (recon evidence, build/infra/notes.md)

1. `sceNetApctlAddHandler`, `sceNetUpnpInit`, `sceNpInit`, `sceNpAuthInit`, `sceNpServiceInit`.
2. `sceUtilityNetconf` action 0 (CONNECT_APNET). Today the port finishes it with ABORT, so the game silently goes back
   to the hideout. It must finish with result 0.
3. `sceUtilityNpSignin` dialog (status 0 is accepted), `sceNetApctlGetInfo(8 = IP)`, `sceNpGetContentRatingFlag`,
   `sceGameUpdate*` (update check; no-op is fine).
4. "Autentificando permiso de uso del servidor de búsqueda": `sceNpAuthCreateStartRequest` (service
   `EP9000-UCES01421_00`) → ticket callback `(reqId, 248, arg)` → `sceNpAuthGetTicket` (dummy X-I-5 v2.1 ticket,
   PPSSPP layout) → `sceNpAuthGetEntitlementById("EP9000-UCES01421_00-DLL001")` → 1 → Abort/Destroy.
5. "Conectando con servidor de búsqueda": `sceNpMatching2Init(0x14000, 0x26, 0x3800, 100)`, `sceNetUpnpStart`,
   `CreateContext("NPWR01755", passphrase, u16* ctxId, 1)`, `ContextStart`, `GetServerIdListLocal`,
   `GetServerInfo`, `GetWorldInfoList`, `RegisterSignalingCallback(ctx, 0x089E5698, arg)`.
6. Menus, then `SearchRoom` / `GetRoomDataExternalList` (search) or `CreateJoinRoom` (host), `JoinRoom` (visitor).
7. After the room exists the game opens its own socket: `sceNetInetSocket(2, 6, 0)`, `Setsockopt(SOL_SOCKET 0xFFFF,
   0x1000 / 0x2000, 4 bytes)`, `Bind`, then polls `sceNetInetRecvfrom(len 0x574)`. Peer addresses come from
   signaling: on event 0x5102 the game calls `SignalingGetConnectionStatus(ctx, roomId, memberId, &status,
   &in_addr, &port)` and stores the peer's address (port in network order).

## Matching2 on the PSP

The PSP library takes the PS3 `SceNpMatching2*` structs (RPCS3 `rpcs3/Emu/Cell/Modules/sceNp2.h`, copy in
`build/infra/ref_rpcs3/`), little-endian, 32-bit pointers, lists linked through a leading `next` pointer. Differences
from PS3, all checked against the game's handlers:

- **Request callback** `cb(ctxId, reqId, event, errorCode, data*, arg)` (a0..t1): the response data is passed
  directly (the PSP has no `GetEventData`). `optParam = {cbFunc, cbArg, timeout_us, u16 appReqId}`. The game finds
  the request by the reqId we wrote to `*assignedReqId` before posting the callback.
- **Room event callback** (`CreateJoinRoom`/`JoinRoom` 4th arg `{cb 0x089E6F48, arg, 1, 1, 0, 0}`):
  `cb(ctxId, -, roomId lo, roomId hi, u16 event, data*, arg)` (a0..t2; the u64 takes an even register pair).
- **Signaling callback** (0x089E5698): `cb(ctxId, -, roomId lo, roomId hi, memberId, u16 event, errorCode, arg)`
  (a0..t3).
- **Event numbers**: 0x0001 GetServerInfo, 0x0002 GetWorldInfoList, **0x0007 GetRoomDataExternalList** (PS3: 0x0005;
  the search UI callback in the mission overlay at 0x08BA7D68 accepts only 7 and 0x106), 0x0101 CreateJoinRoom,
  0x0102 JoinRoom, 0x0103 LeaveRoom, 0x0106 SearchRoom, 0x0109 SetRoomDataInternal, 0x0105 KickoutRoomMember (the
  room handler 0x089E6158 has no 0x105 branch: it only frees the request slot). Room events 0x1101 joined,
  0x1102 left, 0x1103 kicked out (data `optData[15] == 2`), 0x1104 destroyed. Signaling 0x5101 dead,
  0x5102 established, 0x5103 netinfo.
- `RoomDataExternal`: the game reads the member count as a **u32 at +0x24** (PS3: u16 openPrivateSlotNum,
  u16 curMemberNum).
- Error codes are in the PSP NP range 0x8055xxxx (the game compares 0x80550D30, 0x80550C0A, ...). Server-side
  errors use `0x80550D00 | (PS3 server error & 0xFF)`, e.g. ROOM_FULL 0x80550D19, NO_SUCH_ROOM 0x80550D13; client
  timeouts 0x80550C3C. To be confirmed by the message the game shows.

Room data the game publishes (host, purpose "charlar", no level filter): maxSlot 4, flagAttr 0x04000000, searchable
int attrs 0x4C=0, 0x4D=0x00030005 (5 = purpose), 0x4E=1, 0x4F=1, 0x50=40, 0x51=1, 0x52=1; external bin attrs
0x55 (94 bytes: UTF-16 hero name at 0, comment at 0x20) and 0x56 (80 bytes: online id). The search sends 7 int
filters (operators EQ 1, NE 2, LT 3, LE 4, GT 5, GE 6): 4C == 0, 4D == 0x30005, 4E != 0, 4F <= 1, 50 >= 1,
51 >= 1, 52 <= 40, range {startIndex 1, max 20}, and asks for bin attrs 0x55/0x56 in `GetRoomDataExternalList`.

## Design

### Server: `tools/infra_server.py`
Python 3 stdlib, one process. Two listeners speaking the same frames: raw TCP (default 27320, LAN and tests) and
WebSocket (default 27321, path `/infra`, one frame per binary WS message) for the public deployment behind a
**Cloudflare Tunnel**: `cloudflared` on the host maps `https://online.montick.app` to `http://localhost:27321`, so
the home PC opens no port and its IP stays hidden (the Cloudflare proxy does not carry raw TCP on the free plan).
Moving to a cloud machine later = moving the tunnel; the address the players use does not change.
Clients log in with their online id and get a session id and a **virtual IP** (10.80.0.1, 10.80.0.2, ...). The
server keeps rooms (id, owner, max slots, int/bin attrs, members with member ids from 0x10 up, their bound game port)
and implements: login, ping, search (filters + range), room details, create, join (ROOM_FULL / NO_SUCH_ROOM), leave
(owner leaving destroys the room), kickout, set room data internal, and pushes room events and signaling events to
the members. It forwards game datagrams between virtual IPs. A client that disconnects leaves its room.
`--selftest` covers it without the game.

Exposed to the internet, so it is defensive: frames over 64 KiB close the connection; online ids are 3-16 chars
`[A-Za-z0-9_-]`; at most 4 connections per client address (the `CF-Connecting-IP` header behind the tunnel), one
room per session, 16 slots max, 64 rooms in total by default (`--max-rooms`); datagrams over 400/s per session are
dropped; 30 s without any frame closes the session. It never executes or stores anything from clients on disk.
It counts bytes per session and prints a traffic summary every 60 s (to size the 40 Mb/s home upload).

### Wire format
Frames `u32 length (LE, of kind + body) | u8 kind | body`; over WebSocket each binary message carries exactly one
frame. Kind 1: UTF-8 JSON object (requests carry `"id"`, answers echo it; events have `"ev"`); binary attribute
data is hex. Kind 2: datagram `u32 vip | u16 dst_port | u16 src_port | payload` (to the server: destination vip;
from the server: source vip).

### Runtime
- `net/infra_protocol` (pure): frame codec, JSON helpers.
- `net/infra_client`: one connection to `PSPRECOMP_INFRA_SERVER`, request/answer with timeout, event queue,
  datagram queues per local port, RTT from pings. Everything off the guest CPU (like the ad hoc layer). Address
  forms: `host[:port]` or `tcp://host[:port]` (raw TCP, all platforms), `ws://host[:port]/path` and
  `wss://host/path` (WebSocket through WinHTTP on Windows: TLS and the WS handshake come from the OS, no new
  library; on other platforms `wss://` is reported as unsupported). Default `wss://online.montick.app/infra`;
  `PSPRECOMP_INFRA_SERVER=off` disables infrastructure mode.
- `net/np2_structs` (pure): writes the Matching2 response structs into guest memory at a given base (offsets above).
- `hle/psp_hle_np.cpp` (sceNp/Auth/Service/Apctl) and `hle/psp_hle_np_matching2.cpp` (Matching2 over the client,
  callbacks through `guest_callbacks`, now 8 arguments).
- `hle/psp_hle_inet.cpp`: sceNetInet UDP sockets (types 2 and 6) on the virtual network: `Socket`, `Setsockopt`
  (accepted), `Bind` (port reported to the server), `Sendto`, `Recvfrom` (nonblocking: -1 and errno EAGAIN 11,
  `GetPspError` 0x8001000B), `Close`, `SocketAbort`. `sceNetApctlGetInfo(IP)` returns the virtual IP.
- Signal meter: `sceNetApctlGetInfo(5 = STRENGTH)` returns a percentage from the measured RTT.
- Infrastructure mode is reached only when the player picks "Modo Infraestructura" in the game; the runtime then
  connects to the configured server (default above). With `PSPRECOMP_INFRA_SERVER=off` the netconf dialog aborts
  as today. Nothing changes for ad hoc or offline play. The recon flag `PSPRECOMP_INFRA_RECON` goes away.

## Traffic (measured 2026-10-22, two instances, build/infra/e2e8 and e2e14)

The server's `traffic:` lines (all sessions, kB = 1024 bytes) for one 2-player room:

| Phase | Server upload | Server download |
|---|---|---|
| Hideout together (TCP P2P state sync) | 19.8-22.3 kB/s | the same |
| Mission (UDP P2P, ~5 datagrams/s each way) | 0.6-0.8 kB/s | the same |

Per player the hideout traffic is asymmetric: over a whole 8-minute session the host sent 4.1 MB and the visitor
1.0 MB (e2e8), so in the hideout the host uploads about 17-18 kB/s per visitor and a visitor about 4-5 kB/s.

Estimate for 4 players (3 visitors): host-centric (what the game's sockets show: every visitor connects to the
host's listener) about 3 x 22 = 66 kB/s of server upload per room in the hideout; if visitors also exchanged state
with each other, up to 6 x 22 = 132 kB/s. Missions stay near 1 kB/s per player pair.

40 Mb/s of home upload is about 4880 kB/s; with 30 % headroom about 3400 kB/s usable: roughly 50 full rooms all in
their hideouts at once (25 in the worst case above), and far more when rooms are in missions.

Observed game rule: a visitor who gives no command for about 60 s in a mission leaves with "Se ha agotado el tiempo
de conexión" (seen in four runs; with the visitor drumming for 2+ minutes and the host idle nobody left, and the
network traffic was normal up to the visitor's own Close). The host is not affected.

## Signal meter (recon 2026-10-22, plan Task 8)

The meter under each hero (team totem, hideout) shows full bars in every session so far, and it does not follow
the connection: the game calls `sceNetApctlGetInfo` only with code 8 (IP, at connection time), never 5
(STRENGTH); its signaling callback ignores 0x5103 (netinfo); the overlays import no network function; and the
bars stayed full with the server adding 300 ms per hop (`--delay-ms 300`, e2e15), dropping 60 % of the game
datagrams (`--loss 60`, e2e16) and with the members' NAT type set to 3 (e2e17). `sceNetApctlGetInfo(5)` still
answers from the measured server RTT (`net/infra_signal.h`: <=80 ms 99 %, <=150 75 %, <=250 50 %, <=400 25 %,
else 5 %) for any code path that asks. Where the game takes the meter's value from is still open (candidates: a
field of the hideout state the host sends over TCP, or a constant in infrastructure mode).

## Out of scope (later)
Launcher setting for the server address (env only for now), hosting next to patapon.net, TCP sockets (`sceNetInetConnect/Listen`)
unless the game turns out to use them in a session (checked in the plan), chat via `sceNpMatching2SendRoomChatMessage`
(not imported), ID-online search by friend id beyond what the game sends.
