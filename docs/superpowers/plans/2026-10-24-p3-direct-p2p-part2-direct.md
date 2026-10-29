# Direct P2P, Part 2 — Secure Direct Path (Phases 1–3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Players in one infrastructure-mode room exchange game datagrams and stream segments over a direct,
encrypted, authenticated UDP path found with STUN and NAT hole punching. The relay stays as the fallback.

**Architecture:**
- Pure units, each with ctests:
  - `direct_crypto`: Monocypher XChaCha20-Poly1305 + BLAKE2b;
  - `stun`;
  - `direct_path`: per-peer state machine with an injected clock;
  - `reliable_channel`: ARQ for stream segments.
- `direct_link` is the I/O shell: one UDP socket plus one thread. `infra::Client` owns it and keeps its public
  `send_*`/`pop_*` API, so `psp_hle_inet.cpp` does not change.
- `tools/infra_server.py` (protocol 3) does the signaling over TLS:
  - capabilities;
  - candidate offers;
  - per-pair keys;
  - opaque kind 4 frames for channel packets on the relay.

**Tech Stack:** C++20 runtime + Monocypher 4.0.2 (vendored), Winsock UDP; Python 3 server; Tauri launcher
(Rust + React); lab tools from Part 1.

**Spec:** `docs/superpowers/specs/2026-10-24-p3-direct-p2p-design.md` — read its "Revision (29 Oct 2026)" section
first. **Prerequisite (done 24 Oct, pushed 25 Oct):** `docs/superpowers/plans/2026-10-24-p3-direct-p2p-part1-measure.md`.
Its baseline is `build/infra/p2p_baseline/REPORT.md`, and its `--impair` server option is used here.

**Revised 29 Oct 2026.** New Tasks 0a–0c (real beta numbers, boss-mission traffic, desync meter) come first.
Delivery is split into two auto-updater releases:
- **Release A (u7)** = Tasks 0a–0c, 1–6, 9, 10 (datagram parts), 11, 12.
- **Release B (u8)** = Tasks 7–8, then Task 10's stream parts and Task 11 again.
- If Task 0b finds stream traffic inside the boss mission, Tasks 7–8 move into Release A (ledger the ruling).

## Decisions for the user (before Task 0a / before Task 11)

1. **Task 0a:** OK to open the SMB session to `tester` and copy `beta.db` (+ `-wal`/`-shm`) and `infra_server.log`
   (copies only, memory `beta-server-logs-access`). Without it, Task 0a is skipped and the field comparison in
   Task 11 has no "before".
2. **Task 11:** u7 as a **mandatory** update (recommended: a pair goes direct only when both players have u7, and the
   updater makes it one click) or optional.
3. **Task 10 Step 6:** a time when `tester` can run a client on the LAN with this PC.

## Global Constraints

- psprecomp branch `direct-p2p-2` from `ge-batching` (198e094; its pushed twin is p3/main 34cb608). Launcher branch
  `direct-option` from `updater` (09184d6 = origin/main). Nothing is pushed; ask before any push. A push follows the
  `push-p3-NN` recipe (next: `push-p3-34`): never the icon commit f639180 nor the debug_menu commit 067ac6e.
- Commits:
  - psprecomp and launcher author `Patapon Community <rexgamer1560@gmail.com>`;
  - no Claude co-author trailer;
  - messages end with `Verified: ...`.
- Native execution: inline, no subagents, no helper models.
- **Security over the feature** (spec "Security model", rules 1–11, are requirements, not suggestions). When a rule
  and a deadline conflict, the rule wins and the gap goes to the ledger.
- No home-made crypto: only Monocypher primitives (`crypto_aead_lock` / `crypto_aead_unlock`, `crypto_blake2b_keyed`,
  `crypto_wipe`, `crypto_verify16` if a constant-time compare is needed).
- No address this feature handles may reach a log, report, DB, UI or a player outside the room. Logs say the
  candidate **type** (`srflx`/`host`), the vip, and the RTT.
- Lab hooks (`PSPRECOMP_INFRA_DIRECT_IMPAIR`) work only when `psp_beta_until()` returns null (not a beta exe).
- Old clients (protocol 2), opted-out clients and server `--no-direct` keep today's behaviour byte for byte on the
  wire (kinds 1–3 only).
- Constants (from the spec):

  | Area | Values |
  |---|---|
  | Packets | magic `P3D1`; header 16 B; tag 16 B; max packet 1400 B |
  | Counters | start at 1; limit 2^62; replay window 2048 |
  | Probing | ≤ 4 candidates; every 100 ms; ≤ 10 probes per candidate per attempt; attempt 5 s; ≤ 3 attempts per pairing; retry after 30 s |
  | Liveness | keepalive after 2 s silent; dead after 4 s |
  | Rate limits | 600 packets/s; 1 MiB/s; flood → relay after 3 s |
  | Channel | RTO = max(40 ms, SRTT + 4·RTTVAR), cap 2000 ms; window 256 segments |
  | Signaling | offer rate 4/min; kind 4 frame ≤ 1500 B |

- Gates for runtime tasks:
  - `ninja && ctest` in `build/rtw` (115 tests on 29 Oct);
  - rt_regress 30 fps PASS against `build/tour/batch_base` (not `relevo_base`, stale), run at the same hour as the
    reference (the hideout sky follows the real clock);
  - for tasks that touch infra: `two.py` co-op smoke (`host_join.sh`).
