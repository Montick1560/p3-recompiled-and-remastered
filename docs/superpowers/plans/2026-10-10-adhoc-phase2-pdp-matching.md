# Ad hoc multiplayer, phase 2 (PDP + matching) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two port instances in the same ad hoc group exchange PDP datagrams and run the PSP
matching protocol (hello/join/accept/cancel/bye), so they see each other in Patapon 3's ad hoc
lobby.

**Architecture:** PDP sockets are host UDP sockets bound to `port + port_offset` on the
instance's bind address; a peer MAC resolves to the IP the PRO server reported for it, and an
incoming datagram's source IP resolves back to a MAC (unknown sources are dropped, as in PPSSPP).
Matching is a per-context state machine (pure, unit-tested against an in-memory transport) driven
by a host thread that receives on the context's PDP socket, runs timers (hello, keepalive,
resend) and posts game callbacks through the guest-callback runner with 5 arguments and guest
buffers that stay valid until the callback returns.

**Tech Stack:** C++17, host UDP sockets (`psp_socket_compat.h`), ctest standalone tests.

**Spec:** `docs/superpowers/specs/2026-10-10-adhoc-multiplayer-design.md`
**Phase 1:** `docs/superpowers/plans/2026-10-10-adhoc-phase1-control.md` (config, PRO codec and
client, adhocctl, guest callbacks, netconf).

## Global Constraints

- Wire formats byte-identical to PPSSPP (`Core/HLE/sceNetAdhocMatching.cpp`, `sceNetAdhoc.cpp`):
  PDP payload = raw game data (no header); host port = PSP port + port offset (default 10000);
  matching packets: PING `[0]`, HELLO `[1][len u32][opt]`, JOIN `[2][len u32][opt]`, ACCEPT
  `[3][optlen u32][siblings u32][opt][siblings x MAC]`, CANCEL `[4][optlen u32][opt]`, BULK
  `[5][len u32][data]`, BIRTH `[7][MAC]`, DEATH `[8][MAC]`, BYE `[9]` (little-endian).
- Return codes from PPSSPP `ErrorCodes.h` (`SCE_NET_ADHOC_ERROR_*` 0x804107xx,
  `SCE_NET_ADHOC_MATCHING_ERROR_*` 0x804108xx).
- Blocking calls (PdpRecv without the nonblock flag) wait with the PSP timeout and release the
  guest CPU (`PspCpuReleaseScope`).
- Generic runtime code only; GPL-2.0-or-later provenance in each ported file.
- Commits authored `Patapon Community`, no co-author trailer; push `git push p3 patapon3-windows:main`.

## What Patapon 3 uses (static probe, phase 1 plan "Probe evidence")

- `sceNetAdhocPdpCreate(mac, port, 8192, 0)`, PdpSend/Recv/Delete.
- `sceNetAdhocMatching*`: Init/Term/Create/Delete/Start/Stop/SelectTarget/CancelTarget[WithOpt]/
  GetMembers/SetHelloOpt (no SendData).
- PTP open/listen/connect with 8192 buffers (phase 3).

## Review Focus

1. Two instances on one PC: each PDP socket binds to its own `PSPRECOMP_ADHOC_BIND` address;
   datagrams to a peer go to that peer's IP. Pinned in Task 2 (`two_bind_addresses`).
2. Datagrams from IPs that are not group peers are dropped without a callback or crash. Pinned in
   Task 2 (`unknown_source_dropped`).
3. Malformed matching packets (length fields larger than the datagram, truncated headers) are
   ignored without reading past the buffer. Pinned in Task 3 (`malformed_rejected`).
4. A blocking PdpRecv returns `SCE_NET_ADHOC_ERROR_TIMEOUT` after its timeout and a nonblocking
   one `WOULD_BLOCK` at once. Pinned in Task 2 (`recv_timeouts`).
5. Matching callback buffers (MAC + opt data) stay valid until the guest handler returns, even
   when events arrive faster than the game consumes them. Pinned in Task 5 (`buffers_outlive_calls`).

---

### Task 1: Peer resolution in adhocctl

**Files:** Modify `runtime/include/net/adhocctl.h`, `runtime/src/net/adhocctl.cpp`,
`runtime/tests/test_adhocctl.cpp`.

**Interfaces — Produces:**
```cpp
bool Adhocctl::resolve_mac(const pro::Mac& mac, uint32_t* ip) const;   // peer IP (wire order)
bool Adhocctl::resolve_ip(uint32_t ip, pro::Mac* mac) const;           // first peer with that IP
pro::Mac Adhocctl::local_mac() const;
uint16_t Adhocctl::port_offset() const;
std::string Adhocctl::bind_ip() const;
```
- [ ] **Step 1:** Failing test `resolve_peers`: after a fake-server CONNECT for kPeer at 10.0.0.2,
  `resolve_mac(kPeer)` gives that IP, `resolve_ip` gives kPeer, unknown ones fail.
- [ ] **Steps 2–4:** RED → implement from `client_.peers()` and the stored config → GREEN + ctest.
- [ ] **Step 5:** Commit `feat(net): resolve ad hoc peers between MAC and IP`.

