# Ad hoc multiplayer, phase 0+1 (probe + control plane) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Patapon 3 joins an ad hoc group through a PRO ad hoc server: the game's netconf dialog connects, the server sees the player in the group, and two port instances in the same group see each other in the server's peer notices.

**Architecture:** A new generic unit `runtime/src/net/` holds pure parts (config, PRO packet codec) with ctest tests, a host TCP client for the server with its own receive thread, an adhocctl core (state + group operations + peer table) used by the HLE functions the game imports (`sceNetAdhocctlInit/Term/AddHandler/DelHandler`) and by the `sceUtilityNetconf*` dialog (ad hoc connect/create/join), and a guest-callback runner that delivers adhocctl handler events to game code on a runtime-created PSP thread. A small Python PRO server (`tools/adhoc_server.py`) serves the tests.

**Tech Stack:** C++17, host sockets (Winsock2 on Windows, BSD elsewhere), Python 3 stdlib (test server), ctest standalone tests, debug socket TCP 9999.

**Spec:** `docs/superpowers/specs/2026-10-10-adhoc-multiplayer-design.md`

## Global Constraints

- Protocol = PPSSPP's PRO ad hoc server protocol (`DECO/tools/ppsspp-src/Core/HLE/proAdhoc.h`, opcodes 0..7, packed little-endian structs), server TCP port 27312 by default.
- Generic runtime code only (no Patapon addresses in `runtime/src`), GPL-2.0-or-later provenance noted where PPSSPP code is ported.
- Not pointed at patapon.net: default server `127.0.0.1`; `PSPRECOMP_ADHOC_SERVER` overrides.
- Every blocking wait has a timeout and releases the guest CPU lock (`psp_cpu_block_begin`).
- Single player unchanged: with no server reachable the game's own error flow runs; no crash, no hang.
- Commits authored `Patapon Community` (repo config), no co-author trailer; push `git push p3 patapon3-windows:main`.
- Gates per task: `cd DECO/build/rtw && ninja && ctest` (0 failures) and the scale-1 language-menu baseline 0 diff (`tools/tgadiff.py`) for runtime changes.

## Review Focus

1. Server unreachable or killed mid-session: netconf must finish with an error status within its timeout, the game must not freeze, and the receive thread must exit cleanly at shutdown. Pinned in Task 4 (`server_gone`) and Task 7 (`netconf_times_out`).
2. Two instances on one PC: they need distinct MACs and port offsets; a stale persisted MAC must not be shared. Pinned in Task 1 (`mac_env_override`, `mac_persisted_once`).
3. Partial TCP reads: server packets split or coalesced across `recv` calls must decode identically. Pinned in Task 2 (`split_and_coalesced`).
4. Handler callbacks re-entering the HLE (a game handler calling adhocctl functions) must not deadlock the runner. Pinned in Task 6 (`reentrant_call`).
5. Nickname/group names with non-ASCII bytes or no terminator must be copied with fixed lengths, never read past the field. Pinned in Task 2 (`fixed_length_fields`).

---

### Task 0: Probe — what Patapon 3 calls in the ad hoc menu

**Files:** none in the repo; results recorded at the end of this file ("Probe evidence").

- [ ] **Step 1:** Boot to the hideout with HLE trace armed and open the DxD SQUARE menu:
  ```bash
  cd /c/Users/torso/Documents/deco/build
  TRACE_HLE=1 HIDEOUT_WAIT=20 timeout 400 python -I tools/to_square.py probe_adhoc PSPRECOMP_HLE_TRACE=2
  ```
  (to_square.py sends `T 1` before SQUARE when `TRACE_HLE` is set.)
- [ ] **Step 2:** With the game still running, select the menu's "Ad-hoc" entry (first entry: `B 4000 120` = CROSS) and wait 15 s; take `S` screenshots before/after.
- [ ] **Step 3:** Extract the network/utility calls in order:
  ```bash
  grep -a "HLE-TRACE.*\(sceNet\|sceWlan\|sceUtilityNetconf\|UNIMPLEMENTED\)" logs/probe_adhoc.log | head -200 > logs/probe_adhoc_net.txt
  ```
  Record in "Probe evidence": the sequence, the netconf `netAction` (read the param struct at the InitStart a0: `R <a0> 0x44`), the matching calls and their arguments.
- [ ] **Step 4:** Kill the game: `taskkill //IM psprecomp_runtime.exe //F`.

### Task 1: adhoc_config (pure)

**Files:** Create `runtime/include/net/adhoc_config.h`, `runtime/src/net/adhoc_config.cpp`, `runtime/tests/test_adhoc_config.cpp`; Modify `runtime/CMakeLists.txt`.