- Run `python -I build/infra/diskquiet.py 60 900` before every tour or timed run: the C: SSD stalls 5–50 s and eats
  scripted key presses (`pc-disk-stalls`). Test instances use debug ports 9997/9998 (and two.py's C/D); never 9999.
- Gates for server tasks: `--selftest` → `selftest OK`, and `sh build/infra/regress_vs.sh`.
- Telemetry and the new `direct_stats` table stay in SQLite `beta.db` (spec "Revision", point 3).

## Review Focus

1. **Path switch in the middle of a big stream message** (the host's ~880 B state segments while the path dies):
   every byte arrives once and in order through kind 4. Task 8 test `switch_mid_transfer`.
2. **A peer that lies about its candidates** (a third party's address): we send it at most the probe budget and no
   game traffic. Task 5 test `lying_candidate_bounded`.
3. **The same player in two rooms with the same peer** (hideout + VS room): one pairing, not two, and no
   `direct_drop` while they still share a room. Task 4 selftest `pair_once_across_rooms`.
4. **NAT rebinding** (the peer's port changes mid-game): traffic keeps going after a re-validation, never to an
   unvalidated address. Task 5 test `rebinding_revalidates`.
5. **Clock jumps and stalls** (the PC freezes for 5–50 s on its slow disk, `pc-disk-stalls`): after a stall, the
   path does not flood and the channel does not resend a burst beyond its window. Task 5 test
   `stall_then_resume` and Task 7 test `long_stall_bounded_burst`.

6. **Mixed versions in one room** (a u6 player with u7 players, the first nights after the release): the u6 pair
   stays relayed with no error on either side. Task 4 selftest `no_caps_no_pair` and Task 10 Step 3.

---

### Task 0a: Real beta link numbers (`beta.db`)

**Files:**
- Create (local, not in a repo): `build/infra/campo/beta_link_report.py`, `build/infra/campo/REPORT.md`.
- Input: copies in `build/beta/reportes_2026-10-29/copia/` (never the originals on `tester`).

**Interfaces:**
- Produces: `python -I build/infra/campo/beta_link_report.py <beta.db> [--log <infra_server.log>]` prints Markdown
  tables; `--selftest` runs assert checks on an in-memory DB and prints `selftest OK`.
- Reads only `sessions` (`pings, slow_pings, rtt_p50, rtt_p95, rtt_max, max_rx_gap_ms`, label, staff, transport,
  times) and `rooms`/`events` (who shared a room). Never prints an IP, an online_id or a nickname: sessions are
  numbered in the report.

- [ ] **Step 1:** With the user's OK (Decision 1), open the SMB session and copy `beta.db`, `-wal`, `-shm` and
  `infra_server.log` to the folder above.
- [ ] **Step 2: Write `--selftest` first:** an in-memory DB with 6 sessions (2 without link_stats) and 2 rooms →
  asserts on: sessions counted, the share with `rtt_p95 >= 250` or `max_rx_gap_ms >= 400`, the per-room "worst
  member" pick, and no IP-shaped string in the output. Run → FAIL (functions missing).
- [ ] **Step 3: Implement**, run `--selftest` → `selftest OK`.
- [ ] **Step 4: Run on the copy.** `REPORT.md` (Spanish summary first):
  - sessions with link_stats, and the distribution of their p50 / p95 / max RTT and longest gap;
  - the share of multi-player sessions (shared a room with someone) over the 250 ms / 400 ms lines;
  - the count of `database is locked` lines in the log (the only signal that would reopen the PostgreSQL question).
- [ ] **Step 5:** Ledger line with the two headline numbers. No commit (build/ is not tracked).

### Task 0b: Boss mission, four players: does the mission use streams?

**Files:**
- Create: `build/infra/boss4/cmd.txt` (the two.py script), `build/infra/boss4/REPORT.md`.

- [ ] **Step 1:** `python -I build/infra/diskquiet.py 60 900`, then `two.py` with four instances (A–D), the local
  server with `--impair delay=90,jitter=15,stall=2,stall_ms=400`, and `LAG_DIR=build/infra/boss4`. Mission: the
  video's "[Multi] Torre de la Nevisca de la Legión" if `build/save_weather` reaches it, else the first co-op
  mission with a boss in that save (ledger which one). `SHOTS_DIR` on E:.
- [ ] **Step 2:** `tools/infra_lagjoin.py` over `net_<X>.csv` inside the mission window (M marks): datagrams/s and
  stream segments/s per instance, delivery p50/p99.
- [ ] **Step 3: Rule.** Stream segments ≤ 1/s per instance inside the mission → "datagram-only confirmed",
  Release A stays as planned. More → Tasks 7–8 move into Release A. Either way a `Ruling:` ledger line, and
  `REPORT.md` with the numbers.
- [ ] **Step 4:** If four instances do not fit (RAM, disk stalls), run three, then two, and say so in the report.

### Task 0c: Desync meter (game state from RAM, two instances at once)

**Files:**
- Create: `build/infra/desync_meter.py`, `build/infra/desync_fields.json`.

**Interfaces:**
- Produces: `python -I build/infra/desync_meter.py --ports 9997,9998 --fields build/infra/desync_fields.json
  --seconds N --out <csv>`: samples every field on every instance through the debug socket every 100 ms (same wall
  clock tick for all), and prints per field: samples, max |a−b|, p95 |a−b|, and the time to agree after a change
  (p50/p95). `--selftest` checks the pure `compare(a_series, b_series)` with asserts.
- `desync_fields.json`: `[{"name": "boss_hp", "addr": "0x...", "type": "u32|i32|f32", "via": "pointer chain or
  null"}]`.

- [ ] **Step 1 (timebox 45 min):** find, in a single-player boss mission, the boss HP and the boss X position:
  memory search through the debug socket (values that drop when the boss is hit / move when it walks), then the
  pointer that leads to them, so the address survives a new mission. Check what `efonte/patapon-re` and the
  `analysis/` notes already name. Not found in time → ledger it, and the meter ships with whatever fields were found
  (at least the hero X of each instance as seen by the other, if that is all).
- [ ] **Step 2:** `--selftest` first (identical series → 0; a lagged copy → time to agree = the lag), FAIL → implement
  → `selftest OK`.
- [ ] **Step 3: Baseline.** Run it during Task 0b's mission (or a 2-instance rerun) over the relay with the
  "international" profile. The numbers go to `build/infra/boss4/REPORT.md` as the "before" for Task 10.

### Task 1: Vendor Monocypher and a CSPRNG

**Files:**
- Create: `runtime/third_party/monocypher/monocypher.c`, `monocypher.h`, `LICENCE.md`, `README.md` (the version
  and the SHA-256 of the release tarball and of both files).
- Create: `runtime/include/net/secure_random.h`, `runtime/src/net/secure_random.cpp`.
- Modify: `runtime/CMakeLists.txt`:
  - Monocypher C file in the runtime library sources;
  - the `third_party/monocypher` include dir;
  - `bcrypt` linked on Windows;
  - new test.
- Test: `runtime/tests/test_direct_crypto_primitives.cpp`.

**Interfaces:**
- Produces: `namespace net { bool secure_random(uint8_t* out, size_t len); }`. It is false when the OS gives no
  randomness: BCryptGenRandom with `BCRYPT_USE_SYSTEM_PREFERRED_RNG` on Windows, `getentropy` in 256-byte chunks
  elsewhere. Callers treat false as "direct path off".
- Produces: the Monocypher 4.0.2 API, `#include "monocypher.h"`.

- [ ] **Step 1: Download Monocypher 4.0.2** from its official release (`https://monocypher.org/download/` or the
  GitHub tag `4.0.2` of `LoupVaillant/Monocypher`) into a new empty directory under the scratchpad.
  - Record the tarball's SHA-256 and compare it with the one published on the download page. Stop if it differs.
  - Copy only `src/monocypher.c`, `src/monocypher.h` and the licence.
- [ ] **Step 2: Write the failing test** `test_direct_crypto_primitives.cpp`:
  - `xchacha_kat`: `crypto_aead_lock` reproduces the test vector of draft-irtf-cfrg-xchacha-03 §A.3.1
    (key, nonce, AD, plaintext, ciphertext and tag copied from the draft).
  - `blake2b_kat`: `crypto_blake2b` of `"abc"` with a 64-byte hash matches RFC 7693 Appendix A.
  - `random_nonconstant`: two `secure_random` draws of 32 bytes succeed and differ; not all zero.
- [ ] **Step 3: Run** `ninja test_direct_crypto_primitives`. Expected: fails to build (headers missing).
- [ ] **Step 4: Add the vendored files, `secure_random`, and the CMake changes.**
- [ ] **Step 5: Run** the test. Expected: all pass. Then `ninja && ctest`: all pass.
- [ ] **Step 6: Commit** `build: vendor Monocypher 4.0.2 and add an OS CSPRNG (direct P2P)`.

### Task 2: Sealed packets (`direct_crypto`)

**Files:**
- Create: `runtime/include/net/direct_crypto.h`, `runtime/src/net/direct_crypto.cpp`.
- Test: `runtime/tests/test_direct_crypto.cpp` (registered in CMake).

**Interfaces:**
- Consumes: Task 1.
- Produces (`namespace direct`):
  ```cpp
  constexpr size_t kHeaderSize = 16, kTagSize = 16, kMaxPacket = 1400;
  constexpr uint64_t kCounterLimit = 1ull << 62;
  enum class Type : uint8_t { Probe = 1, ProbeAck = 2, Keepalive = 3, Dgram = 4, Seg = 5, Ack = 6, Close = 7 };
  using Key = std::array<uint8_t, 32>;
  struct Keys { Key tx{}, rx{}; void wipe(); };
  // tx = BLAKE2b-256(key = pair_key, "p3d1 dir" || be32(my_vip) || be32(peer_vip)); rx with the vips swapped.
  Keys derive_keys(const uint8_t pair_key[32], uint32_t my_vip, uint32_t peer_vip);
  // "P3D1" | le32 pair_id | le64 counter | AEAD(type || body) | tag. Nonce = le64 counter || 16 zero bytes,
  // AD = the 16-byte header. Empty when counter == 0, counter >= kCounterLimit, or the packet > kMaxPacket.
  std::vector<uint8_t> seal(const Key& key, uint32_t pair_id, uint64_t counter, Type type,
                            const uint8_t* body, size_t len);
  bool read_header(const uint8_t* pkt, size_t len, uint32_t* pair_id, uint64_t* counter);  // magic + length only
  struct Opened { uint32_t pair_id = 0; uint64_t counter = 0; Type type{}; std::vector<uint8_t> body; };
  bool open(const Key& key, const uint8_t* pkt, size_t len, Opened* out);  // false: bad tag / size / unknown type
  class ReplayWindow {  // 2048 counters
  public:
      bool check(uint64_t counter) const;  // false: 0, duplicate, or older than newest − 2047
      void accept(uint64_t counter);       // only after open() succeeded
  };
  ```

- [ ] **Step 1: Write the failing tests:**
  - `roundtrip_each_type`: seal/open every `Type` with bodies of 0, 1, 1000 bytes.
  - `keys_mirror`: `derive_keys(k, A, B).tx == derive_keys(k, B, A).rx`, and `.tx != .rx`.
  - `bit_flip_rejected`: for a 200-byte packet, flipping each single bit (header, ciphertext, tag) makes `open`
    false.
  - `wrong_key_rejected`, `truncated_rejected` (every length 0..len−1), `oversize_refused` (`seal` returns empty
    for a body that makes > 1400 B).
  - `counter_rules`: `seal` with counter 0 or `kCounterLimit` returns empty.
  - `replay_window`:
    - 5 accepted → `check(5)` false; `check(4)` true while it is in the window;
    - after accepting 3000, `check(900)` false and `check(2000)` true;
    - huge jumps work.
  - `fuzz_open`: 200 000 random buffers (seeded `std::mt19937_64`, lengths 0..1500) and 200 000 single-byte
    mutations of a valid packet. `open` never returns true and never crashes.
- [ ] **Step 2: Run** `ninja test_direct_crypto` → fails to build.
- [ ] **Step 3: Implement** with Monocypher only. Use `crypto_wipe` on every stack copy of a key.
- [ ] **Step 4: Run** → all pass.
- [ ] **Step 5: Commit** `feat(net): sealed direct-path packets with replay window`.

### Task 3: STUN Binding (`stun`)

**Files:**
- Create: `runtime/include/net/stun.h`, `runtime/src/net/stun.cpp`.
- Test: `runtime/tests/test_stun.cpp`.

**Interfaces:**
- Produces (`namespace stun`):
  ```cpp
  constexpr uint32_t kMagicCookie = 0x2112A442;
  std::vector<uint8_t> binding_request(const uint8_t txid[12]);  // 20 bytes, no attributes
  // A Binding Success Response (0x0101) with this transaction id: XOR-MAPPED-ADDRESS (0x0020), else
  // MAPPED-ADDRESS (0x0001), IPv4 only, host byte order.
  bool parse_binding_success(const uint8_t* pkt, size_t len, const uint8_t txid[12], uint32_t* ip, uint16_t* port);
  bool looks_like_stun(const uint8_t* pkt, size_t len);  // top two bits 0 and the magic cookie at bytes 4..7
  ```

- [ ] **Step 1: Write the failing tests:**
  - `rfc5769_ipv4_response`: the "Sample IPv4 response" of RFC 5769 §2.2, bytes copied from the RFC, with its
    transaction id → 192.0.2.1:32853.
  - `request_layout`: type 0x0001, length 0, cookie, the txid.
  - `wrong_txid`, `wrong_type` (0x0111 error response), `attribute_overruns_length`, `ipv6_only_response` → false.
  - `demux`: a sealed `P3D1` packet is not `looks_like_stun`; the RFC response is.
  - `fuzz_parse`: 200 000 random and mutated buffers → no crash, never true with a wrong txid.
- [ ] **Step 2–4: Run → fail, implement, run → pass.**
- [ ] **Step 5: Commit** `feat(net): STUN binding request/response codec`.

### Task 4: Server signaling (protocol 3)

**Files:**
- Modify: `tools/infra_server.py`:
  - login caps and the answer's `direct` object;
  - `--no-direct` and `--stun`, plus a `"direct": false` key in the beta config;
  - `op_direct_offer`, pairing, `direct_peer` / `direct_drop`;
  - kind 4 forwarding;
  - `op_direct_stats` + the `direct_stats` table;
  - selftest.
- Modify: the server guide (where `--delay-ms` is documented in `docs/`).

**Interfaces:**
- Produces (wire, protocol 3):
  - Login request: `"caps": {"direct": 1}`.
  - Login answer: `"direct": {"on": bool, "stun": ["stun.cloudflare.com:3478", "stun.l.google.com:19302"]}`, only
    when caps were sent. `on` is false with `--no-direct` or beta config `"direct": false`.
  - `{"op":"direct_offer","cands":[{"t":"srflx"|"host","a":"a.b.c.d:port"}]}`:
    - ≤ 4 candidates, valid unicast IPv4, port 1–65535;
    - ≤ 4 offers per minute per connection;
    - answer `{}`;
    - errors `BAD_REQUEST` (malformed) / `SERVICE_UNAVAILABLE` (rate).
  - Event `{"ev":"direct_peer","vip":V,"pair":P,"key":"<64 hex>","cands":[...]}`, sent to both sides when:
    - the two connections share at least one room;
    - both sent caps and an offer;
    - direct is on.

    One pairing per connection pair, whatever the number of shared rooms. A new offer from either side replaces
    the pairing: `direct_drop` for the old one first, then a new `direct_peer` with a fresh key and pair id.
  - Host-candidate rule: the peer's `host` candidates are forwarded only when both sides offered a `srflx` with the
    same IP.
  - Event `{"ev":"direct_drop","vip":V,"pair":P}`, sent to the remaining side(s) when the two no longer share a room
    or one disconnects.
  - Kind 4 frame. To the server: `u32 dst_vip | opaque bytes (≤ 1500)`; to the peer: `u32 src_vip | the same bytes`.
    - Forwarded only when a live pairing exists between the two.
    - Never dropped otherwise; counted in the connection's stream byte budget (`STREAM_BYTES_PER_S`).
    - Without a pairing it is dropped and counted.
  - `{"op":"direct_stats","pairs":[{"path":"direct"|"relay","rtt_p50","rtt_p95","loss_x10","direct_s","fallbacks"}]}`:
    - ≤ 8 entries, integers 0..600000;
    - stored in a new table `direct_stats(session INT, path TEXT, rtt_p50 INT, rtt_p95 INT, loss_x10 INT,
      direct_s INT, fallbacks INT)`.
  - Metrics events `direct_pair` / `direct_drop` with no detail.
- Keys come from `secrets.token_bytes(32)`. Pair ids come from `secrets.randbits(32)`, never 0, unique among live
  pairs.

- [ ] **Step 1: Write the failing selftest blocks** (with `TestClient`; the logger is captured into a list):
  - `direct_caps_answer`: login with caps → `direct.on` true and the STUN list; without caps → no `direct` key;
    `--no-direct` → `on` false.
  - `pair_on_join`: A creates, both offer, B joins → both get `direct_peer` with the same pair/key, each other's
    vip, and only srflx candidates (different srflx IPs).
  - `host_rule_same_nat`: equal srflx IPs → host candidates included.
  - `pair_once_across_rooms`: A and B also share a VS room → still one pairing. Leaving the VS room sends no drop;
    leaving the last shared room sends `direct_drop` to the one left.
  - `reoffer_rekeys`: a second offer from B → `direct_drop` then `direct_peer` with a new key and pair id.
  - `kind4_only_when_paired`: kind 4 from A to C (no pairing) is not delivered; A→B is delivered byte for byte with
    A's vip.
  - `offer_validation`: 5 candidates, `"999.1.1.1:5"`, `"10.0.0.1:0"`, `"224.0.0.1:5"`, `"0.0.0.0:5"`,
    non-string → `BAD_REQUEST`; a fifth offer within a minute → `SERVICE_UNAVAILABLE`.
  - `no_caps_no_pair`: a protocol-2 client (no caps) in the room gets no `direct_*` events and nobody is paired
    with it.
  - `privacy`: after all blocks, no captured log line contains any offered candidate IP or any key hex. The DB
    holds no candidate IP: dump all tables and search.
  - `direct_stats_stored`, and invalid forms refused (same pattern as Part 1's `link_stats`).
- [ ] **Step 2: Run** the selftest → FAIL in the new blocks.
- [ ] **Step 3: Implement.**
  - Pairings live in `self.pairs: dict[frozenset[Conn], Pair]`. `maybe_pair(a, b)` is called after a join (joiner
    × each member), after an offer (offerer × every connection that shares a room), and on config reload when
    direct turns on.
  - `unpair_if_apart(a, b)` is called in `leave_room` and `drop`.
  - Candidates are kept in `conn.cands` only.
- [ ] **Step 4: Run** the selftest → `selftest OK`; then `sh build/infra/regress_vs.sh` → passes.
- [ ] **Step 5: Commit** `feat(infra-server): direct-path signaling (caps, offers, per-pair keys, kind 4)`.

### Task 5: The per-peer path (`direct_path`)

**Files:**
- Create: `runtime/include/net/direct_path.h`, `runtime/src/net/direct_path.cpp`.
- Test: `runtime/tests/test_direct_path.cpp`, a simulated network in the test: a queue of `Out` with per-test
  delay/loss/address mapping, and a fake clock.

**Interfaces:**
- Consumes: Task 2 (`Keys`, `seal`, `open`, `ReplayWindow`, `Type`, `Opened`).
- Produces (`namespace direct`):
  ```cpp
  struct Addr { uint32_t ip = 0; uint16_t port = 0; bool operator==(const Addr&) const = default; };
  enum class CandType : uint8_t { Host, Srflx };
  struct Cand { Addr addr; CandType type = CandType::Srflx; };
  struct Out { Addr to; std::vector<uint8_t> bytes; };
  using Rng = std::function<uint64_t()>;  // secure_random in production, seeded in tests
  struct PathConfig {
      int probe_interval_ms = 100, probes_per_cand = 10, attempt_ms = 5000, max_attempts = 3, retry_ms = 30000;
      int keepalive_ms = 2000, dead_ms = 4000;
      int max_pkts_per_s = 600, max_bytes_per_s = 1 << 20, flood_s = 3;
      int max_unvalidated_acks_per_s = 10;
  };
  enum class PathState { Probing, Up, Down, Closed };
  struct PathStats {
      int rtt_p50 = -1, rtt_p95 = -1;
      uint64_t sent = 0, received = 0, dropped_auth = 0, dropped_replay = 0, dropped_rate = 0;
      int64_t up_ms = 0;
      int fallbacks = 0;
  };
  class Path {
  public:
      Path(uint32_t pair_id, const Keys& keys, std::vector<Cand> cands, int64_t now_ms, Rng rng, PathConfig cfg = {});
      ~Path();  // wipes keys
      void tick(int64_t now_ms, std::vector<Out>* out);
      // A packet whose header carries this pair id. true when *app holds an accepted Dgram/Seg/Ack.
      bool on_packet(const Addr& from, const uint8_t* pkt, size_t len, int64_t now_ms, std::vector<Out>* out,
                     Opened* app);
      bool send(Type type, const uint8_t* body, size_t len, int64_t now_ms, std::vector<Out>* out);  // false: not Up / too big
      // Seals for the relay (kind 4) with the same counter space; empty when the counter is exhausted.
      std::vector<uint8_t> seal_for_relay(Type type, const uint8_t* body, size_t len);
      // A packet that came through the relay (kind 4): same checks, no address learning.
      bool on_relayed(const uint8_t* pkt, size_t len, int64_t now_ms, Opened* app);
      void close(int64_t now_ms, std::vector<Out>* out);  // CLOSE to the path if Up, wipe, Closed
      PathState state() const;
      CandType path_type() const;
      PathStats stats() const;
  };
  ```
- Behaviour, from the spec:
  - A candidate becomes the path only on an authenticated `ProbeAck` from that address that echoes a challenge we
    sent to it.
  - An authenticated `Probe` from any address gets a `ProbeAck`. Replies to unvalidated addresses are capped at
    `max_unvalidated_acks_per_s`.
  - Unauthenticated input changes nothing and sends nothing.
  - A peer `Close` makes the path Closed.

- [ ] **Step 1: Write the failing tests:**
  - `punch_both_sides`: two Paths with mirrored keys and each other's candidate. Within 300 simulated ms both are
    Up; `send(Dgram)` on each arrives at the other as `app`.
  - `unreachable_goes_down`: candidates that drop everything → Down at 5 s. Per attempt, ≤ 10 probes per candidate,
    each ≤ 96 B. A retry happens at 35 s; after the third attempt, no more probes ever.
  - `lying_candidate_bounded`: a candidate address that never answers gets ≤ 30 probes in total over a pairing's
    life, and never a `Dgram`.
  - `garbage_ignored`: random bytes, a packet sealed with another key, a replayed valid packet → no output,
    `dropped_auth` / `dropped_replay` counted.
  - `keepalive_and_dead`: Up and silent → a Keepalive every 2 s. The peer goes silent → Down at 4 s, `fallbacks`
    counted, retry rules apply.
  - `rebinding_revalidates`: the peer's packets start coming from a new port. We probe it, and only after its
    `ProbeAck` does `send` go to the new address; until then, to the old one.
  - `rate_limit_floods_to_closed`: 700 valid packets/s for 4 s → packets over the limit dropped, then Closed.
  - `stall_then_resume`: the clock jumps 30 s while Up. The next tick sends at most one Keepalive and one probe
    burst bounded by the config, and no queue of old packets.
  - `relay_shares_counters`: interleaved `send` and `seal_for_relay` packets all open on the peer
    (`on_packet` / `on_relayed`). A packet replayed across paths is dropped.
  - `close_wipes`: after `close`, `send` is false and `on_packet` of a valid packet returns false.
- [ ] **Step 2–4: Run → fail; implement; run → pass.**
- [ ] **Step 5: Commit** `feat(net): direct-path state machine (probing, validation, keepalive, limits)`.

### Task 6: `direct_link` and datagrams over the direct path

**Files:**
- Create: `runtime/include/net/direct_link.h`, `runtime/src/net/direct_link.cpp`.
- Modify:
  - `runtime/include/net/infra_client.h`, `runtime/src/net/infra_client.cpp`: caps, the link's lifetime, events,
    the datagram path, `direct_stats`;
  - `runtime/include/net/infra_client.h`: `kProto = 3`.
- Modify: `runtime/CMakeLists.txt` (sources + tests).
- Test: `runtime/tests/test_direct_link.cpp` (real UDP sockets on 127.0.0.1); new cases in `test_infra_client.cpp`.

**Interfaces:**
- Consumes: Tasks 1–3 and 5; Task 4's wire protocol.
- Produces (`namespace direct`):
  ```cpp
  struct LinkOptions {
      std::vector<std::string> stun;          // "host:port" from the login answer
      std::vector<uint32_t> test_host_ips;    // tests only: host candidates to add (e.g. 127.0.0.1)
      std::string impair;                     // "delay=MS,jitter=MS,loss=PCT,reorder=PCT"; ignored in beta builds
  };
  // A game datagram (any order) or, from Task 8 on, a stream segment already in order: type is Dgram or Seg.
  using DeliverFn = std::function<void(uint32_t peer_vip, Type type, uint16_t dst, uint16_t src,
                                       std::vector<uint8_t> payload)>;
  using RelayFn = std::function<void(uint32_t peer_vip, std::vector<uint8_t> sealed)>;  // to send as kind 4
  class Link {
  public:
      bool start(const LinkOptions& opt, DeliverFn deliver, RelayFn relay);  // false: no socket or no randomness
      void stop();  // Close to every peer, wipe, join; ≤ 1 s
      std::vector<Cand> local_candidates(int wait_ms);  // srflx from STUN (+ test_host_ips), after gathering
      void add_peer(uint32_t my_vip, uint32_t peer_vip, uint32_t pair_id, const uint8_t key[32],
                    std::vector<Cand> cands);
      void drop_peer(uint32_t peer_vip);
      bool has_peer(uint32_t peer_vip) const;
      bool is_up(uint32_t peer_vip) const;
      bool send_dgram(uint32_t peer_vip, uint16_t dst, uint16_t src, const uint8_t* data, size_t len);  // false → relay
      void on_relayed(uint32_t peer_vip, const uint8_t* bytes, size_t len);  // a kind 4 payload
      nlohmann::json stats_json() const;  // direct_stats "pairs" array, no addresses
  };
  }
  ```
  `Dgram` body: `le16 dst | le16 src | payload`.
- `infra::Client`:
  - **Enabling.** Direct is enabled when all of these hold:
    - `PSPRECOMP_INFRA_DIRECT` is not `0`/`false`;
    - the address is `Wss`, or a loopback host (`127.0.0.1`, `localhost`, `::1`);
    - `secure_random` works.

    Then the login adds `caps` and the client starts the link when the answer says `on`, sending `direct_offer`
    after gathering (≤ 2 s, on its own thread). Otherwise it logs exactly one reason line:
    `[INFRA] direct: off (opted out | server off | plain-text server | no randomness)`.
  - **Events.** `direct_peer` → `add_peer`; `direct_drop` → `drop_peer`. Neither reaches the game's event handler.
  - **Datagrams.** `send_datagram` tries `send_dgram` first. Delivered `Dgram`s go into the same per-port queues and
    byte caps as relayed ones, through one private `enqueue_datagram(...)` that the reader also uses.
  - **Disconnect.** `disconnect()` sends `direct_stats`, waiting ≤ 1000 ms like `link_stats`, then stops the link.
    `lost()` stops the link without sending anything, so the keys die with the server session.
  - **Log lines.**
    - `[INFRA] direct: on (N STUN servers)`
    - `[INFRA] direct: vip %08x up (%s, rtt %d ms)`
    - `[INFRA] direct: vip %08x back to relay (%s)`
    - `[INFRA] direct: lab impairment ignored in a beta build`

    No other direct-path log lines.

- [ ] **Step 1: Write the failing tests:**
  - `test_direct_link.cpp`:
    - `loopback_dgram_both_ways`: two Links, `test_host_ips={127.0.0.1}`, no STUN, `add_peer` with the same key on
      both. `is_up` within 1 s; 100 datagrams each way arrive with exact bytes and ports.
    - `wrong_key_never_up`: the keys differ → never Up within 6 s; `send_dgram` false; nothing delivered.
    - `drop_peer_stops`: after `drop_peer`, `send_dgram` false, and a packet from the old peer is not delivered.
    - `impair_loss_100_falls_back`: impair `loss=100` on one side after Up → that side is not Up within 5 s.
    - `stop_is_quick`: `stop()` returns in ≤ 1 s.
    - `stun_only_from_asked_server`: a fake STUN server on loopback answers, and a second socket sends a valid
      response with the right txid from another port. Only the first sets the srflx candidate. A response with a
      wrong txid from the right address is ignored too.
  - `test_infra_client.cpp`:
    - `caps_on_loopback`: the login frame carries `caps.direct == 1` and `proto == 3`.
    - `no_caps_when_opted_out`: `PSPRECOMP_INFRA_DIRECT=0` → no `caps`, and the log reason "opted out".
    - `no_caps_plaintext_remote`: `Address{Tcp, "192.0.2.10"}` → no caps.
    - `direct_events_not_forwarded`: `direct_peer` / `direct_drop` events never reach `on_event`'s handler.
    - `loss_drops_peers`: after the server connection is lost (`fake.close_client()`), every peer is dropped
      (keys wiped) and `send_datagram` goes nowhere until a new login and pairing.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.**
  - The link thread `select()`s on the socket with a 20 ms timeout and runs `tick` for every Path.
  - It demuxes STUN vs `P3D1` and looks up the Path by pair id. A packet with an unknown pair id is dropped and
    counted.
  - The impairment parser is shared with nothing else. It is active only when `psp_beta_until()` is null.
- [ ] **Step 4: Run** → pass; `ninja && ctest` → all pass.
- [ ] **Step 5: Gates:**
  - rt_regress 30 fps PASS;
  - `two.py` co-op smoke (`host_join.sh`) with the local server: both logs show
    `[INFRA] direct: vip ... up (host` (loopback host candidates come from `PSPRECOMP_INFRA_DIRECT_TEST_HOSTS=127.0.0.1`,
    a lab-only env read only outside beta builds);
  - the mission still plays;
  - `INFRA_SERVER_ARGS=--no-direct` → `direct: off (server off)` and the same smoke passes.
- [ ] **Step 6: Commit** `feat(infra): datagrams over the secure direct path, relay fallback`.

### Task 7: Reliable channel for stream segments (`reliable_channel`)

**Files:**
- Create: `runtime/include/net/reliable_channel.h`, `runtime/src/net/reliable_channel.cpp`.
- Test: `runtime/tests/test_reliable_channel.cpp`, with a simulated link: delay, jitter, loss, reorder, seeded.

**Interfaces:**
- Produces (`namespace direct`):
  ```cpp
  struct ChannelConfig { int min_rto_ms = 40, max_rto_ms = 2000, window = 256, ack_delay_ms = 20; };
  struct Delivered { uint16_t dst_port = 0, src_port = 0; std::vector<uint8_t> bytes; };
  class Channel {
  public:
      explicit Channel(ChannelConfig cfg = {});
      uint32_t push(uint16_t dst_port, uint16_t src_port, const uint8_t* seg, size_t len);  // seq from 1
      // SEG bodies to send now (`le32 seq | le16 dst | le16 src | bytes`): new ones inside the window,
      // RTO expiries (with exponential backoff, capped at max_rto_ms), fast retransmits (3 SACKs past a hole).
      void poll(int64_t now_ms, std::vector<std::vector<uint8_t>>* seg_bodies);
      void on_seg(const uint8_t* body, size_t len, int64_t now_ms, std::vector<Delivered>* ready);  // in order, once
      bool ack_due(int64_t now_ms) const;   // at once after an out-of-order SEG, else after ack_delay_ms
      std::vector<uint8_t> ack_body();      // le32 cumulative | le32 SACK bitmap (bit i = cumulative+1+i held)
      void on_ack(const uint8_t* body, size_t len, int64_t now_ms);
      size_t unacked() const;
      int srtt_ms() const;
  };
  }
  ```
  RTT samples come only from segments sent once (Karn). Malformed bodies are ignored. Out-of-order segments beyond
  the window are not stored.

- [ ] **Step 1: Write the failing tests:**
  - `in_order_lossless`.
  - `ten_mb_lossy_exact`: 10 MB in ≤ 1025-byte segments both ways at once, with delay 90 ± 15 ms, 5 % loss and
    20 % reorder. The received bytes hash (FNV-1a 64) to the sent ones, nothing is delivered twice, and the
    simulated time stays under 120 s.
  - `duplicates_ignored`.
  - `fast_retransmit_on_sack`: one lost segment is resent before its RTO.
  - `rto_backoff_capped`: a dead link backs off to 2000 ms and no faster.
  - `long_stall_bounded_burst`: the clock jumps 40 s with 256 unacked → `poll` returns ≤ window segments, not
    thousands.
  - `malformed_bodies`: truncated SEG/ACK are ignored, no crash, plus a fuzz loop of 100 000 random bodies.
- [ ] **Step 2–4: Run → fail; implement; run → pass.**
- [ ] **Step 5: Commit** `feat(net): reliable ordered channel for stream segments over the direct path`.

### Task 8: Streams through the channel (direct or kind 4), path switch

**Files:**
- Modify: `runtime/src/net/direct_link.cpp` and `.h`: a `Channel` per peer; `send_stream`; ACK handling; the poll
  loop.
- Modify: `runtime/src/net/infra_client.cpp` and `.h`:
  - streams to a paired vip go into the link;
  - kind 4 in the reader goes to `Link::on_relayed`;
  - kind 4 in the writer comes from `RelayFn`;
  - a stream reset on `direct_drop`;
  - `pop_peer_reset`.
- Modify: `runtime/include/net/infra_protocol.h` and `.cpp`: `kKindChannel = 4`, `encode_channel(vip, bytes)`,
  and `FrameReader` accepting kind 4.
- Modify: `runtime/include/net/p2p_stream.h` and `.cpp`: `void reset_peer(uint32_t vip)` (every connection with
  that peer gets the reset state an RST gives).
- Modify: `runtime/src/hle/psp_hle_inet.cpp`: drain `pop_peer_reset` into `g_streams.reset_peer` where it drains
  `pop_stream`.
- Test: `test_direct_link.cpp`, `test_infra_protocol.cpp`, `test_p2p_stream.cpp` (new cases).

**Interfaces:**
- Consumes: Tasks 5–7.
- Produces:
  - `bool Link::send_stream(uint32_t peer_vip, uint16_t dst, uint16_t src, const uint8_t* seg, size_t len)`. It
    returns false only when the peer is not paired (the caller uses kind 3). Once paired it always queues, and the
    link sends over the direct path or the RelayFn.
  - `DeliverFn` gets `(vip, Type::Seg, dst, src, payload)` already in order. The client turns them into
    `Datagram`s in `streams_`, same caps.
  - `bool Client::pop_peer_reset(uint32_t* vip)`.
- Ordering rule (spec "Ordering at the switch-over"): from `add_peer` on, every stream segment to that vip goes
  through the channel. Kind 3 is used only before pairing.

- [ ] **Step 1: Write the failing tests:**
  - `channel_over_relay_only`: two Links paired but with no reachable candidate. The RelayFn of each feeds the
    other's `on_relayed`. 1 MB of segments goes through intact.
  - `switch_mid_transfer`: 10 MB with impair loss 5 % and reorder 20 %. The direct path is killed and restored three
    times (`impair` toggled through a test setter); the bytes are exact and in order, with no duplicates.
  - `kind4_frames`: `encode_channel` / `FrameReader` round trip; a kind 4 over 1500 B is refused at encode.
  - `reset_peer_resets_only_that_peer` in `test_p2p_stream.cpp`.
  - `drop_resets_streams`: after `direct_drop` the client reports `pop_peer_reset(vip)` once.
- [ ] **Step 2–4: Run → fail; implement; run → pass. `ninja && ctest` → all pass.**
- [ ] **Step 5: Gates:**
  - rt_regress 30 fps PASS;
  - `two.py` co-op mission start→results with direct up, and again with `INFRA_SERVER_ARGS=--no-direct`;
  - `sh build/infra/regress_vs.sh`;
  - ad hoc smoke (`ADHOC=1 two.py`).
- [ ] **Step 6: Commit** `feat(infra): game streams over the reliable channel, direct or relayed (kind 4)`.

### Task 9: Launcher option, settings, docs

**Files:**
- Modify (launcher repo, branch `direct-option`):
  - `src-tauri/core_options.json`: the new option;
  - `src/screens/Options.tsx` + `src/styles.css`: description line breaks (`white-space: pre-line` on
    `.description`);
  - tests in `src-tauri/src/options.rs` and `src/App.test.tsx`, or the Options test file.
- Modify (psprecomp):
  - `runtime/src/psp_settings.cpp`: `{"online", "direct", "PSPRECOMP_INFRA_DIRECT", Kind::Bool, "true", ...}`;
  - `docs/ENV_FLAGS.md`: `PSPRECOMP_INFRA_DIRECT`, `PSPRECOMP_INFRA_DIRECT_IMPAIR`,
    `PSPRECOMP_INFRA_DIRECT_TEST_HOSTS`;
  - the server guide: `--no-direct`, `--stun`, beta config `"direct"`;
  - `ARCHITECTURE.md`: the direct-path subsystem and why it exists.

**Interfaces:**
- Produces:
  - Settings key `[online] direct = true|false`, default `true`.
  - Label es "Conexión directa entre jugadores", en "Direct connection between players".
  - The `description` es is the spec's copy in plain text (no markdown asterisks), lines separated by `\n`:
    ```
    Activada: tu juego se conecta directo con los jugadores de tu sala, como en PSN y en la mayoría de los juegos en línea. Hay menos retraso y todos ven la misma partida.
    Desactivada: todo pasa por nuestro servidor. Se puede jugar igual, pero con jugadores lejanos puede haber más retraso y diferencias entre pantallas.
    Privacidad: para conectarse directo, los juegos de una sala intercambian su dirección de conexión, como cualquier juego en línea. Solo se comparte con tu sala mientras juegan juntos, la conexión va cifrada y no se muestra en pantalla ni se guarda en ningún registro.
    ```
  - The en description is a faithful translation of the same three lines.

- [ ] **Step 1: Write the failing tests:**
  - `options.rs`: `effective(...)["online.direct"] == "true"` with an empty ini; `"false"` is kept; `"maybe"` →
    default.
  - The Options screen test: the description renders three separate lines, each starting with "Activada",
    "Desactivada", "Privacidad", and contains "dirección de conexión".
  - psprecomp: a `test_settings` case (or the existing settings test file) where `[online] direct = false` sets
    `PSPRECOMP_INFRA_DIRECT=0` when the env is unset.
- [ ] **Step 2–4: Run → fail; implement; run → pass:**
  - launcher: `npm test`, `npm run typecheck`, `cargo test`;
  - psprecomp: `ninja && ctest`.
- [ ] **Step 5: Look at it:** build the launcher (`npm run tauri build -- --no-bundle`), open the online options
  tab, and take a screenshot. The text fits without clipping at 1280×720 and 1920×1080. Save the screenshots into
  the ledger folder.
- [ ] **Step 6: Commit** in both repos: `feat(launcher): "Conexión directa entre jugadores" option with privacy
  note` and `feat(settings): [online] direct -> PSPRECOMP_INFRA_DIRECT; docs`.

### Task 10: Lab acceptance

**Files:**
- Create: `build/infra/ipgrep.py`, a local tool. It scans files for dotted IPv4 addresses and prints each match with
  file and line, except `127.0.0.1` and `0.0.0.0`.
- Create: `build/infra/p2p_direct/REPORT.md`.
- Modify: `build/infra/p2p_baseline.sh`, which gains a `direct` switch: client `PSPRECOMP_INFRA_DIRECT_IMPAIR` with
  the same profile on each instance, and `PSPRECOMP_INFRA_DIRECT_TEST_HOSTS=127.0.0.1`.

**Interfaces:**
- Consumes: everything above; Part 1's `build/infra/p2p_baseline/REPORT.md`.

- [ ] **Step 1:** Run the `intl` and `bad` profiles with the direct path. The client impairment is
  `delay=90,jitter=15,loss=2,reorder=5` (intl) and `delay=150,jitter=40,loss=5,reorder=10` (bad). The server keeps
  the matching `--impair` for the relay leg. Expected, in `REPORT.md` next to Part 1's numbers:
  - stream delivery p99 ≤ one-way delay + 1 RTT + 50 ms (intl: ≤ 320 ms) and below the relay baseline's p99;
  - fewer gaps ≥ 250 ms than the baseline.
  - Release A (datagrams only): the same bounds on **mission datagram** delivery (intl p99 ≤ 320 ms vs the
    baseline's 473 / 329 ms), and Task 0c's meter shows a lower p95 |a−b| and time to agree than its relay "before".
- [ ] **Step 2:** A mission with the direct path killed mid-mission: impair `loss=100` switched on for 20 s
  through a lab-only debug socket command or a file the link polls in non-beta builds (the builder chooses one and
  documents it in ENV_FLAGS). The game continues over kind 4 with no disconnect; frames over 100 ms ≤ 1 per
  instance outside the 4 s detection window; the path comes back Up after the impairment ends.
- [ ] **Step 3:** Compatibility matrix, each with the co-op smoke passing:
  - one instance opted out (`PSPRECOMP_INFRA_DIRECT=0`) → that pair relayed;
  - one instance is a real protocol-2 client: the u6 beta exe from `build/beta/Patapon3Recomp/game/<id>/`
    (copied to a scratch folder, no rebuild) → that pair relayed, the others direct, no error on either side.
  - server `--no-direct` → all relayed.
- [ ] **Step 4:** Regression gates: 30 fps rt_regress PASS; `regress_vs.sh`; ad hoc smoke; the server selftest.
- [ ] **Step 5: Privacy check.**
  - `python -I build/infra/ipgrep.py build/logs build/infra/p2p_direct` (client logs, reports, stats CSVs) finds
    nothing.
  - The server DB dump has no IPv4 besides loopback.
  - The server log's lines that contain "direct" carry no address.
- [ ] **Step 6: Two machines on one LAN** (with the user, the `tester` PC): both clients show `direct: ... up (host`
  through the same-NAT host-candidate rule, and a co-op mission plays. Write the result to the ledger.
- [ ] **Step 7:** Write `REPORT.md` (Spanish summary first). No psprecomp commit (outputs live in `build/`).

### Task 11: Release through the auto-updater, then the field test

**Files:**
- Create: `build/beta/ACTUALIZACION_7.md` (Release A; `_8` for Release B), `build/beta/notas_u7.txt`,
  `build/beta/patapon3-beta1-u7.zip` + `.sha256`, `build/beta/latest.json`.
- Modify: launcher version `0.3.0-beta.2` (u7) / `0.3.0-beta.3` (u8) in `package.json`, `src-tauri/Cargo.toml`,
  `src-tauri/tauri.conf.json`; `build/beta/CHANGELOG.md` section "Actualización 7".
- User-side: deploy `tools/infra_server.py` to `tester` (backup first, ask first), restart.

- [ ] **Step 1:** Build and package by `build/beta/PUBLICAR_ACTUALIZACION.md`: launcher without
  `P3_UPDATE_PUBKEY` / `P3_NO_BETA_GATE`; game closed + diskquiet, `sh build/beta/make_beta_exes.sh` (UNTIL stays
  `2027-01-01T05:59:59Z`); `make_zip.py`; smoke from a fresh unzip. Also check that a beta exe ignores
  `PSPRECOMP_INFRA_DIRECT_IMPAIR` (its log says so).
- [ ] **Step 2: Update e2e before publishing.** Unzip u6 into `E:\p3_upd_e2e` (the Task 6 recipe of
  `2026-10-29-launcher-autoupdater.md`: test-key launchers, local `http.server`, WebView2 CDP driver
  `cdp.mjs`), serve u7 signed with the test key, and check u6 → u7 installs, protected files identical, the game
  starts and the new option shows.
- [ ] **Step 3:** Sign with the real key: `--min-version 0.3.0-beta.2` if Decision 2 = mandatory, else
  `0.3.0-beta.1`. `ACTUALIZACION_7.md` with the user's steps (R2 zip first, then `update/latest.json`; server copy
  to `tester`; `DOWNLOAD_URL` bump in `tools/beta_report_server.py`) and the player text, which explains the new
  option in the words of the launcher description and says how to turn it off.
- [ ] **Step 4:** Server deploy with the user's OK. `curl https://online.montick.app/beta` answers.
- [ ] **Step 5:** After the next beta night:
  - copy `beta.db` (read-only copy) and rerun Task 0a's report on it;
  - report the direct success rate (pairs that went direct / pairs offered), RTT direct vs relay per pair,
    fallbacks, and `link_stats` for the relay-only pairs, next to Task 0a's "before".
- [ ] **Step 6:** The user's international playtest, in the same mission as the video if possible. The boss comes
  out on every screen; hits land on every screen. Record the outcome (and a clip if the user records one) in the
  ledger.

### Task 12: Launcher leftovers from update 6 (ships in Release A)

**Files:**
- Modify (launcher, branch `direct-option`): `src-tauri/Cargo.toml`, `src/screens/Update.tsx`,
  `src/screens/Update.test.tsx`, `lang/es.json`, `lang/en.json`.

- [ ] **Step 1: Failing test** `restart failure after a good install says it is installed` (Update.test.tsx):
  `installUpdate` resolves, `restartLauncher` rejects → the screen shows `update.installed_reopen` ("La
  actualización ya se instaló. Cierra y vuelve a abrir el launcher." / "The update is installed. Close and reopen
  the launcher.") and not `update.failed`. Run → FAIL.
- [ ] **Step 2:** Implement (one catch around `restartLauncher` only); both lang files. `npm test` → PASS.
- [ ] **Step 3:** `zip` with only the decoder it needs: replace `features = ["deflate"]` by the crate's flate2-only
  deflate feature (look it up in the registry copy of zip 2.x; zopfli and deflate64 must leave `cargo tree`).
  `cargo test` → all PASS (the update_apply tests read deflated zips); the release exe is smaller (record both sizes).
- [ ] **Step 4:** Commit `fix(launcher): say the update is installed when only the restart fails; slimmer zip`.
