#!/usr/bin/env python3
"""Room server for Patapon 3's infrastructure mode (no Sony server involved).

The port answers the PSN/NP imports itself and implements sceNpMatching2 over
this server: login (online id -> session + virtual IP 10.80.0.n), room search
with the game's int filters, room details, create / join / leave / kickout,
room and signaling events pushed to the members, and forwarding of the game's
UDP datagrams between virtual IPs of the same room.

Wire format (docs/superpowers/specs/2026-10-22-p3-infrastructure-mode-design.md):
frames `u32 length (LE, of kind + body) | u8 kind | body`; kind 1 = UTF-8 JSON
(requests carry "id", answers echo it, events carry "ev"), kind 2 = datagram
`u32 vip | u16 dst_port | u16 src_port | payload`. Two listeners speak it: raw
TCP (LAN, tests) and WebSocket (one frame per binary message, path /infra) for
the public server behind a Cloudflare Tunnel.

    python -I tools/infra_server.py [--host 127.0.0.1] [--port 27320] [--ws-port 27321]
    python -I tools/infra_server.py --selftest
"""
import argparse
import base64
import hashlib
import heapq
import json
import re
import secrets
import selectors
import socket
import struct
import sys
import threading
import time

MAX_FRAME = 64 * 1024        # kind + body; a bigger frame closes the connection
MAX_TX = 1024 * 1024         # a client that does not read its queued output is dropped
MAX_HEADER = 8 * 1024        # HTTP upgrade request
MAX_DGRAM = 4096             # game datagram payload (the game sends up to 0x574)
DGRAM_PER_S = 400
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ONLINE_ID = re.compile(r"[A-Za-z0-9_-]{3,16}")
VIP_BASE = 0x0A500000        # 10.80.0.0
FIRST_MEMBER_ID = 0x10

# PSP NP Matching2 server errors: 0x80550D00 | (PS3 SCE_NP_MATCHING2_SERVER_ERROR_* & 0xFF)
BAD_REQUEST = 0x80550D01
SERVICE_UNAVAILABLE = 0x80550D02
FORBIDDEN = 0x80550D07
NO_SUCH_ROOM = 0x80550D13
ROOM_FULL = 0x80550D19
NO_SUCH_USER = 0x80550D1C
ALREADY_JOINED = 0x80550D30

# SCE_NP_MATCHING2_EVENT_CAUSE_*
CAUSE_LEAVE, CAUSE_KICKOUT, CAUSE_DISAPPEARED = 1, 2, 5
SIG_DEAD, SIG_ESTABLISHED = 0x5101, 0x5102

# int filter operators the game sends
OPS = {1: lambda a, b: a == b, 2: lambda a, b: a != b, 3: lambda a, b: a < b,
       4: lambda a, b: a <= b, 5: lambda a, b: a > b, 6: lambda a, b: a >= b}


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


class Err(Exception):
    def __init__(self, code):
        super().__init__("0x%08X" % code)
        self.code = code


def frame(kind, body):
    return struct.pack("<IB", 1 + len(body), kind) + body


def json_frame(obj):
    return frame(1, json.dumps(obj, separators=(",", ":")).encode())


def ws_wrap(payload, opcode=2, mask=None):
    n = len(payload)
    head = bytes([0x80 | opcode])
    mbit = 0x80 if mask else 0
    if n < 126:
        head += bytes([mbit | n])
    elif n < 65536:
        head += bytes([mbit | 126]) + struct.pack(">H", n)
    else:
        head += bytes([mbit | 127]) + struct.pack(">Q", n)
    if mask:
        return head + mask + ws_mask(payload, mask)
    return head + payload


def ws_mask(data, mask):
    if not data:
        return b""
    n = len(data)
    key = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "little") ^ int.from_bytes(key, "little")).to_bytes(n, "little")


def u32(v):
    v = int(v)
    if not 0 <= v <= 0xFFFFFFFF:
        raise ValueError("u32 out of range")
    return v


def attr_id(v):
    v = int(v)
    if not 0 <= v <= 0xFFFF:
        raise ValueError("attr id out of range")
    return v


def hex_bytes(s, limit):
    if not isinstance(s, str) or len(s) > 2 * limit:
        raise ValueError("bad hex")
    return bytes.fromhex(s)


def bin_attrs(items, limit=16, size=1024):
    if not isinstance(items, list) or len(items) > limit:
        raise ValueError("bad bin attrs")
    out = {}
    for i, h in items:
        out[attr_id(i)] = hex_bytes(h, size)
    return out


class Member:
    def __init__(self, conn, member_id, bins):
        self.conn = conn
        self.member_id = member_id
        self.join_ms = int(time.time() * 1000)
        self.bins = bins
        self.announced = False  # signaling ESTABLISHED sent to the older members

    def json(self):
        c = self.conn
        return {"member_id": self.member_id, "online_id": c.online_id, "vip": c.vip, "port": c.port,
                "join_ms": self.join_ms, "bins": [[i, b.hex()] for i, b in self.bins.items()]}


