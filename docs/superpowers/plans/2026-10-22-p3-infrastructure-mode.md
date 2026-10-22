# Patapon 3 infrastructure mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The game's own infrastructure mode works in the port against our server: open a hideout, find it in the game's room list, join it, play together, signal meter under each hero.

**Architecture:** The runtime answers the PSN/NP imports itself (sign-in, dummy ticket) and implements `sceNpMatching2` over a TCP client to `tools/infra_server.py`, which keeps the rooms and pushes room/signaling events. Response structs are the PS3 Matching2 structs, little-endian, written into guest memory by pure writers and handed to the game's callbacks through `guest_callbacks`. The game's own UDP socket (`sceNetInet`) runs on a virtual network: every client gets a virtual IP from the server, which forwards datagrams.

**Tech Stack:** C++20 runtime (`psprecomp/runtime`), nlohmann/json (vendored), ninja + ctest in `build/rtw`, Python 3 stdlib server in `psprecomp/tools/`, e2e drivers in `build/infra/` (outside the repo), debug ports 9997/9998.

**Spec:** `docs/superpowers/specs/2026-10-22-p3-infrastructure-mode-design.md` (recon evidence: `build/infra/notes.md`, RPCS3 struct reference: `build/infra/ref_rpcs3/sceNp2.h`, game decompiles: `build/infra/m2_module.c`, `build/infra/ovm_search.c`).

## Global Constraints

- Branch `infra-mode` in `psprecomp/`, created from `infra-recon` (keeps df1d73f; Task 9 removes the recon-only parts). Commits authored as configured ("Patapon Community"), **no Claude co-author trailer**, message ends with `Verified: ...`. Nothing is pushed without the user's OK (next push branch `push-p3-32`, via the push-p3 worktree as usual).
- Infrastructure is enabled only by `PSPRECOMP_INFRA_SERVER=host[:port]` (default port 27320). Without it the netconf dialog aborts infrastructure actions exactly as before, and nothing in the ad hoc path changes: 30 fps `rt_regress.py` PASS and ctest green at the end of every task that touches the runtime.
- Never touch the user's game on port 9999; test instances on 9997/9998, killed by path (`*\build\rtw\*`), never by image name. Save copies only (never `DECO/savedata*` or `build/save_main/`).
- Struct offsets and event numbers come from the spec (PS3 layout, PSP differences listed there). An event number that is not in the spec is confirmed against the game's handler before it is used.
- The guest CPU is never held while waiting on the network: blocking work runs on client threads, results reach the game through `guest_callbacks_post` or polled queues (same rule as `runtime/src/net/adhocctl.cpp`).
- Docs in the same change: `docs/ENV_FLAGS.md` (new env vars), `ARCHITECTURE.md` (new net/HLE units), `README.md` (capability) — Task 9 at the latest.

## Review Focus

1. **Server gone mid-session** (killed or unreachable): every pending request answers with an error (0x80550C3C timeout) instead of hanging the game on "Conectando..."; members in a room get 0x1104 RoomDestroyed. Test in Task 3 (client) and Task 5 (e2e: kill the server while in a room).
2. **Room full / room gone**: a 5th joiner gets ROOM_FULL, joining a room that was just closed gets NO_SUCH_ROOM, and the game shows a message and returns to the list. Test in Task 1 (server) and Task 5.
3. **Purpose and level filters**: a room opened "para enfrentamientos" must not appear in a "charlar" search, and level ranges are applied with the game's operators. Test in Task 1.
4. **Host leaves / client crashes**: the server removes the member and tells the others (0x1102, or 0x1104 when the owner leaves); no ghost rooms in later searches. Test in Task 1 and Task 5.
5. **Ad hoc and offline play unchanged** when `PSPRECOMP_INFRA_SERVER` is unset, including the 8-argument guest callbacks: ctest (`hle_net`, `guest_callback_queue`) and 30 fps `rt_regress` in Task 5 and Task 9.

---

### Task 1: Room server (`tools/infra_server.py`)

**Files:**
- Create: `tools/infra_server.py`
- Test: `python -I tools/infra_server.py --selftest`