**Interfaces — Produces:**
```cpp
struct AdhocConfig {
    std::string server_host = "127.0.0.1";
    uint16_t server_port = 27312;
    std::string nickname = "Hero";          // <= 127 bytes
    std::array<uint8_t, 6> mac{};           // locally administered unicast
    uint16_t port_offset = 10000;
    std::string product_code;               // 9 chars, e.g. "UCES01421"
};
// env: PSPRECOMP_ADHOC_SERVER (host[:port]), PSPRECOMP_ADHOC_NICK, PSPRECOMP_ADHOC_MAC
// (aa:bb:cc:dd:ee:ff), PSPRECOMP_ADHOC_PORT_OFFSET. mac_file: where a generated MAC persists.
AdhocConfig adhoc_config_load(const char* (*getenv_fn)(const char*), const std::string& mac_file,
                              const std::string& product_code, uint32_t random_seed);
bool adhoc_parse_mac(const std::string& s, std::array<uint8_t, 6>* out);
std::string adhoc_format_mac(const std::array<uint8_t, 6>& mac);
```
- [ ] **Step 1: Failing tests** (`test_adhoc_config.cpp`): `defaults` (no env → 127.0.0.1:27312, offset 10000); `server_with_port` ("host:27999"); `mac_env_override` (env MAC wins over file); `mac_persisted_once` (no env, empty file → generated MAC has bit0 of byte0 clear and bit1 set, is written to the file; a second load returns the same MAC); `bad_mac_rejected` ("zz:..", 5 bytes → generated instead); `nick_truncated` (200-byte nickname → 127 bytes).
- [ ] **Step 2:** Run `ninja test_adhoc_config` → fails (header missing).
- [ ] **Step 3: Implement** with a deterministic xorshift from `random_seed` for the MAC (callers pass time-based seed); MAC file = one line `aa:bb:cc:dd:ee:ff`.
- [ ] **Step 4:** `ninja test_adhoc_config && ./test_adhoc_config.exe` → all passed; `ninja && ctest`.
- [ ] **Step 5: Commit** `feat(net): ad hoc config (server, nickname, persisted MAC, port offset)`.

### Task 2: PRO packet codec (pure)

**Files:** Create `runtime/include/net/pro_protocol.h`, `runtime/src/net/pro_protocol.cpp`, `runtime/tests/test_pro_protocol.cpp`.

**Interfaces — Produces:**
```cpp
namespace pro {
enum Opcode : uint8_t { PING = 0, LOGIN = 1, CONNECT = 2, DISCONNECT = 3, SCAN = 4,
                        SCAN_COMPLETE = 5, CONNECT_BSSID = 6, CHAT = 7 };
using Mac = std::array<uint8_t, 6>;
std::vector<uint8_t> encode_login(const Mac& mac, const std::string& nick, const std::string& product); // 144 bytes
std::vector<uint8_t> encode_connect(const std::string& group);  // 9 bytes, group padded/truncated to 8
std::vector<uint8_t> encode_simple(Opcode op);                   // PING / DISCONNECT / SCAN: 1 byte
std::vector<uint8_t> encode_chat(const std::string& msg);        // 65 bytes
struct Event {                     // one decoded server->client packet
    Opcode op;
    std::string nick;              // CONNECT (peer joined), CHAT
    Mac mac{};                     // CONNECT, SCAN (group host), CONNECT_BSSID
    uint32_t ip = 0;               // CONNECT, DISCONNECT (network byte order as sent)
    std::string group;             // SCAN (8 bytes, NULs stripped)
    std::string message;           // CHAT
};
class Decoder {                    // stream -> events; keeps partial packets
public:
    void feed(const uint8_t* data, size_t n);
    bool next(Event* out);
    bool error() const;            // unknown opcode seen: stream unusable
};
}
```
S2C sizes (from `proAdhoc.h`): CONNECT 1+128+6+4 = 139, DISCONNECT 1+4 = 5, SCAN 1+8+6 = 15, SCAN_COMPLETE 1, CONNECT_BSSID 1+6 = 7, CHAT 1+64+128 = 193, PING 1.
- [ ] **Step 1: Failing tests:** `login_layout` (opcode 1, MAC at 1..6, nick NUL-padded to 128 at 7..134, product at 135..143); `connect_layout`; `decode_each_s2c` (hand-built byte arrays for every S2C type); `split_and_coalesced` (two packets fed byte by byte and then as one buffer give the same events); `fixed_length_fields` (128-byte nickname without NUL decodes to 128 chars, no over-read); `unknown_opcode_sets_error`.
- [ ] **Step 2:** RED. **Step 3:** implement (memcpy into fixed buffers; `Decoder` keeps a `std::vector<uint8_t>` buffer and a size table per opcode). **Step 4:** GREEN + `ctest`. **Step 5: Commit** `feat(net): PRO ad hoc server packet codec`.