class Room:
    def __init__(self, room_id, owner, max_slot, flag_attr, ints, bins, bins_internal):
        self.id = room_id
        self.owner = owner          # Member
        self.max_slot = max_slot
        self.flag_attr = flag_attr
        self.ints = ints            # {attr id: u32}
        self.bins = bins            # {attr id: bytes}, external
        self.bins_internal = bins_internal
        self.members = []           # join order, owner first
        self.next_member_id = FIRST_MEMBER_ID

    def ext(self, attrs=None):
        bins = [[i, b.hex()] for i, b in self.bins.items() if not attrs or i in attrs]
        return {"id": self.id, "owner": self.owner.conn.online_id, "max_slot": self.max_slot,
                "members": len(self.members), "ints": [[i, v] for i, v in self.ints.items()], "bins": bins}

    def internal(self, me):
        return {"id": self.id, "max_slot": self.max_slot, "flag_attr": self.flag_attr,
                "members": [m.json() for m in self.members], "me": me.member_id,
                "owner": self.owner.member_id,
                "bins_internal": [[i, b.hex()] for i, b in self.bins_internal.items()]}

    def member_of(self, conn):
        for m in self.members:
            if m.conn is conn:
                return m
        return None


class Conn:
    def __init__(self, sock, kind, peer_ip):
        self.sock = sock
        self.kind = kind            # "tcp" or "ws"
        self.peer_ip = peer_ip
        self.addr_key = None        # counted address (peer ip, or CF-Connecting-IP behind the tunnel)
        self.rx = bytearray()
        self.tx = bytearray()
        self.ws_open = False
        self.ws_msg = bytearray()
        self.close_after_flush = False
        self.closed = False
        self.last = time.monotonic()
        self.bytes_in = self.bytes_out = 0
        self.online_id = None
        self.session = None
        self.vip = 0
        self.port = 0
        self.room = None
        self.dgram_window = 0.0
        self.dgram_count = 0
        self.dgram_dropped = 0

    def label(self):
        vip = socket.inet_ntoa(struct.pack(">I", self.vip)) if self.vip else "-"
        return "%s(%s %s %s)" % (self.online_id or "?", self.kind, self.addr_key or self.peer_ip, vip)


