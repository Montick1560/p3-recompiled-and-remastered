# VS search in infrastructure mode (a player in two rooms) — design

## Goal
Two parties, each together in its own hideout over infrastructure mode, pick VS and find each other: the game's VS
matchmaking runs as it did on PSN, and the VS starts.

## Evidence (23 Oct 2026, the user's instance `build/e2e_portable3/logs/game.log`, server log screenshot)
- Montick hosts a hideout room (CreateJoinRoom 0x0101 ok, room ...773, a visitor joins as 0x11).
- On VS, the game first calls SetRoomDataInternal on the hideout room with flagFilter `0x54000000`, flagAttr
  `0x14000000`: it sets HIDDEN (`0x10000000`) and NAT_TYPE_RESTRICTION (`0x04000000`) and clears CLOSED
  (`0x40000000`) (RPCS3 `sceNp2.h`, `SCE_NP_MATCHING2_ROOM_FLAG_ATTR_*`). The runtime logs it as "not sent".
- Then SearchRoom with 7, 5 and 4 int filters (relaxed each time), 0 rooms, then CreateJoinRoom for a VS room
  **while still in the hideout room**. The server answers `0x80550D30` (ALREADY_JOINED): `op_create` and `op_join`
  allow one room per connection. The game puts the flags back (`0x04000000`) and loops: about 10 searches a second
  and one create every few seconds, forever.
- So on PSN a member could be in its hideout room and a VS room at once. The runtime (`psp_hle_np_matching2.cpp`
  `State`: one `room_id`, `me`, `peers`, `room_cb`) and the client (`infra_client` `room_id_`) also hold one room.

## Design
1. **Server: a connection is a member of up to 4 rooms.**
   - `Conn.rooms` (id -> Room) replaces `Conn.room`. Create/join refuse only a second membership in the *same* room
     (ALREADY_JOINED) or a fifth room (REQUEST_OVERFLOW-like: `SERVICE_UNAVAILABLE`).
   - leave/kick/set_internal name their room; a dropped connection leaves every room (each with its own events).
   - Datagrams and stream segments pass between two connections that share at least one room.
   - `bound` resolves signaling in every room of the connection.
2. **Room flags, PS3 semantics.**
   - `set_internal` takes `flag_filter` and `flag_attr`: `room.flag = (room.flag & ~filter) | (attr & filter)`.
   - Search takes `flag_filter` / `flag_attr` too: a room matches when `(room.flag & filter) == (attr & filter)`.
     A room with HIDDEN is never returned by a search. A room with CLOSED or FULL refuses a join with ROOM_FULL.
3. **Runtime: rooms by id.** The Matching2 state keeps every room the game is in (id -> `me`, peers, room event
   callback). Events, signaling callbacks and SignalingGetConnectionStatus use the room the event or call names.
   LeaveRoom drops only that room. The client tracks a set of joined rooms and, when the connection is lost, sends
   `room_destroyed` for each.
4. **Runtime: flags reach the server.** SetRoomDataInternal sends `flag_filter` / `flag_attr`; SearchRoom sends the
   request's flagFilter (@0x18) and flagAttr (@0x1C).
5. **Proof: two parties on one PC.** Four test instances (A+B one party, C+D the other) over the local server; both
   hosts pick VS; they find each other and the VS starts on all four (screenshots). What the game does after the
   rooms meet is not known yet: the e2e records every request with `PSPRECOMP_INFRA_TRACE` before anything else
   is changed, and each further gap gets its own fix with evidence.

## Acceptance
- One party picking VS: no ALREADY_JOINED, the host ends up in its hideout room and a VS room, and searches stop
  looping (the game waits for an opponent).
- Two parties: the VS starts on the four instances.
- Hideout co-op, the co-op mission e2e, ad hoc and identity e2es still pass.

## Out of scope
Changing the game. Matchmaking quality (ranking, regions). More than two parties at once.

## Result (23 Oct 2026)
- Server: up to 4 rooms per connection and room flags (964991b); runtime: `np2::Rooms` by room id, flags sent
  (7013744). No further gap was found in the e2e: no fix beyond these two commits.
- One party (build/infra/vs1): the host hides its hideout (`0x54000000`/`0x14000000`), searches with 7 int filters,
  creates the VS room while in the hideout room (server: `create` ok, runtime `in 2 room(s)`), no ALREADY_JOINED,
  the search stops and both show "Buscando oponente.".
- Two parties, four instances (build/infra/vs2): the second host's 7-filter searches miss (they require attr
  `0x52 == 2`, which the VS room never sets), the game relaxes to 5 filters, finds the first host's VS room and
  joins it; its visitor and the first host's visitor join it too (4/4). All four show "Info. oponente de enfrent."
  (blue P3A + P3B, red P3C + P3D) and the VS map with the same clock.
- Without input, the three non-owners leave after 60 s ("Se ha agotado el tiempo de conexion"): the game's idle
  rule, as in co-op. With drums (build/infra/vs5, 1v1) the match runs to 0:00, DERROTA / VICTORIA, rewards, and both
  return to their hideouts, which become visible again (`0x04000000`).
- Regressions: hideout co-op + mission (0 datagrams lost), ad hoc smoke, identity ("2.Edson"); ctest 106/106; 30 fps
  RT_REGRESS PASS; server selftest OK. Hand-off: build/autopilot/EN_QUE_NOS_QUEDAMOS_2026-10-23b.md.