### Task 3: Python test server

**Files:** Create `tools/adhoc_server.py`.

Behaviour (PPSSPP `proAdhocServer.cpp` subset): listen TCP (default 27312); per client: LOGIN registers (mac, nick, product, peer ip); CONNECT(group): the client joins `product/group` (created if absent); the server sends the joiner one S2C CONNECT per existing member and every existing member an S2C CONNECT for the joiner, then the joiner CONNECT_BSSID with the group's first member MAC; DISCONNECT: members get S2C DISCONNECT(ip of leaver); SCAN: one S2C SCAN per group of the same product (group name, first member MAC) then SCAN_COMPLETE; PING ignored; CHAT relayed to the group as S2C CHAT; socket close = DISCONNECT. Logs one line per event.
- [ ] **Step 1:** Write the server with a `--selftest` mode that starts it on an ephemeral port, connects two raw clients (login A, login B, A connect "G", B connect "G", B scan) and asserts the exact S2C bytes each receives (`assert`), printing `selftest ok`.
- [ ] **Step 2:** `python -I tools/adhoc_server.py --selftest` → `selftest ok`.
- [ ] **Step 3: Commit** `feat(tools): minimal PRO ad hoc server for tests`.

### Task 4: ProClient (host TCP client)

**Files:** Create `runtime/include/net/pro_client.h`, `runtime/src/net/pro_client.cpp`, `runtime/src/net/host_socket.h` (Winsock/BSD shim: init, close, set nonblocking, errno), `runtime/tests/test_pro_client.cpp`.

**Interfaces:**
- Consumes: `pro::encode_*`, `pro::Decoder`, `AdhocConfig`.
- Produces:
```cpp
class ProClient {
public:
    // Connects and sends LOGIN; starts the receive thread. False on failure (logged).
    bool start(const std::string& host, uint16_t port, const pro::Mac& mac,
               const std::string& nick, const std::string& product);
    void stop();                                  // idempotent; joins the thread
    bool send_connect(const std::string& group);  // also used for create/join
    bool send_disconnect();
    bool send_scan();
    bool connected() const;                       // TCP up and logged in
    bool wait_event(pro::Event* out, int timeout_ms);  // from the receive thread
    struct Peer { pro::Mac mac; std::string nick; uint32_t ip; };
    std::vector<Peer> peers() const;              // maintained from CONNECT/DISCONNECT events
};
```
- [ ] **Step 1: Failing tests** against an in-test fake server (a thread with a listening socket on 127.0.0.1:0): `login_sent` (server reads exactly 144 bytes with our MAC); `peer_joined` (server writes an S2C CONNECT → `wait_event` returns it and `peers()` has the IP); `peer_left`; `server_gone` (server closes → `connected()` false, `wait_event` returns a DISCONNECT-from-server event with ip 0 within 1 s, `stop()` returns).
- [ ] **Step 2–4:** RED → implement (blocking `recv` with 200 ms timeout loop on the thread; `std::mutex` + `std::condition_variable` event queue) → GREEN + ctest.
- [ ] **Step 5: Commit** `feat(net): PRO ad hoc server client`.

### Task 5: adhocctl core + imported HLE + net basics

**Files:** Create `runtime/include/net/adhocctl.h`, `runtime/src/net/adhocctl.cpp`, `runtime/src/hle/psp_hle_net.cpp`; Modify `runtime/src/hle/psp_hle_misc.cpp` (move `sceWlanGetEtherAddr` to config MAC), the HLE registration list, `runtime/CMakeLists.txt`; Test `runtime/tests/test_adhocctl.cpp`.