### Task 2: PDP sockets

**Files:** Create `runtime/include/net/pdp.h`, `runtime/src/net/pdp.cpp`,
`runtime/tests/test_pdp.cpp`; Modify `runtime/src/hle/psp_hle_net.cpp`, `runtime/CMakeLists.txt`.

**Interfaces — Produces:**
```cpp
namespace pdp {
struct Resolver {   // adhocctl in the runtime, a fake in tests
    std::function<bool(const pro::Mac&, uint32_t*)> mac_to_ip;
    std::function<bool(uint32_t, pro::Mac*)> ip_to_mac;
    std::string bind_ip;
    uint16_t port_offset;
    pro::Mac local_mac;
};
class Sockets {
public:
    explicit Sockets(Resolver r);
    int create(uint16_t port, int bufsize);              // id 1..255 or error (PORT_IN_USE, ...)
    int remove(int id);
    int send(int id, const pro::Mac& to, uint16_t port, const uint8_t* data, int len);
    // timeout_us 0 + nonblock -> WOULD_BLOCK at once; blocking -> TIMEOUT after timeout_us.
    // *len in: buffer size, out: datagram size (NOT_ENOUGH_SPACE if it does not fit).
    int recv(int id, pro::Mac* from, uint16_t* from_port, uint8_t* buf, int* len,
             uint32_t timeout_us, bool nonblock);
    uint16_t local_port(int id) const;                   // PSP port (bound - offset)
};
}
```
HLE: `sceNetAdhocPdpCreate(mac*, port, bufsize, flag)` (writes the local MAC to `*mac`),
`PdpDelete(id, flag)`, `PdpSend(id, mac*, port, data*, len, timeout, flag)`,
`PdpRecv(id, mac*, port* (u16), buf*, len*, timeout, flag)`; blocking recv inside
`PspCpuReleaseScope`.
- [ ] **Step 1:** Failing tests (two `Sockets` in one process, bind 127.0.0.1 and 127.0.0.2,
  offsets equal, fake resolver mapping MAC A↔127.0.0.1, B↔127.0.0.2): `send_and_receive` (A
  port 30000 → B port 30000, B gets data, MAC A, port 30000), `two_bind_addresses`,
  `unknown_source_dropped` (raw UDP from 127.0.0.3 is discarded; recv then WOULD_BLOCK),
  `recv_timeouts` (nonblock → WOULD_BLOCK < 50 ms; blocking 200 ms → TIMEOUT in 150..600 ms),
  `port_in_use`, `buffer_too_small` (NOT_ENOUGH_SPACE and the needed size in `*len`).
- [ ] **Steps 2–4:** RED → implement (`select` for waits, `recvfrom` with MSG_PEEK for size) →
  GREEN + ctest.
- [ ] **Step 5:** HLE glue + `test_hle_net` cases for argument errors (null mac/len, bad id).
- [ ] **Step 6:** Commit `feat(net): ad hoc PDP sockets over UDP`.

### Task 3: Matching packet codec (pure)

**Files:** Create `runtime/include/net/matching_packet.h`, `runtime/src/net/matching_packet.cpp`,
`runtime/tests/test_matching_packet.cpp`.

**Interfaces — Produces:**
```cpp
namespace matching {
enum PacketType : uint8_t { PING = 0, HELLO = 1, JOIN = 2, ACCEPT = 3, CANCEL = 4, BULK = 5,
                            BULK_ABORT = 6, BIRTH = 7, DEATH = 8, BYE = 9 };
struct Packet {
    PacketType type;
    std::vector<uint8_t> opt;            // HELLO/JOIN/ACCEPT/CANCEL opt, BULK data
    std::vector<pro::Mac> siblings;      // ACCEPT
    pro::Mac mac{};                      // BIRTH/DEATH
};
std::vector<uint8_t> encode(const Packet& p);
bool decode(const uint8_t* data, size_t len, Packet* out);  // false: malformed
}
```
- [ ] **Step 1:** Failing tests: `layouts` (each type against hand-built bytes copied from the
  PPSSPP builders), `round_trip`, `malformed_rejected` (HELLO len 100 in a 20-byte datagram, ACCEPT
  sibling count beyond the data, empty datagram, unknown type 0x42).
- [ ] **Steps 2–4:** RED → implement → GREEN.
- [ ] **Step 5:** Commit `feat(net): ad hoc matching packet codec`.

### Task 4: Matching state machine (pure)

**Files:** Create `runtime/include/net/matching.h`, `runtime/src/net/matching.cpp`,
`runtime/tests/test_matching.cpp`.

