# Ad hoc relay (aemu_postoffice) — plan

Date: 2026-10-11. Status: DONE except play across machines. Verified through tools/postoffice_server.py: two runtime instances (lobby, co-op quest with FEVER) and PPSSPP 1.20.4 in relay mode (`AdhocServerRelayMode = 1`, server 127.0.0.3) hosting the runtime (lobby, co-op quest, the runtime's hero marching on PPSSPP's screen).

## Why

Paragate (patapon.net) and other PPSSPP ad hoc servers that list `data_mode: AemuPostoffice`
carry all PDP/PTP traffic through a relay ("postoffice") on the ad hoc server host, TCP 27313,
instead of peer-to-peer UDP/TCP. A PPSSPP player in relay mode never opens direct sockets, so the
runtime must speak the same relay protocol to play with them, and it is also what makes play work
behind NAT. Direct P2P mode (verified 2026-10-11 against PPSSPP 1.20.4) stays the default.

## Protocol (from aemu_postoffice `design.md` / `aemu_postoffice_packets.h`, PPSSPP 1.20.4 usage)

All integers little endian. MAC fields are 8 bytes (6 used + 2 padding).
Ports on the wire are PSP port + port offset (PPSSPP `offset_port_simple`: 0 stays 0, a sum of
65536 becomes 65535); received ports are converted back by subtracting the offset.

- One TCP connection per PSP socket. First client message: `init` (24 bytes):
  `i32 type (0 PDP, 1 PTP_LISTEN, 2 PTP_CONNECT, 3 PTP_ACCEPT) | src mac[8] | u16 sport | dst mac[8] | u16 dport`.
- PDP: client -> server frames `mac[8] (destination) | u16 port | u32 size | data`; the server
  forwards to the PDP session registered as (dst mac, port) with the header rewritten to the
  sender's (mac, registered port). Max 10 KiB per datagram.
- PTP listen: init LISTEN (own mac, port). The server sends a 10-byte `mac[8] | u16 port` request
  per incoming connect.
- PTP connect: init CONNECT (own mac, sport, peer mac, dport). The server looks for the listener
  (retries ~2 s, then closes); on accept the client receives a 10-byte ack, then data mode.
- PTP accept: for each request, a NEW connection with init ACCEPT (own mac, listen port, requester
  mac, requester port); the server answers with a 10-byte ack (requester mac/port) and both sides
  enter data mode.
- PTP data: frames `u32 size | data` (max 50 KiB). A recv delivers at most one frame's remaining
  bytes; a frame larger than the buffer is delivered over several recvs (client `DATA_TRUNC`).

## Design

- `net/relay_protocol.{h,cpp}` (pure, unit-tested): encoders for init / PDP header / PTP header,
  `parse_connect`, `PdpFrames` (byte accumulator -> datagrams with peek size) and `PtpFrames`
  (byte accumulator -> payload bytes, one frame per read, remainder kept).
- `pdp::Resolver` gains `relay` (bool), `relay_host`, `relay_port` (27313) and `self_mac`. In relay
  mode `pdp::Sockets` and `ptp::Sockets` keep their slot/state machines and error codes but the
  slot fd is a TCP connection to the relay: create/listen connect and send init; send/recv use the
  framing; accept reads requests and opens the ACCEPT connection; connect sends CONNECT and waits
  for the ack (nonblocking: WOULD_BLOCK until it arrives). No peer-IP filtering in relay mode
  (the relay only forwards between registered sessions), as in PPSSPP.
- Config: `PSPRECOMP_ADHOC_RELAY=1` turns it on (default off). The relay host is the PRO server
  host. `PSPRECOMP_ADHOC_RELAY_PORT` overrides 27313 (tests).
- `tools/postoffice_server.py`: small relay server written from the protocol above (PDP forwarding,
  PTP listen/connect/accept pairing, connect retries) with `--selftest`, for local tests.

## Tasks

1. relay_protocol + tests (`test_relay_protocol`).
2. tools/postoffice_server.py + selftest.
3. Config + Resolver fields + wiring in `psp_net_wire_pdp` (+ test in test_adhoc_config).
4. PDP relay mode in pdp::Sockets.
5. PTP relay mode in ptp::Sockets.
6. Verify: two runtime instances in relay mode (lobby, chat, co-op quest) through
   adhoc_server.py + postoffice_server.py; then the runtime with PPSSPP 1.20.4
   (`AdhocServerRelayMode = 1`).
7. Docs: README, ENV_FLAGS, DEBUGGING ad hoc notes.