**Interfaces — Produces:**
```cpp
enum class AdhocctlState { Disconnected = 0, Connected = 1, Scanning = 2 };
struct AdhocctlEvent { int event; int error; };  // ADHOCCTL_EVENT_* (proAdhoc.h)
class Adhocctl {                                 // owns a ProClient
public:
    int init(const AdhocConfig& cfg);            // SCE error codes as PPSSPP
    int term();
    int connect(const std::string& group);       // create/join/connect all map here
    int disconnect();
    AdhocctlState state() const;
    std::string group() const;
    bool poll_event(AdhocctlEvent* out);         // CONNECT when BSSID arrives, DISCONNECT, ...
    std::vector<ProClient::Peer> peers() const;
};
Adhocctl& adhocctl();
```
HLE (`psp_hle_net.cpp`): `sceNetInit/Term` (0), `sceNetAdhocInit/Term` (0, double init → PPSSPP error), `sceNetAdhocctlInit(stack, prio, product*)` reads the 9-byte product code and calls `adhocctl().init`, `sceNetAdhocctlTerm`, `sceNetAdhocctlAddHandler(fn, arg)` → handler id, `sceNetAdhocctlDelHandler(id)`, `sceWlanGetSwitchState` → 1, `sceWlanGetEtherAddr` → config MAC, `sceNetEtherNtostr/Strton`, `sceNetGetMallocStat` (PPSSPP values). Return codes and argument checks follow `sceNetAdhoc.cpp`.
- [ ] **Step 1: Failing test** (`test_adhocctl.cpp`, against the Task 3 server launched by the test? No — against the in-process fake from Task 4 reused as a helper): `connect_reaches_connected` (fake server answers CONNECT with CONNECT_BSSID → state Connected, event CONNECT), `disconnect_event`, `init_twice_error`.
- [ ] **Steps 2–4** RED → implement → GREEN + ctest.
- [ ] **Step 5: Commit** `feat(net): adhocctl core and imported net/adhocctl HLE`.

### Task 6: Guest callback runner

**Files:** Create `runtime/include/net/guest_callbacks.h`, `runtime/src/net/guest_callbacks.cpp`; Test: integration only (needs guest code) plus a unit test of the queue (`test_guest_callback_queue.cpp`).

**Interfaces — Produces:**
```cpp
// Queue a guest call fn(a0, a1, a2) to run on the runtime's callback thread
// (a PSP thread created with psp_thread_create, own stack from
// psp_alloc_kernel_memory, priority 0x30). Calls run in FIFO order with the
// guest CPU lock held like any guest code.
void guest_callbacks_post(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);
```
Adhocctl events are posted to every registered handler as `handler(event, error, arg)` (PPSSPP `__AdhocctlNotify`).
- [ ] **Step 1: Failing unit test** of the pure FIFO (`GuestCallQueue`: push/pop order, `reentrant_call` — a pop while a push happens from the consumer thread does not deadlock).
- [ ] **Steps 2–4** RED → implement queue + thread glue → GREEN + ctest.
- [ ] **Step 5: Commit** `feat(net): guest callback runner for ad hoc handlers`.

### Task 7: Netconf dialog, ad hoc modes

**Files:** Modify `runtime/src/hle/psp_hle_utility.cpp` (sceUtilityNetconf*); Create `runtime/src/net/netconf_adhoc.cpp/.h` (pure status logic) + `runtime/tests/test_netconf_adhoc.cpp`.

Behaviour (PPSSPP `PSPNetconfDialog.cpp` 217–360): `netAction` 2 (CONNECT_ADHOC), 4 (CREATE), 5 (JOIN): read `NetconfData->groupName` (8 bytes); call `adhocctl().connect(group)` once; stay RUNNING until state Connected (then FINISHED, `base.result = 0`) or 15 s timeout (FINISHED, result = PPSSPP's timeout error) ; infrastructure actions (0,1,3) finish with PPSSPP's "not supported" result as today.
- [ ] **Step 1: Failing tests** for the pure decision function `netconf_adhoc_step(state, elapsed_ms, connect_result) -> {status, result}`: `connects`, `netconf_times_out`, `infra_rejected`.
- [ ] **Steps 2–4** RED → implement + glue → GREEN + ctest.
- [ ] **Step 5: Commit** `feat(net): netconf dialog ad hoc connect/create/join`.

### Task 8: Integration — two instances in one group

- [ ] **Step 1:** `python -I tools/adhoc_server.py &` (log to `logs/adhoc_server.log`).
- [ ] **Step 2:** Start instance A (`PSPRECOMP_ADHOC_MAC=02:00:00:00:00:0a PSPRECOMP_ADHOC_PORT_OFFSET=10000 PSPRECOMP_ADHOC_NICK=A`, savedata copy A, debug socket default) and drive it to the ad hoc menu as in Task 0; instance B needs a different debug-socket port: add `PSPRECOMP_DEBUG_PORT` (default 9999) in `psp_debug_socket_start` call site as part of this task, with B on 9998.
- [ ] **Step 3:** Expected: the server log shows A and B in the same group; each runtime logs `[ADHOC] connected group=<name> peers=1`.
- [ ] **Step 4:** Docs: README "Building and Running" (ad hoc section + env vars), `docs/ENV_FLAGS.md` (new flags), ARCHITECTURE.md (net unit), `jugar.bat` comment lines for the server. HANDOFF + commit + push.

## Probe evidence

(filled by Task 0)