**Interfaces — Produces:**
```cpp
namespace matching {
enum Mode { PARENT = 1, CHILD = 2, P2P = 3 };
enum Event { EV_HELLO = 1, EV_REQUEST = 2, EV_LEAVE = 3, EV_DENY = 4, EV_CANCEL = 5,
             EV_ACCEPT = 6, EV_ESTABLISHED = 7, EV_TIMEOUT = 8, EV_ERROR = 9, EV_BYE = 10,
             EV_DATA = 11, EV_DATA_ACK = 12, EV_DATA_TIMEOUT = 13 };
struct Io {   // what the context does to the outside world
    std::function<void(const pro::Mac& to, const std::vector<uint8_t>& packet)> send;
    std::function<std::vector<pro::Mac>()> group_peers;           // adhocctl peers
    std::function<void(int event, const pro::Mac& peer, const std::vector<uint8_t>& opt)> notify;
};
class Context {
public:
    Context(Mode mode, int maxpeers, uint32_t hello_us, uint32_t keepalive_us,
            int init_count, uint32_t rexmt_us, pro::Mac self, Io io);
    void start(std::vector<uint8_t> hello_opt);
    void stop();                                       // sends BYE
    void set_hello_opt(std::vector<uint8_t> opt);
    int select_target(const pro::Mac& peer, std::vector<uint8_t> opt);   // JOIN / ACCEPT
    int cancel_target(const pro::Mac& peer, std::vector<uint8_t> opt);   // CANCEL / leave
    void on_packet(const pro::Mac& from, const uint8_t* data, size_t len);
    void tick(uint64_t now_us);                        // hello, keepalive pings, timeouts
    std::vector<pro::Mac> members() const;             // GetMembers order (PPSSPP)
};
}
```
Behaviour: port PPSSPP `actOn*Packet`, `send*Packet`, `handleTimeout`, `countConnectedPeers`,
`sceNetAdhocMatchingSelectTarget/CancelTargetWithOpt/GetMembers` (states OFFER, PARENT, CHILD,
P2P, INCOMING/OUTGOING_REQUEST, CANCEL_IN_PROGRESS; events as listed).
- [ ] **Step 1:** Failing tests with two contexts wired by an in-memory `Io` (parent P, child C;
  then P2P pair): `hello_offer` (C gets EV_HELLO with P's opt), `join_accept` (C selects P → P
  gets EV_REQUEST; P selects C → both reach ESTABLISHED/ACCEPT, members lists match PPSSPP order),
  `deny` (P cancels a request → C gets EV_DENY), `leave_and_bye` (stop → peer gets EV_BYE),
  `timeout` (no pings for keepalive×init_count → EV_TIMEOUT), `maxpeers` (third join refused),
  `p2p_pair`.
- [ ] **Steps 2–4:** RED → implement → GREEN.
- [ ] **Step 5:** Commit `feat(net): ad hoc matching state machine`.

### Task 5: Matching HLE + callback buffers

**Files:** Create `runtime/src/hle/psp_hle_net_matching.cpp`; Modify
`runtime/include/net/guest_callbacks.h`, `runtime/src/net/guest_callbacks.cpp`,
`runtime/tests/test_guest_callback_queue.cpp`, `runtime/CMakeLists.txt`.

- `GuestCall` grows to five arguments and an optional guest buffer released after the call;
  buffers come from a guest ring (`psp_alloc_kernel_memory(256 KB)`, released in FIFO order).
- Per context: a host thread receives on the context's PDP socket (`pdp::Sockets::recv`
  nonblocking with 10 ms waits), feeds `Context::on_packet`, calls `tick`, and `notify` posts
  `handler(id, event, mac*, optlen, opt*)` with MAC at buf+0 and opt at buf+8 (PPSSPP layout).
- HLE: Init(memsize)/Term, Create (argument checks per PPSSPP: mode 1..3, port, maxnum 1..16,
  rxbuflen, intervals), Start(id, evthPri, evthStack, inthPri, inthStack, optLen, optData),
  Stop, Delete, SelectTarget(id, mac*, optLen, opt*), CancelTarget[WithOpt], SetHelloOpt,
  GetMembers(id, size*, buf*) (6-byte MAC + padding entries like PPSSPP `SceNetAdhocMatchingMemberInfoEmu`).
- [ ] **Step 1:** Failing tests: queue `buffers_outlive_calls` (a buffer is not reused before its
  call ran), `five_args`.
- [ ] **Steps 2–4:** RED → implement → GREEN; `test_hle_net` gains matching argument-error cases.
- [ ] **Step 5:** Commit `feat(net): sceNetAdhocMatching over PDP`.

### Task 6: Integration — two instances, one lobby

- [ ] **Step 1:** Server: `python -I tools/adhoc_server.py`.
- [ ] **Step 2:** Instance A: `PSPRECOMP_ADHOC_MAC=02:00:00:00:00:0a PSPRECOMP_ADHOC_BIND=127.0.0.1
  PSPRECOMP_ADHOC_NICK=A`; instance B: `...0b`, `BIND=127.0.0.2`, `NICK=B`,
  `PSPRECOMP_DEBUG_PORT=9998`, separate savedata copies. Drive both to the game's ad hoc lobby
  (menu path from the user / live probe).
- [ ] **Step 3:** Expected: each log shows `[ADHOC] connected group=...`, matching HELLO from the
  other MAC, and the lobby screenshot lists the other player.
- [ ] **Step 4:** Docs (ARCHITECTURE net row, ENV_FLAGS), HANDOFF, commit, push.