**Interfaces:**
- Produces: the wire protocol of the spec. Frames `<I length><B kind>body`; kind 1 JSON, kind 2 datagram `<I vip><H dst_port><H src_port>payload`.
  Requests (client → server, JSON with `"id"`, answer `{"id", "ok": true, ...}` or `{"id", "ok": false, "error": <u32>}`):
  - `{"op":"login","online_id"}` → `{"session", "vip"}` (vip as u32, 10.80.0.n in network byte order meaning `0x0A500000 + n`)
  - `{"op":"ping","t"}` → `{"t"}`
  - `{"op":"search","world","start","max","ints":[[op,id,value]...],"attrs":[id...]}` → `{"total","rooms":[room_ext...]}`
  - `{"op":"details","room_ids":[...],"attrs":[...]}` → `{"rooms":[room_ext...]}`
  - `{"op":"create","max_slot","flag_attr","ints":[[id,value]...],"bins":[[id,hex]...],"bins_internal":[...],"member_bins":[...]}` → `{"room": room_int}`
  - `{"op":"join","room_id","member_bins":[...],"opt":hex}` → `{"room": room_int}` or error ROOM_FULL 0x80550D19 / NO_SUCH_ROOM 0x80550D13
  - `{"op":"leave","room_id","opt":hex}`, `{"op":"kick","room_id","member_id","opt":hex}`, `{"op":"set_internal","room_id","bins":[...],"flag_attr"}` → `{}`
  - `{"op":"bound","port"}` (the client's game socket port) → `{}`
  Events (server → client, JSON with `"ev"`): `member_joined` / `member_left` / `kicked` / `room_destroyed` `{room_id, member, opt}`, `signaling` `{room_id, member_id, event: 0x5102|0x5101}`.
  `room_ext` = `{id, owner, max_slot, members, ints, bins}`; `room_int` = `{id, max_slot, flag_attr, members:[member], me, owner, bins_internal}`; `member` = `{member_id, online_id, vip, port, join_ms, bins}`.

- [ ] **Step 1: Write the selftest first** (`--selftest` runs the server on an ephemeral port in a thread and drives raw sockets):
  - `login` twice → vips `0x0A500001`, `0x0A500002`, distinct sessions.
  - A creates with ints `[[0x4C,0],[0x4D,0x30005],[0x4E,1],[0x4F,1],[0x50,40],[0x51,1],[0x52,1]]` and bins `0x55`/`0x56`; B searches with the game's filters from the spec → 1 room with owner `A`, `members == 1`, both bins; B searches with `4D == 0x30003` → 0 rooms; with `50 >= 41` → 0 rooms.
  - B joins → B gets `room_int` with 2 members (owner member id 0x10, B 0x11, `me` = 0x11); A receives `member_joined` for 0x11 and both receive `signaling 0x5102` for the other member.
  - Datagram A → B's vip/port arrives at B with source A's vip and A's bound port.
  - Three more joins: the 4th member succeeds, the 5th gets `0x80550D19`; join of an unknown id → `0x80550D13`.
  - B disconnects → A gets `member_left` (0x11); A leaves → room gone from the next search; a joined C gets `room_destroyed`.
- [ ] **Step 2: Run it, expect failure** — `python -I tools/infra_server.py --selftest` → fails (file missing).
- [ ] **Step 3: Implement** with `selectors` like `tools/adhoc_server.py` (same logging style, `--host`, `--port 27320`, `--delay-ms N` adds latency to datagram forwarding for the signal-meter test in Task 8). Filter operators: EQ 1, NE 2, LT 3, LE 4, GT 5, GE 6 on the room's searchable int attrs; `range.start` is 1-based.
- [ ] **Step 4: Run it** — `python -I tools/infra_server.py --selftest` → `selftest OK`.
- [ ] **Step 5: Commit** — `git add tools/infra_server.py && git commit` ("feat(net): infrastructure-mode room server ...", `Verified: infra_server --selftest`).

### Task 2: Wire codec (`net/infra_protocol`)

**Files:**
- Create: `runtime/include/net/infra_protocol.h`, `runtime/src/net/infra_protocol.cpp`
- Modify: `runtime/CMakeLists.txt` (runtime sources + `test_infra_protocol`)
- Test: `runtime/tests/test_infra_protocol.cpp`

**Interfaces:**
- Produces (namespace `infra`):
  - `std::vector<uint8_t> encode_json(const nlohmann::json& j);`
  - `std::vector<uint8_t> encode_datagram(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t* data, size_t len);`
  - `class FrameReader { void feed(const uint8_t*, size_t); bool next(Frame* out); };` with `struct Frame { uint8_t kind; nlohmann::json json; uint32_t vip; uint16_t dst_port, src_port; std::vector<uint8_t> payload; };` (a malformed JSON frame is skipped and counted, never thrown).
  - `std::string to_hex(const uint8_t*, size_t);` `std::vector<uint8_t> from_hex(const std::string&);`

- [ ] **Step 1: Failing tests** `json_roundtrip_split_feed` (a frame fed one byte at a time comes out once, equal), `datagram_roundtrip` (vip 0x0A500002, ports 3658/3658, 1396-byte payload), `two_frames_one_feed`, `bad_json_skipped` (next frame still decodes), `hex_roundtrip` (`"00ff10"`).
- [ ] **Step 2:** `ninja test_infra_protocol && ctest -R infra_protocol` → build fails (missing source).
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** same command → PASS; `ctest` all green.
- [ ] **Step 5: Commit** (`Verified: ctest infra_protocol + full ctest`).

### Task 3: Client (`net/infra_client`)

**Files:**
- Create: `runtime/include/net/infra_client.h`, `runtime/src/net/infra_client.cpp`
- Modify: `runtime/CMakeLists.txt`
- Test: `runtime/tests/test_infra_client.cpp` (an in-process loopback fake server thread speaking Task 2 frames)

**Interfaces:**
- Consumes: Task 2 codec.
- Produces (namespace `infra`):
  - `Client& client();` (process singleton) ; `bool Client::configured() const;` (`PSPRECOMP_INFRA_SERVER` set)
  - `bool Client::connect(const std::string& online_id, int timeout_ms);` (TCP + login; stores `vip()`, `session()`)
  - `void Client::request(nlohmann::json req, std::function<void(const nlohmann::json& answer)> done, int timeout_ms);` — `done` runs on the client thread exactly once: the server's answer, or `{"ok":false,"error":0x80550C3C}` on timeout / disconnect.
  - `void Client::on_event(std::function<void(const nlohmann::json&)> fn);`
  - `void Client::send_datagram(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t*, size_t);`
  - `bool Client::pop_datagram(uint16_t local_port, Datagram* out);` (`Datagram {vip, src_port, payload}`), queue capped at 256 per port (oldest dropped).
  - `int Client::rtt_ms() const;` (median of the last 5 pings, one every 2 s; -1 before the first answer)
  - `uint32_t Client::vip() const;`

- [ ] **Step 1: Failing tests**: `login_sets_vip`, `request_answer_matched_by_id` (two requests answered out of order), `request_times_out` (fake server never answers → `error == 0x80550C3C` within timeout + 200 ms), `disconnect_fails_pending` (fake server closes → pending `done` gets the error, `on_event` receives `{"ev":"room_destroyed"}` synthesised for a joined room), `datagram_queue_per_port`, `rtt_from_ping`.
- [ ] **Step 2:** `ninja test_infra_client && ctest -R infra_client` → fails.
- [ ] **Step 3: Implement** (one reader thread, a mutex for the pending map; sockets like `relay_conn.cpp`).
- [ ] **Step 4:** PASS; full ctest green.
- [ ] **Step 5: Commit.**

### Task 4: Matching2 struct writers (`net/np2_structs`)

**Files:**
- Create: `runtime/include/net/np2_structs.h`, `runtime/src/net/np2_structs.cpp`
- Modify: `runtime/CMakeLists.txt`
- Test: `runtime/tests/test_np2_structs.cpp`

**Interfaces:**
- Produces (namespace `np2`), all writing into a `GuestBuf { uint8_t* base_host; uint32_t base_guest; uint32_t size; uint32_t used; uint32_t alloc(uint32_t n); }` (zeroed, 4-aligned; `alloc` returns 0 when full):
  - `uint32_t write_server_info(GuestBuf&, uint16_t server_id, uint8_t status);`
  - `uint32_t write_world_list(GuestBuf&, uint32_t world_id, uint32_t rooms, uint32_t members);` → `{World* list, u32 n}`
  - `uint32_t write_room_ext(GuestBuf&, const nlohmann::json& room_ext, uint32_t* out_entry);` (one `RoomDataExternal`, 0x58)
  - `uint32_t write_search_response(GuestBuf&, uint32_t start, uint32_t total, const nlohmann::json& rooms);`
  - `uint32_t write_ext_list_response(GuestBuf&, const nlohmann::json& rooms);` → `{RoomDataExternal* list, u32 n}`
  - `uint32_t write_room_int_response(GuestBuf&, const nlohmann::json& room_int);` → `{RoomDataInternal*}`
  - `uint32_t write_member_update(GuestBuf&, const nlohmann::json& member, uint8_t cause, const std::vector<uint8_t>& opt);` → `RoomMemberUpdateInfo`

- [ ] **Step 1: Failing tests** (assert bytes at spec offsets): `room_ext_offsets` (roomId @0x18, maxSlot u16 @0x22, **member count u32 @0x24**, owner `SceNpUserInfo2*` @0x30 with online id at +0 and name pointer at +36, int attrs @0x40/0x44, bin attrs @0x50/0x54 with `{u16 id, ptr, size}`), `room_int_offsets` (roomId @0x10, maxSlot @0x20, members @0x24, num @0x28, me @0x2C, owner @0x30; member: online id @4, name ptr @0x28, memberId u16 @0x38, list linked through +0), `search_response_layout` ({1, total, n, list}), `member_update_layout` (member @0, cause u8 @4, opt[16] @8, len 16 @0x18), `buffer_full_returns_zero`.
- [ ] **Step 2:** `ninja test_np2_structs && ctest -R np2_structs` → fails.
- [ ] **Step 3: Implement** (move the recon writers out of `psp_hle_np_matching2.cpp`).
- [ ] **Step 4:** PASS; full ctest green.
- [ ] **Step 5: Commit.**

### Task 5: Matching2 over the server — two instances meet in a room

**Files:**
- Modify: `runtime/src/hle/psp_hle_np_matching2.cpp` (rewrite on Tasks 3–4), `runtime/src/hle/psp_hle_np.cpp` (online id = `PSPRECOMP_ADHOC_NICK` or "Patapon"; `sceNetApctlGetInfo(8)` returns the vip as dotted text), `runtime/src/hle/psp_hle_utility.cpp` + `runtime/src/net/netconf_adhoc.cpp` (`in.infra_ok = infra::client().configured()`), `runtime/include/net/netconf_adhoc.h`
- Create (outside the repo): `build/infra/two.py` (two instances A = 9997 host, B = 9998 visitor, each with `PSPRECOMP_INFRA_SERVER=127.0.0.1`, own save copy of `build/save_weather`, nick P3A/P3B; starts `tools/infra_server.py`; `cmd.txt` lines `<A|B|*> P <key> [wait]`, `<A|B|*> S <name>`, `W <s>`, `K` kills the server, `Q` quits; never port 9999)
- Test: `runtime/tests/test_netconf_adhoc.cpp` (existing test of the pure `netconf::step`; add `infra_action_ok_when_infra_ok` (action 0, `infra_ok = true` → finished, result 0) and `infra_action_aborts_without_infra` (→ result 2))

**Interfaces:**
- Consumes: Task 3 `infra::client()`, Task 4 writers, `guest_callbacks_post` (8 args).
- Produces: `void psp_hle_register_np_matching2();` unchanged; requests per the spec event table: GetServerIdListLocal (1 server, id 1) / GetServerInfo (0x0001, status 1) / GetWorldInfoList (0x0002) answered locally after `connect`; SearchRoom 0x0106; GetRoomDataExternalList **0x0007**; CreateJoinRoom 0x0101 (assigned req id at t1); JoinRoom 0x0102; LeaveRoom 0x0103; KickoutRoomMember 0x0105; SetRoomDataInternal 0x0109; server errors in the callback's `errorCode` with null data. Room events to the room-event callback saved from CreateJoinRoom/JoinRoom arg 4 (7 args: ctx, 0, roomId lo, roomId hi, event, data, arg); signaling events to the callback from `RegisterSignalingCallback` (8 args). `SignalingGetConnectionStatus(ctx, -, roomId lo, roomId hi, memberId, int* status, in_addr* addr, u16* port)` → status 2, the member's vip, its bound port in network order.

- [ ] **Step 1: Failing unit tests** for the netconf gate (above) → `ctest -R netconf` fails.
- [ ] **Step 2: Implement** the gate and the Matching2 rewrite; delete the file room store and the `PSPRECOMP_INFRA_ROOMS` code.
- [ ] **Step 3:** `ninja && ctest` → all green.
- [ ] **Step 4: e2e, room list:** `python -I build/infra/two.py build/infra/e2e5` — A hosts "charlar" (no level, comment), B searches "charlar". Expected: B's screenshot shows A's room with A's hero name, comment and `1/4` (compare with `build/infra/lista_escondites.png`); server log `create` then `search` with 1 room.
- [ ] **Step 5: e2e, join:** B selects the room. Expected: server log `join` ok; A's log `[NP2] room event 0x1101` and `signaling 0x5102`; B's log `[NP2] 0x0102 ok`; screenshots of both after 10 s (record what the game shows: the visitor entering the host hideout needs Task 6 traffic, so a timeout/"error de red" here is acceptable and noted, a hang is not).
- [ ] **Step 6: e2e, errors:** a search for "enfrentamientos" from B lists nothing; `K` (kill server) while B waits in a request → B leaves "Conectando..." with an error message within 35 s.
- [ ] **Step 7:** 30 fps `rt_regress.py` (no `PSPRECOMP_INFRA_SERVER`) → PASS.
- [ ] **Step 8: Commit** with the evidence paths.

### Task 6: Game sockets on the virtual network (`sceNetInet`)

**Files:**
- Create: `runtime/src/hle/psp_hle_inet.cpp`, `runtime/include/hle/psp_hle_inet.h`, `runtime/include/net/inet_errno.h` (pure: errno → PSP error)
- Modify: `runtime/src/hle/psp_hle_utility.cpp` (remove the `sceNetInetInit/Term` no-ops there, register from the new file), `runtime/src/hle/psp_hle_dispatch.cpp`, `runtime/CMakeLists.txt`
- Test: `runtime/tests/test_inet_errno.cpp`

**Interfaces:**
- Consumes: Task 3 `send_datagram` / `pop_datagram` / `vip()`, bound-port report (`{"op":"bound"}`).
- Produces: `sceNetInetInit/Term/Socket/Setsockopt/Bind/Sendto/Recvfrom/Close/SocketAbort/GetPspError`. Sockets are ids from 0 (the game gets 0 today and works with it). `Socket(2, 2|6, 0)` ok, other types → -1 EPROTONOSUPPORT logged. `SceNetInetSockaddrIn {u8 len, u8 family, u16 port (network order), u32 addr, u8 zero[8]}`. `Bind` port 0 → pick 3658 + n. `Recvfrom` with nothing queued → -1, errno EAGAIN (11) for that thread, `GetPspError` → 0x8001000B (`inet_errno_to_psp(11)`). `Sendto` to a non-virtual address → -1 ENETUNREACH. `sceNetInetConnect/Listen/Accept/Send/Recv` stay unimplemented but log every call with arguments (recon for TCP).
- `uint32_t inet_errno_to_psp(int psp_errno);` (EAGAIN 11 → 0x8001000B; the other values copied from PPSSPP `convertInetErrno2PSPError` in `tools/ppsspp-src/Core/HLE/NetInetConstants.cpp`).

- [ ] **Step 1: Failing test** `test_inet_errno` (EAGAIN → 0x8001000B, ENOTCONN and ECONNRESET → PPSSPP's values, unknown passes through) → fails.
- [ ] **Step 2: Implement;** `ninja && ctest` green.
- [ ] **Step 3: e2e, together in the hideout:** `two.py` as Task 5 Step 5, then wait 20 s. Expected: B appears in A's hideout and A's team totem lists both heroes (screenshots `together_A`, `together_B`); server log shows datagrams both ways; no `[NP2]`/`[INET]` errors. Note any TCP call logged.
- [ ] **Step 4: e2e, leave:** B chooses "Ir a mi escondite" → A gets 0x1102 and B returns home; A "Disolver" with C joined (third instance on 9996) → C gets 0x1104 and the game's message.
- [ ] **Step 5:** 30 fps `rt_regress.py` PASS. **Commit.**

### Task 7: Play a co-op mission online

**Files:** fixes only where the e2e shows them (each fix gets its own failing test first, in the file of the unit it touches).

- [ ] **Step 1: e2e:** A and B together (Task 6), A starts a multiplayer mission, both play 60 s with scripted drums on both, then A quits to the hideout. Expected: both load the same mission, both see the other's units, the mission ends or quits for both, no desync message. Record the session (`build/rec60/rec.py` per instance) for the user.
- [ ] **Step 2:** If the game uses TCP (Task 6 recon log) or another unimplemented import blocks the mission: write the failing test for the missing behaviour, implement it in the matching file (`psp_hle_inet.cpp` for sockets), re-run Step 1.
- [ ] **Step 3: Commit** with the recording path.

### Task 8: Signal meter from the real connection

**Files:**
- Modify: `runtime/src/hle/psp_hle_np.cpp` (`sceNetApctlGetInfo(5)`)
- Create: `runtime/include/net/infra_signal.h` (pure)
- Test: `runtime/tests/test_infra_signal.cpp`

**Interfaces:**
- Consumes: Task 3 `rtt_ms()`.
- Produces: `uint8_t strength_from_rtt(int rtt_ms);` — -1 (unknown) → 99; ≤ 80 ms → 99; ≤ 150 → 75; ≤ 250 → 50; ≤ 400 → 25; more → 5. (The game's own thresholds for its 4 bars are read in Step 1 and the table is adjusted so each bar count is reachable.)

- [ ] **Step 1: Recon:** in a Task 6 session, log every `sceNetApctlGetInfo` code and the caller `ra`; find where code 5 is read and how the game turns the % into bars (decompile the caller with `build/tools/ghidra/decomp.sh` or the mission-overlay project `build/ghidra_proj_mission`). Write the thresholds into the spec. If the game never asks for code 5 in our sessions, find what it shows under the hero instead (the meter may be the peer's value sent in the room data) and adapt this task to feed that value.
- [ ] **Step 2: Failing test** `strength_buckets` (one value per bar count from Step 1) → fails.
- [ ] **Step 3: Implement;** ctest green.
- [ ] **Step 4: e2e:** `infra_server.py --delay-ms 0` then `--delay-ms 300`: screenshots of the team totem show full bars, then fewer.
- [ ] **Step 5: Commit.**

### Task 9: Clean-up, docs, deploy

**Files:**
- Modify: `runtime/src/hle/psp_hle_dispatch.cpp` (drop `PSPRECOMP_INFRA_RECON` per-call logging), `runtime/src/hle/psp_hle_np*.cpp` (logs behind `PSPRECOMP_INFRA_TRACE=1`), `docs/ENV_FLAGS.md` (`PSPRECOMP_INFRA_SERVER`, `PSPRECOMP_INFRA_TRACE`), `ARCHITECTURE.md` (infra client, np2 structs, inet), `README.md` (infrastructure mode + how to run the server), this plan's Evidence section.

- [ ] **Step 1:** `grep -rn INFRA_RECON runtime/` → nothing.
- [ ] **Step 2:** `ninja && ctest` green; 30 fps `rt_regress.py` PASS; ad hoc two-instance smoke (`build/tools/two_lobby.py`) still finds the room.
- [ ] **Step 3:** Deploy to `build/e2e_portable` / `build/e2e_portable3` (backup first in `build/backup_pre_infra_<date>/`), `cmp` against the builds; hand-off note `build/autopilot/EN_QUE_NOS_QUEDAMOS_<date>.md` in Spanish with how to run the server and test with a friend.
- [ ] **Step 4: Commit.** Then ask the user before any push.