class Server:
    def __init__(self, host="127.0.0.1", port=27320, ws_port=27321, ws_path="/infra", max_rooms=64,
                 idle=30.0, stats_every=60.0, delay_ms=0, max_per_addr=4, max_conns=512, logger=log):
        self.log = logger
        self.listeners = {}
        for p, kind in ((port, "tcp"), (ws_port, "ws")):
            if p is None:
                continue
            ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                ls.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            else:
                ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            ls.bind((host, p))
            ls.listen(64)
            ls.setblocking(False)
            self.listeners[ls] = kind
        self.port = self.ws_port = None
        for ls, kind in self.listeners.items():
            if kind == "tcp":
                self.port = ls.getsockname()[1]
            else:
                self.ws_port = ls.getsockname()[1]
        self.ws_path = ws_path
        self.max_rooms = max_rooms
        self.idle = idle
        self.stats_every = stats_every
        self.delay = delay_ms / 1000.0
        self.max_per_addr = max_per_addr
        self.max_conns = max_conns
        self.sel = selectors.DefaultSelector()
        for ls in self.listeners:
            self.sel.register(ls, selectors.EVENT_READ)
        self.conns = {}         # sock -> Conn
        self.per_addr = {}      # addr key -> live connections
        self.by_vip = {}        # vip -> Conn
        self.rooms = {}         # room id -> Room
        self.next_room_id = 1
        self.delayed = []       # heap of (due, seq, conn, frame bytes)
        self.seq = 0
        self.running = True
        self.stats_t = time.monotonic()
        self.stats_in = self.stats_out = 0

    # ---- connection plumbing ----
    def count_addr(self, conn, key):
        if self.per_addr.get(key, 0) >= self.max_per_addr:
            return False
        self.per_addr[key] = self.per_addr.get(key, 0) + 1
        conn.addr_key = key
        return True

    def raw_send(self, conn, data):
        if conn.closed:
            return
        conn.tx += data
        if len(conn.tx) > MAX_TX:
            return self.drop(conn, "output queue full")
        self.flush(conn)

    def send_frame(self, conn, data):
        self.raw_send(conn, ws_wrap(data) if conn.kind == "ws" else data)

    def send_json(self, conn, obj):
        self.send_frame(conn, json_frame(obj))

    def flush(self, conn):
        if conn.closed:
            return
        try:
            while conn.tx:
                n = conn.sock.send(conn.tx)
                if n <= 0:
                    break
                conn.bytes_out += n
                self.stats_out += n
                del conn.tx[:n]
        except BlockingIOError:
            pass
        except OSError as e:
            return self.drop(conn, "send failed: %s" % e)
        if not conn.tx and conn.close_after_flush:
            return self.drop(conn, "done")
        want = selectors.EVENT_READ | (selectors.EVENT_WRITE if conn.tx else 0)
        if self.sel.get_key(conn.sock).events != want:
            self.sel.modify(conn.sock, want)

    def drop(self, conn, why):
        if conn.closed:
            return
        conn.closed = True
        if conn.room is not None:
            self.leave_room(conn, CAUSE_DISAPPEARED, b"")
        try:
            self.sel.unregister(conn.sock)
        except (KeyError, ValueError):
            pass
        conn.sock.close()
        self.conns.pop(conn.sock, None)
        if conn.addr_key is not None:
            self.per_addr[conn.addr_key] -= 1
            if not self.per_addr[conn.addr_key]:
                del self.per_addr[conn.addr_key]
        if conn.vip:
            self.by_vip.pop(conn.vip, None)
        self.log("close %s: %s (in %d B, out %d B, dropped %d datagrams)"
                 % (conn.label(), why, conn.bytes_in, conn.bytes_out, conn.dgram_dropped))

    def accept(self, ls, kind):
        while True:
            try:
                sock, addr = ls.accept()
            except (BlockingIOError, InterruptedError):
                return
            sock.setblocking(False)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn = Conn(sock, kind, addr[0])
            if len(self.conns) >= self.max_conns:
                sock.close()
                self.log("refused %s: server full" % addr[0])
                continue
            if kind == "tcp" and not self.count_addr(conn, addr[0]):
                sock.close()
                self.log("refused %s: too many connections from this address" % addr[0])
                continue
            self.conns[sock] = conn
            self.sel.register(sock, selectors.EVENT_READ)

    def on_readable(self, conn):
        try:
            data = conn.sock.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            data = b""
        if not data:
            return self.drop(conn, "connection closed")
        conn.bytes_in += len(data)
        self.stats_in += len(data)
        conn.rx += data
        if conn.kind == "tcp":
            self.parse_frames(conn)
        elif not conn.ws_open:
            self.ws_handshake(conn)
        else:
            self.parse_ws(conn)

    def parse_frames(self, conn):
        while len(conn.rx) >= 4 and not conn.closed:
            n = struct.unpack_from("<I", conn.rx)[0]
            if n == 0 or n > MAX_FRAME:
                return self.drop(conn, "bad frame length %d" % n)
            if len(conn.rx) < 4 + n:
                return
            body = bytes(conn.rx[4:4 + n])
            del conn.rx[:4 + n]
            self.on_frame(conn, body)

    # ---- WebSocket (RFC 6455, server side) ----
    def ws_handshake(self, conn):
        end = conn.rx.find(b"\r\n\r\n")
        if end < 0:
            if len(conn.rx) > MAX_HEADER:
                self.drop(conn, "HTTP header too long")
            return
        head = bytes(conn.rx[:end]).decode("latin-1")
        del conn.rx[:end + 4]
        lines = head.split("\r\n")
        parts = lines[0].split(" ")
        headers = {}
        for line in lines[1:]:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()
        path = parts[1].split("?", 1)[0] if len(parts) >= 3 else ""
        upgrade = headers.get("upgrade", "").lower() == "websocket"
        key = headers.get("sec-websocket-key", "")
        # The tunnel connects from loopback and names the real client; a direct client cannot pick its address.
        addr = conn.peer_ip
        if conn.peer_ip in ("127.0.0.1", "::1") and headers.get("cf-connecting-ip"):
            addr = headers["cf-connecting-ip"][:64]
        if parts[0] != "GET" or len(parts) < 3:
            conn.close_after_flush = True
            return self.raw_send(conn, b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        if not upgrade:
            body = b"Patapon 3 infrastructure server\n"
            conn.close_after_flush = True
            return self.raw_send(conn, b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                                       b"Connection: close\r\n\r\n%s" % (len(body), body))
        if path != self.ws_path or not key:
            conn.close_after_flush = True
            return self.raw_send(conn, b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        if not self.count_addr(conn, addr):
            self.log("refused %s: too many connections from this address" % addr)
            conn.close_after_flush = True
            return self.raw_send(conn, b"HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n"
                                       b"Connection: close\r\n\r\n")
        accept = base64.b64encode(hashlib.sha1(key.encode() + WS_GUID).digest())
        self.raw_send(conn, b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        conn.ws_open = True
        if conn.rx:
            self.parse_ws(conn)

    def parse_ws(self, conn):
        while len(conn.rx) >= 2 and not conn.closed:
            b0, b1 = conn.rx[0], conn.rx[1]
            fin, op, masked, n = b0 & 0x80, b0 & 0x0F, b1 & 0x80, b1 & 0x7F
            pos = 2
            if n == 126:
                if len(conn.rx) < 4:
                    return
                n = struct.unpack_from(">H", conn.rx, 2)[0]
                pos = 4
            elif n == 127:
                if len(conn.rx) < 10:
                    return
                n = struct.unpack_from(">Q", conn.rx, 2)[0]
                pos = 10
            if not masked:
                return self.drop(conn, "unmasked WebSocket frame")
            if n + len(conn.ws_msg) > MAX_FRAME + 4:
                return self.drop(conn, "WebSocket message too long")
            if len(conn.rx) < pos + 4 + n:
                return
            mask = bytes(conn.rx[pos:pos + 4])
            payload = ws_mask(bytes(conn.rx[pos + 4:pos + 4 + n]), mask)
            del conn.rx[:pos + 4 + n]
            conn.last = time.monotonic()
            if op == 8:
                conn.close_after_flush = True
                return self.raw_send(conn, ws_wrap(payload[:2], 8))
            if op == 9:
                self.raw_send(conn, ws_wrap(payload[:125], 10))
                continue
            if op == 10:
                continue
            if op not in (0, 2):
                return self.drop(conn, "WebSocket opcode %d" % op)
            conn.ws_msg += payload
            if not fin:
                continue
            msg = bytes(conn.ws_msg)
            conn.ws_msg.clear()
            if len(msg) < 5 or struct.unpack_from("<I", msg)[0] != len(msg) - 4:
                return self.drop(conn, "WebSocket message is not one frame")
            self.on_frame(conn, msg[4:])

    # ---- frames ----
    def on_frame(self, conn, body):
        conn.last = time.monotonic()
        kind, body = body[0], body[1:]
        if kind == 2:
            return self.on_datagram(conn, body)
        if kind != 1:
            return self.drop(conn, "unknown frame kind %d" % kind)
        try:
            msg = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            self.log("%s: malformed JSON frame skipped" % conn.label())
            return
        if not isinstance(msg, dict):
            return
        rid = msg.get("id")
        op = msg.get("op")
        handler = getattr(self, "op_" + op, None) if isinstance(op, str) and op.isidentifier() else None
        try:
            if handler is None:
                raise Err(BAD_REQUEST)
            if op != "login" and conn.session is None:
                raise Err(FORBIDDEN)
            res = handler(conn, msg)
        except Err as e:
            if op not in ("ping",):
                self.log("%s %s -> error 0x%08X" % (conn.label(), op, e.code))
            return self.send_json(conn, {"id": rid, "ok": False, "error": e.code})
        except (KeyError, TypeError, ValueError, AttributeError) as e:
            self.log("%s %s -> bad request (%s)" % (conn.label(), op, e))
            return self.send_json(conn, {"id": rid, "ok": False, "error": BAD_REQUEST})
        if res is None:
            return  # answered later (delayed ping)
        answer = {"id": rid, "ok": True}
        answer.update(res)
        self.send_json(conn, answer)

    def later(self, conn, data):
        self.seq += 1
        heapq.heappush(self.delayed, (time.monotonic() + self.delay, self.seq, conn, data))

    def on_datagram(self, conn, body):
        if conn.session is None or len(body) < 8 or len(body) - 8 > MAX_DGRAM:
            conn.dgram_dropped += 1
            return
        now = time.monotonic()
        if now - conn.dgram_window >= 1.0:
            conn.dgram_window, conn.dgram_count = now, 0
        conn.dgram_count += 1
        if conn.dgram_count > DGRAM_PER_S:
            conn.dgram_dropped += 1
            return
        vip, dst_port, src_port = struct.unpack_from("<IHH", body)
        dst = self.by_vip.get(vip)
        if dst is None or conn.room is None or dst.room is not conn.room or dst is conn:
            conn.dgram_dropped += 1
            return
        out = frame(2, struct.pack("<IHH", conn.vip, dst_port, src_port) + body[8:])
        if self.delay:
            self.later(dst, out)
        else:
            self.send_frame(dst, out)

    # ---- requests ----
    def op_login(self, conn, msg):
        if conn.session is not None:
            raise Err(FORBIDDEN)
        oid = msg["online_id"]
        if not isinstance(oid, str) or not ONLINE_ID.fullmatch(oid):
            raise Err(BAD_REQUEST)
        n = 1
        while VIP_BASE + n in self.by_vip:
            n += 1
        if n > 0xFFFE:
            raise Err(SERVICE_UNAVAILABLE)
        conn.online_id, conn.session, conn.vip = oid, secrets.token_hex(8), VIP_BASE + n
        self.by_vip[conn.vip] = conn
        self.log("login %s" % conn.label())
        return {"session": conn.session, "vip": conn.vip}

    def op_ping(self, conn, msg):
        answer = {"id": msg.get("id"), "ok": True, "t": msg.get("t")}
        if self.delay:
            self.later(conn, json_frame(answer))
            return None
        return {"t": msg.get("t")}

    def op_bound(self, conn, msg):
        port = int(msg["port"])
        if not 1 <= port <= 0xFFFF:
            raise Err(BAD_REQUEST)
        conn.port = port
        self.log("%s bound game port %d" % (conn.label(), port))
        if conn.room is not None:
            self.announce(conn.room, conn.room.member_of(conn))
        return {}

    def match(self, room, filters):
        for op, aid, value in filters:
            if not OPS[op](room.ints.get(aid, 0), value):
                return False
        return True

    def op_search(self, conn, msg):
        filters = []
        for op, aid, value in msg.get("ints", [])[:16]:
            op = int(op)
            if op not in OPS:
                raise Err(BAD_REQUEST)
            filters.append((op, attr_id(aid), u32(value)))
        attrs = {attr_id(a) for a in msg.get("attrs", [])[:16]}
        start = max(1, int(msg.get("start", 1)))
        count = max(0, min(20, int(msg.get("max", 20))))
        found = [r for r in self.rooms.values() if self.match(r, filters)]
        page = found[start - 1:start - 1 + count]
        self.log("%s search (%d filters) -> %d of %d rooms" % (conn.label(), len(filters), len(page), len(found)))
        return {"total": len(found), "rooms": [r.ext(attrs) for r in page]}

    def op_details(self, conn, msg):
        attrs = {attr_id(a) for a in msg.get("attrs", [])[:16]}
        rooms = []
        for rid in msg["room_ids"][:20]:
            r = self.rooms.get(int(rid))
            if r is not None:
                rooms.append(r.ext(attrs))
        self.log("%s details %d -> %d rooms" % (conn.label(), len(msg["room_ids"]), len(rooms)))
        return {"rooms": rooms}

    def op_create(self, conn, msg):
        if conn.room is not None:
            raise Err(ALREADY_JOINED)
        if len(self.rooms) >= self.max_rooms:
            raise Err(SERVICE_UNAVAILABLE)
        max_slot = int(msg["max_slot"])
        if not 1 <= max_slot <= 16:
            raise Err(BAD_REQUEST)
        ints = msg.get("ints", [])
        if len(ints) > 32:
            raise Err(BAD_REQUEST)
        ints = {attr_id(i): u32(v) for i, v in ints}
        room = Room(self.next_room_id, None, max_slot, u32(msg.get("flag_attr", 0)), ints,
                    bin_attrs(msg.get("bins", [])), bin_attrs(msg.get("bins_internal", [])))
        member_bins = bin_attrs(msg.get("member_bins", []))
        self.next_room_id += 1
        owner = Member(conn, room.next_member_id, member_bins)
        owner.announced = True
        room.next_member_id += 1
        room.owner = owner
        room.members.append(owner)
        self.rooms[room.id] = room
        conn.room = room
        self.log("%s create room %d (%d slots, %d int, %d bin attrs)"
                 % (conn.label(), room.id, max_slot, len(ints), len(room.bins)))
        return {"room": room.internal(owner)}

    def op_join(self, conn, msg):
        if conn.room is not None:
            raise Err(ALREADY_JOINED)
        room = self.rooms.get(int(msg["room_id"]))
        if room is None:
            raise Err(NO_SUCH_ROOM)
        if len(room.members) >= room.max_slot:
            raise Err(ROOM_FULL)
        opt = hex_bytes(msg.get("opt", ""), 16)
        member = Member(conn, room.next_member_id, bin_attrs(msg.get("member_bins", [])))
        room.next_member_id += 1
        olds = list(room.members)
        room.members.append(member)
        conn.room = room
        self.log("%s join room %d as 0x%X (%d/%d)" % (conn.label(), room.id, member.member_id,
                                                         len(room.members), room.max_slot))
        answer = {"id": msg.get("id"), "ok": True, "room": room.internal(member)}
        self.send_json(conn, answer)
        for m in olds:
            self.send_json(m.conn, {"ev": "member_joined", "room_id": room.id, "member": member.json(),
                                    "opt": opt.hex()})
            if m.conn.port:
                self.signal(conn, room, m, SIG_ESTABLISHED)
        self.announce(room, member)
        return None

    def signal(self, to, room, about, event):
        self.send_json(to, {"ev": "signaling", "room_id": room.id, "member_id": about.member_id,
                            "event": event, "vip": about.conn.vip, "port": about.conn.port})

    def announce(self, room, member):
        """Signaling ESTABLISHED to the older members once the newcomer's game socket is bound."""
        if member is None or member.announced or not member.conn.port:
            return
        member.announced = True
        for m in room.members:
            if m is not member:
                self.signal(m.conn, room, member, SIG_ESTABLISHED)

    def leave_room(self, conn, cause, opt):
        room = conn.room
        conn.room = None
        member = room.member_of(conn)
        room.members.remove(member)
        if member is room.owner:
            del self.rooms[room.id]
            for m in room.members:
                m.conn.room = None
                self.send_json(m.conn, {"ev": "room_destroyed", "room_id": room.id, "cause": cause,
                                        "opt": opt.hex()})
            self.log("%s left room %d: room destroyed (%d members told)"
                     % (conn.label(), room.id, len(room.members)))
            return
        for m in room.members:
            self.send_json(m.conn, {"ev": "member_left", "room_id": room.id, "member": member.json(),
                                    "cause": cause, "opt": opt.hex()})
            if member.announced:
                self.signal(m.conn, room, member, SIG_DEAD)
        self.log("%s left room %d (cause %d, %d left)" % (conn.label(), room.id, cause, len(room.members)))

    def op_leave(self, conn, msg):
        if conn.room is None or conn.room.id != int(msg["room_id"]):
            raise Err(NO_SUCH_ROOM)
        self.leave_room(conn, CAUSE_LEAVE, hex_bytes(msg.get("opt", ""), 16))
        return {}

    def op_kick(self, conn, msg):
        room = conn.room
        if room is None or room.id != int(msg["room_id"]):
            raise Err(NO_SUCH_ROOM)
        if room.owner.conn is not conn:
            raise Err(FORBIDDEN)
        target = next((m for m in room.members if m.member_id == int(msg["member_id"])), None)
        if target is None or target is room.owner:
            raise Err(NO_SUCH_USER)
        opt = hex_bytes(msg.get("opt", ""), 16)
        self.send_json(target.conn, {"ev": "kicked", "room_id": room.id, "cause": CAUSE_KICKOUT, "opt": opt.hex()})
        self.leave_room(target.conn, CAUSE_KICKOUT, opt)
        return {}

    def op_set_internal(self, conn, msg):
        room = conn.room
        if room is None or room.id != int(msg["room_id"]):
            raise Err(NO_SUCH_ROOM)
        room.bins_internal.update(bin_attrs(msg.get("bins", [])))
        if "flag_attr" in msg:
            room.flag_attr = u32(msg["flag_attr"])
        return {}

    # ---- loop ----
    def tick(self, now):
        while self.delayed and self.delayed[0][0] <= now:
            _, _, conn, data = heapq.heappop(self.delayed)
            self.send_frame(conn, data)
        for conn in list(self.conns.values()):
            if now - conn.last > self.idle:
                self.drop(conn, "idle")
        if self.stats_every and now - self.stats_t >= self.stats_every:
            dt = now - self.stats_t
            sessions = sum(1 for c in self.conns.values() if c.session)
            self.log("traffic: %d sessions, up %.1f kB/s, down %.1f kB/s, %d rooms"
                     % (sessions, self.stats_out / dt / 1024, self.stats_in / dt / 1024, len(self.rooms)))
            self.stats_t, self.stats_in, self.stats_out = now, 0, 0

    def serve(self):
        while self.running:
            for key, mask in self.sel.select(timeout=0.02 if self.delayed else 0.2):
                kind = self.listeners.get(key.fileobj)
                if kind is not None:
                    self.accept(key.fileobj, kind)
                    continue
                conn = self.conns.get(key.fileobj)
                if conn is None:
                    continue
                if mask & selectors.EVENT_WRITE:
                    self.flush(conn)
                if mask & selectors.EVENT_READ and not conn.closed:
                    self.on_readable(conn)
            self.tick(time.monotonic())
        for conn in list(self.conns.values()):
            self.drop(conn, "server stopping")
        self.sel.close()
        for ls in self.listeners:
            ls.close()


# ---- selftest ----
class TestClient:
    def __init__(self, port, ws=False, path="/infra", headers=""):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.ws = ws
        self.rx = b""
        self.events, self.dgrams, self.answers = [], [], {}
        self.next_id = 0
        if ws:
            key = base64.b64encode(secrets.token_bytes(16))
            self.sock.sendall(b"GET %s HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              b"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s\r\n"
                              % (path.encode(), key, headers.encode()))
            while b"\r\n\r\n" not in self.rx:
                chunk = self.sock.recv(4096)
                assert chunk, "no HTTP answer"
                self.rx += chunk
            head, self.rx = self.rx.split(b"\r\n\r\n", 1)
            self.status = int(head.split(b" ")[1])
            if self.status == 101:
                want = base64.b64encode(hashlib.sha1(key + WS_GUID).digest())
                assert b"Sec-WebSocket-Accept: " + want in head, "bad accept key"

    def send_raw_frame(self, data):
        self.sock.sendall(ws_wrap(data, 2, secrets.token_bytes(4)) if self.ws else data)

    def send(self, obj):
        self.send_raw_frame(json_frame(obj))

    def dgram(self, vip, dst_port, src_port, payload):
        self.send_raw_frame(frame(2, struct.pack("<IHH", vip, dst_port, src_port) + payload))

    def _read(self, n):
        while len(self.rx) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.rx += chunk
        out, self.rx = self.rx[:n], self.rx[n:]
        return out

    def read_frame(self):
        if self.ws:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            msg = self._read(n)
            assert b0 & 0x0F == 2, "server sent opcode %d" % (b0 & 0x0F)
            assert struct.unpack_from("<I", msg)[0] == len(msg) - 4, "one frame per WS message"
            return msg[4], msg[5:]
        n = struct.unpack("<I", self._read(4))[0]
        body = self._read(n)
        return body[0], body[1:]

    def pump(self, timeout):
        self.sock.settimeout(timeout)
        try:
            kind, body = self.read_frame()
        except socket.timeout:
            return False
        finally:
            self.sock.settimeout(3)
        if kind == 2:
            vip, dst, src = struct.unpack_from("<IHH", body)
            self.dgrams.append((vip, dst, src, body[8:]))
        else:
            msg = json.loads(body)
            if "ev" in msg:
                self.events.append(msg)
            else:
                self.answers[msg["id"]] = msg
        return True

    def req(self, op, **kw):
        self.next_id += 1
        rid = self.next_id
        kw.update(op=op, id=rid)
        self.send(kw)
        deadline = time.monotonic() + 3
        while rid not in self.answers:
            assert time.monotonic() < deadline, "no answer to %s" % op
            self.pump(0.2)
        return self.answers.pop(rid)

    def ok(self, op, **kw):
        a = self.req(op, **kw)
        assert a["ok"], "%s failed: %r" % (op, a)
        return a

    def event(self, pred, timeout=3, what="event"):
        deadline = time.monotonic() + timeout
        while True:
            for e in self.events:
                if pred(e):
                    self.events.remove(e)
                    return e
            assert time.monotonic() < deadline, "no %s (have %r)" % (what, self.events)
            self.pump(0.1)

    def is_closed(self, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self.pump(0.1)
            except (ConnectionError, OSError):
                return True
        return False

    def close(self):
        self.sock.close()


GAME_INTS = [[0x4C, 0], [0x4D, 0x30005], [0x4E, 1], [0x4F, 1], [0x50, 40], [0x51, 1], [0x52, 1]]
GAME_FILTERS = [[1, 0x4C, 0], [1, 0x4D, 0x30005], [2, 0x4E, 0], [4, 0x4F, 1], [6, 0x50, 1], [6, 0x51, 1],
                [4, 0x52, 40]]


def selftest():
    lines = []

    def quiet(msg):
        lines.append(msg)

    def run(srv):
        th = threading.Thread(target=srv.serve, daemon=True)
        th.start()
        return th

    srv = Server("127.0.0.1", 0, 0, max_per_addr=16, logger=quiet)
    try:
        Server("127.0.0.1", srv.port, None, logger=quiet)
        raise AssertionError("a second server bound the same port")
    except OSError:
        pass
    th = run(srv)
    a, b = TestClient(srv.port), TestClient(srv.port)
    la, lb = a.ok("login", online_id="P3A"), b.ok("login", online_id="P3B")
    assert (la["vip"], lb["vip"]) == (0x0A500001, 0x0A500002), (la, lb)
    assert la["session"] != lb["session"]
    assert a.ok("ping", t=123)["t"] == 123

    hero = "Edson".encode("utf-16-le").ljust(94, b"\0").hex()
    oid = b"P3A".ljust(80, b"\0").hex()
    room = a.ok("create", max_slot=4, flag_attr=0x04000000, ints=GAME_INTS, bins=[[0x55, hero], [0x56, oid]],
                member_bins=[])["room"]
    assert room["me"] == room["owner"] == 0x10 and len(room["members"]) == 1, room
    a.ok("bound", port=3658)
    s = b.ok("search", world=0, start=1, max=20, ints=GAME_FILTERS, attrs=[0x55, 0x56])
    assert s["total"] == 1 and len(s["rooms"]) == 1, s
    ext = s["rooms"][0]
    assert ext["owner"] == "P3A" and ext["members"] == 1 and ext["max_slot"] == 4, ext
    assert dict((i, h) for i, h in ext["bins"]) == {0x55: hero, 0x56: oid}, ext
    other = [[1, 0x4D, 0x30003]] + GAME_FILTERS[2:]
    assert b.ok("search", ints=[[1, 0x4C, 0]] + other)["total"] == 0, "purpose filter"
    assert b.ok("search", ints=[[6, 0x50, 41]])["total"] == 0, "level filter"
    d = b.ok("details", room_ids=[room["id"], 999], attrs=[0x55])
    assert len(d["rooms"]) == 1 and [x[0] for x in d["rooms"][0]["bins"]] == [0x55], d

    w = TestClient(srv.ws_port, ws=True)
    assert w.status == 101
    assert w.ok("login", online_id="P3W")["vip"] == 0x0A500003
    sw = w.ok("search", ints=GAME_FILTERS, attrs=[0x55, 0x56])
    assert sw["total"] == 1 and sw["rooms"][0]["owner"] == "P3A", sw
    assert w.ok("ping", t=7)["t"] == 7

    jb = b.ok("join", room_id=room["id"], member_bins=[[0x57, "00ff"]], opt="00" * 15 + "01")["room"]
    assert jb["me"] == 0x11 and jb["owner"] == 0x10 and [m["member_id"] for m in jb["members"]] == [0x10, 0x11]
    assert jb["members"][0]["port"] == 3658 and jb["members"][0]["vip"] == 0x0A500001, jb
    ev = a.event(lambda e: e["ev"] == "member_joined", what="member_joined")
    assert ev["member"]["member_id"] == 0x11 and ev["member"]["online_id"] == "P3B", ev
    assert ev["opt"] == "00" * 15 + "01", ev
    sig = b.event(lambda e: e["ev"] == "signaling", what="B signaling")
    assert sig["member_id"] == 0x10 and sig["event"] == SIG_ESTABLISHED and sig["port"] == 3658, sig
    b.ok("bound", port=3659)
    sig = a.event(lambda e: e["ev"] == "signaling", what="A signaling")
    assert sig["member_id"] == 0x11 and sig["event"] == SIG_ESTABLISHED and sig["vip"] == 0x0A500002, sig
    assert sig["port"] == 3659, sig

    payload = bytes(range(256)) * 5 + b"x" * 116  # 1396 bytes like the game
    a.dgram(0x0A500002, 3659, 3658, payload)
    deadline = time.monotonic() + 3
    while not b.dgrams:
        assert time.monotonic() < deadline, "datagram not forwarded"
        b.pump(0.1)
    assert b.dgrams.pop() == (0x0A500001, 3659, 3658, payload)
    a.dgram(0x0A500003, 1, 1, b"not in my room")  # W is not in the room: dropped
    assert not w.pump(0.3) and not w.dgrams

    c, dd, e = TestClient(srv.port), TestClient(srv.port), TestClient(srv.port)
    for i, cl in enumerate((c, dd, e)):
        cl.ok("login", online_id="P3" + "CDE"[i])
    assert c.ok("join", room_id=room["id"])["room"]["me"] == 0x12
    assert dd.ok("join", room_id=room["id"])["room"]["me"] == 0x13
    full = e.req("join", room_id=room["id"])
    assert not full["ok"] and full["error"] == ROOM_FULL, full
    gone = e.req("join", room_id=4242)
    assert not gone["ok"] and gone["error"] == NO_SUCH_ROOM, gone
    assert e.ok("search", ints=GAME_FILTERS)["rooms"][0]["members"] == 4

    b.close()
    for cl in (a, c, dd):
        left = cl.event(lambda x: x["ev"] == "member_left", what="member_left")
        assert left["member"]["member_id"] == 0x11 and left["cause"] == CAUSE_DISAPPEARED, left
    assert a.event(lambda x: x["ev"] == "signaling" and x["event"] == SIG_DEAD)["member_id"] == 0x11
    a.ok("leave", room_id=room["id"], opt="")
    for cl in (c, dd):
        gone = cl.event(lambda x: x["ev"] == "room_destroyed", what="room_destroyed")
        assert gone["room_id"] == room["id"] and gone["cause"] == CAUSE_LEAVE, gone
    assert e.ok("search", ints=GAME_FILTERS)["total"] == 0, "room gone after the owner left"
    # Kickout: A hosts again, C joins, A kicks C.
    room2 = a.ok("create", max_slot=4, ints=GAME_INTS)["room"]
    c.ok("join", room_id=room2["id"])
    a.event(lambda x: x["ev"] == "member_joined")
    bad = c.req("kick", room_id=room2["id"], member_id=0x10)
    assert not bad["ok"] and bad["error"] == FORBIDDEN, bad
    a.ok("kick", room_id=room2["id"], member_id=0x11, opt="00" * 15 + "02")
    k = c.event(lambda x: x["ev"] == "kicked", what="kicked")
    assert k["cause"] == CAUSE_KICKOUT and k["opt"].endswith("02"), k
    assert a.event(lambda x: x["ev"] == "member_left")["cause"] == CAUSE_KICKOUT
    a.ok("set_internal", room_id=room2["id"], bins=[[0x57, "aa"]], flag_attr=1)
    for cl in (a, c, dd, e, w):
        cl.close()
    srv.running = False
    th.join(timeout=2)

    # ---- limits, on a server with the public defaults ----
    srv = Server("127.0.0.1", 0, 0, idle=1, stats_every=1, logger=quiet)
    th = run(srv)
    x = TestClient(srv.port)
    x.send_raw_frame(struct.pack("<IB", 70 * 1024, 1))
    assert x.is_closed(), "a 70 KiB frame closes the connection"
    y = TestClient(srv.port)
    bad = y.req("login", online_id="a b")
    assert not bad["ok"] and bad["error"] == BAD_REQUEST, bad
    y.close()
    time.sleep(0.2)
    four = [TestClient(srv.port) for _ in range(4)]
    assert TestClient(srv.port).is_closed(), "5th connection from one address refused"
    for cl in four:
        cl.close()
    behind = [TestClient(srv.ws_port, ws=True, headers="CF-Connecting-IP: 203.0.113.7\r\n") for _ in range(4)]
    assert all(cl.status == 101 for cl in behind)
    assert TestClient(srv.ws_port, ws=True, headers="CF-Connecting-IP: 203.0.113.7\r\n").status == 429
    assert TestClient(srv.ws_port, ws=True, headers="CF-Connecting-IP: 198.51.100.1\r\n").status == 101
    for cl in behind:
        cl.close()
    time.sleep(0.2)
    h, g = TestClient(srv.port), TestClient(srv.port)
    h.ok("login", online_id="HOST")
    g.ok("login", online_id="GUEST")
    r = h.ok("create", max_slot=4, ints=GAME_INTS)["room"]
    again = h.req("create", max_slot=4)
    assert not again["ok"], "one room per session"
    g.ok("join", room_id=r["id"])
    h.event(lambda ev: ev["ev"] == "member_joined")
    burst = b"".join(frame(2, struct.pack("<IHH", 0x0A500001, 1, 1) + b"d") for _ in range(500))
    g.send_raw_frame(burst)
    deadline = time.monotonic() + 0.5  # well inside --idle 1: H sends nothing meanwhile
    while time.monotonic() < deadline:
        h.pump(0.05)
    assert 0 < len(h.dgrams) <= DGRAM_PER_S, "rate limit forwarded %d" % len(h.dgrams)
    # G goes silent; H keeps pinging and sees G's session time out of the room.
    deadline = time.monotonic() + 3
    while not any(ev["ev"] == "member_left" for ev in h.events):
        assert time.monotonic() < deadline, "idle member not removed"
        h.ok("ping", t=0)
        h.pump(0.2)
    assert g.is_closed(1), "idle connection closed"
    assert any(l.startswith("traffic: ") and " sessions, up " in l and "kB/s, down " in l for l in lines), \
        "traffic summary line"
    h.close()
    srv.running = False
    th.join(timeout=2)

    # ---- --delay-ms adds latency to datagrams and ping answers ----
    srv = Server("127.0.0.1", 0, None, delay_ms=150, logger=quiet)
    th = run(srv)
    p = TestClient(srv.port)
    p.ok("login", online_id="SLOW")
    t0 = time.monotonic()
    p.ok("ping", t=1)
    assert time.monotonic() - t0 >= 0.14, "delayed ping"
    p.close()
    srv.running = False
    th.join(timeout=2)
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27320, help="raw TCP listener")
    ap.add_argument("--ws-port", type=int, default=27321, help="WebSocket listener (behind the tunnel)")
    ap.add_argument("--ws-path", default="/infra")
    ap.add_argument("--max-rooms", type=int, default=64)
    ap.add_argument("--idle", type=float, default=30.0, help="seconds without a frame before a session closes")
    ap.add_argument("--stats-every", type=float, default=60.0)
    ap.add_argument("--delay-ms", type=int, default=0, help="extra latency on datagrams and pings (tests)")
    ap.add_argument("--max-per-addr", type=int, default=4)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    srv = Server(args.host, args.port, args.ws_port, args.ws_path, args.max_rooms, args.idle, args.stats_every,
                 args.delay_ms, args.max_per_addr)
    log("infrastructure server: tcp %s:%d, ws %s:%d%s" % (args.host, srv.port, args.host, srv.ws_port, args.ws_path))
    try:
        srv.serve()
    except KeyboardInterrupt:
        srv.running = False


if __name__ == "__main__":
    sys.exit(main())
