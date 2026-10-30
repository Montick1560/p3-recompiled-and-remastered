#!/usr/bin/env python3
"""Room server for Patapon 3's infrastructure mode (no Sony server involved).

The port answers the PSN/NP imports itself and implements sceNpMatching2 over
this server: login (online id -> session + virtual IP 10.80.0.n), room search
with the game's int filters, room details, create / join / leave / kickout,
room and signaling events pushed to the members, and forwarding of the game's
UDP datagrams between virtual IPs that share a room. A connection can be in up to
four rooms at once (the game keeps its hideout room while it hosts or joins a VS
room); room flags (HIDDEN, CLOSED, FULL) apply to search and join as on PSN.

Wire format (docs/superpowers/specs/2026-10-22-p3-infrastructure-mode-design.md):
frames `u32 length (LE, of kind + body) | u8 kind | body`; kind 1 = UTF-8 JSON
(requests carry "id", answers echo it, events carry "ev"), kind 2 = datagram
`u32 vip | u16 dst_port | u16 src_port | payload`, kind 3 = game stream segment (same header), kind 4 =
direct-path channel packet relayed between paired players (`u32 vip | opaque sealed bytes`). Protocol 3 adds the
direct-path signaling: login caps, direct_offer, direct_peer / direct_drop events with a per-pair key, and
direct_stats (docs/superpowers/specs/2026-10-24-p3-direct-p2p-design.md). Two listeners speak it: raw
TCP (LAN, tests) and WebSocket (one frame per binary message, path /infra) for
the public server behind a Cloudflare Tunnel.

    python -I tools/infra_server.py [--host 127.0.0.1] [--port 27320] [--ws-port 27321]
    python -I tools/infra_server.py --selftest
"""
import argparse
import base64
import calendar
import hashlib
import heapq
import ipaddress
import json
import os
import queue
import random
import re
import secrets
import selectors
import socket
import sqlite3
import struct
import sys
import threading
import time

MAX_FRAME = 64 * 1024        # kind + body; a bigger frame closes the connection
MAX_TX = 1024 * 1024         # a client that does not read its queued output is dropped
MAX_HEADER = 8 * 1024        # HTTP upgrade request
MAX_DGRAM = 4096             # game datagram payload (the game sends up to 0x574)
DGRAM_PER_S = 1500           # per connection: a host with 3 peers sent over 400/s at the beta
STREAM_BYTES_PER_S = 2 * 1024 * 1024  # game stream sockets (kind 3): over this the session is closed
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ONLINE_ID = re.compile(r"[A-Za-z0-9_-]{3,16}")
CONSOLE_ID = re.compile(r"[0-9a-f]{12}")  # the 6-byte MAC that starts the OpenPSID
VIP_BASE = 0x0A500000        # 10.80.0.0
FIRST_MEMBER_ID = 0x10
MAX_ROOMS_PER_CONN = 4       # the game stays in its hideout room while it hosts or joins a VS room
MAX_HTTP_BODY = 4096         # POST /beta/check
INFRA_PROTO = 2              # login field "proto" of runtimes that speak this wire protocol (beta 1)
BETA_CONNS_PER_ADDR = 64     # with a beta config: connections per address at the handshake (staff load tests)
MAX_ROOMS_PER_ADDR = 8       # with a beta config: rooms owned by invitee sessions of one address
CHECKS_PER_MIN = 30          # POST /beta/check per address
DEFAULT_STUN = ["stun.cloudflare.com:3478", "stun.l.google.com:19302"]
MAX_CANDS = 4                # direct_offer candidates
OFFERS_PER_MIN = 4           # direct_offer per connection
MAX_CHANNEL = 1500           # kind 4 opaque bytes
MAX_DIRECT_PAIRS = 8         # direct_stats entries
DIRECT_COLUMNS = ("rtt_p50", "rtt_p95", "loss_x10", "direct_s", "fallbacks")
# Relay-route numbers a u8 client adds to each direct_stats entry (NULL from u7 clients): p50/p95 (rank rounded
# down, as the route choice uses them) of the last 20 relay samples of the last direct Up span, and the seconds the
# direct path was Up with the relay preferred.
RELAY_COLUMNS = ("relay_p50", "relay_p95", "relay_s")

# SCE_NP_MATCHING2_ROOM_FLAG_ATTR_* (RPCS3 sceNp2.h)
FLAG_CLOSED, FLAG_FULL, FLAG_HIDDEN = 0x40000000, 0x20000000, 0x10000000

# PSP NP Matching2 server errors: 0x80550D00 | (PS3 SCE_NP_MATCHING2_SERVER_ERROR_* & 0xFF)
BAD_REQUEST = 0x80550D01
SERVICE_UNAVAILABLE = 0x80550D02
FORBIDDEN = 0x80550D07
DUPLICATE_CONSOLE = FORBIDDEN  # a member of the room already has this console id
NO_SUCH_ROOM = 0x80550D13
ROOM_FULL = 0x80550D19
NO_SUCH_USER = 0x80550D1C
ALREADY_JOINED = 0x80550D30
# Our own (beta gate), in the same range so the runtime treats them as login failures.
INVITE_UNKNOWN = 0x80550DA1
INVITE_DISABLED = 0x80550DA2
BETA_CLOSED = 0x80550DA3
OUTDATED = 0x80550DA4
INVITE_IN_USE = 0x80550DA5
REFUSAL = {INVITE_UNKNOWN: "invite unknown", INVITE_DISABLED: "invite disabled", BETA_CLOSED: "beta closed",
           OUTDATED: "outdated runtime", INVITE_IN_USE: "invite in use",
           SERVICE_UNAVAILABLE: "too many sessions from this address"}

# SCE_NP_MATCHING2_EVENT_CAUSE_*
CAUSE_LEAVE, CAUSE_KICKOUT, CAUSE_DISAPPEARED = 1, 2, 5
SIG_DEAD, SIG_ESTABLISHED = 0x5101, 0x5102

# int filter operators the game sends
OPS = {1: lambda a, b: a == b, 2: lambda a, b: a != b, 3: lambda a, b: a < b,
       4: lambda a, b: a <= b, 5: lambda a, b: a > b, 6: lambda a, b: a >= b}


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def tee_sink(path, echo=None):
    """An AsyncLog sink for --log: each line is appended to the file (kept across restarts), then echoed to the
    console. The file is written first, so a console that fails still leaves the line on disk."""
    f = open(path, "a", encoding="utf-8", buffering=1)
    echo = echo or (lambda line: print(line, flush=True))

    def sink(line):
        try:
            f.write(line + "\n")
        except OSError:
            pass
        echo(line)
    sink.close = f.close
    return sink


class AsyncLog:
    """The server's logger: lines are stamped when logged and written by a thread of their own, so a console that
    blocks (Windows QuickEdit selection pauses every write to it) or a stalled disk never stops the server. Over
    10000 lines waiting, new lines are dropped and counted."""

    def __init__(self, sink=None, max_lines=10000):
        self.sink = sink or (lambda line: print(line, flush=True))
        self.q = queue.Queue(max_lines)
        self.dropped = 0
        threading.Thread(target=self._run, daemon=True).start()

    def __call__(self, msg):
        try:
            self.q.put_nowait("%s %s" % (time.strftime("%H:%M:%S"), msg))
        except queue.Full:
            self.dropped += 1

    def _run(self):
        while True:
            line = self.q.get()
            if self.dropped:
                n, self.dropped = self.dropped, 0
                self.sink("%s log: %d lines dropped while the output was blocked" % (time.strftime("%H:%M:%S"), n))
            try:
                self.sink(line)
            except OSError:
                pass
            self.q.task_done()

    def flush(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while self.q.unfinished_tasks and time.monotonic() < deadline:
            time.sleep(0.01)


METRICS_SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions(id INTEGER PRIMARY KEY, label TEXT, staff INT, online_id TEXT, transport TEXT,
    proto INT, start_utc TEXT, end_utc TEXT, end_reason TEXT, bytes_in INT, bytes_out INT, dgrams_dropped INT);
CREATE TABLE IF NOT EXISTS rooms(id INTEGER PRIMARY KEY, kind TEXT, owner_session INT, created_utc TEXT,
    closed_utc TEXT, max_members INT);
CREATE TABLE IF NOT EXISTS events(t_utc TEXT, session INT, kind TEXT, room INT, detail TEXT);
CREATE TABLE IF NOT EXISTS direct_stats(session INT, path TEXT, rtt_p50 INT, rtt_p95 INT, loss_x10 INT,
    direct_s INT, fallbacks INT, relay_p50 INT, relay_p95 INT, relay_s INT);
"""
# Relay-link numbers a client reports with link_stats (NULL until then). Added to older files in place.
LINK_COLUMNS = ("pings", "slow_pings", "rtt_p50", "rtt_p95", "rtt_max", "max_rx_gap_ms")
LINK_MAX = 600000


class MetricsDb:
    """Beta metrics in SQLite (WAL): sessions, rooms, events, by invite code label; no address is stored. put() only
    queues; one writer thread commits in batches, so a slow or locked disk never stops the server (the AsyncLog
    pattern: over 10000 rows waiting, new rows are dropped and counted). Session ids come from the server, so rows
    can be updated without waiting for the database. With no path, a no-op."""

    def __init__(self, path, logger=log, max_rows=10000):
        self.path = path
        self.log = logger
        self.q = queue.Queue(max_rows)
        self.dropped = 0
        self.next_sid = 1
        if not path:
            return
        c = sqlite3.connect(path, timeout=30)
        try:
            c.execute("PRAGMA journal_mode=WAL")
            c.executescript(METRICS_SCHEMA)
            have = {r[1] for r in c.execute("PRAGMA table_info(sessions)")}
            for col in LINK_COLUMNS:
                if col not in have:
                    c.execute("ALTER TABLE sessions ADD COLUMN %s INT" % col)
            have = {r[1] for r in c.execute("PRAGMA table_info(direct_stats)")}
            for col in RELAY_COLUMNS:
                if col not in have:
                    c.execute("ALTER TABLE direct_stats ADD COLUMN %s INT" % col)
            now = iso_utc(time.time())
            # A server that died without stopping left these open: closed by this restart.
            n = c.execute("UPDATE sessions SET end_utc=?, end_reason='server_stop' WHERE end_utc IS NULL",
                          (now,)).rowcount
            c.execute("UPDATE rooms SET closed_utc=? WHERE closed_utc IS NULL", (now,))
            self.next_sid = (c.execute("SELECT MAX(id) FROM sessions").fetchone()[0] or 0) + 1
            c.commit()
        finally:
            c.close()
        if n:
            self.log("metrics: %d sessions left open by the previous run closed as server_stop" % n)
        threading.Thread(target=self._run, daemon=True).start()

    def session_id(self):
        sid, self.next_sid = self.next_sid, self.next_sid + 1
        return sid

    def put(self, sql, params=()):
        if not self.path:
            return
        try:
            self.q.put_nowait((sql, params))
        except queue.Full:
            self.dropped += 1

    def _run(self):
        c = sqlite3.connect(self.path, timeout=5)
        c.execute("PRAGMA synchronous=NORMAL")
        while True:
            batch = [self.q.get()]
            while len(batch) < 500:
                try:
                    batch.append(self.q.get_nowait())
                except queue.Empty:
                    break
            while True:
                try:
                    with c:
                        for sql, params in batch:
                            c.execute(sql, params)
                    break
                except sqlite3.OperationalError as e:  # locked or busy: the same batch again
                    self.log("metrics: database busy (%s), retrying" % e)
                    time.sleep(1)
                except sqlite3.Error as e:
                    self.log("metrics: %d rows lost (%s)" % (len(batch), e))
                    break
            if self.dropped:
                n, self.dropped = self.dropped, 0
                self.log("metrics: %d rows dropped while the database was slow" % n)
            for _ in batch:
                self.q.task_done()

    def flush(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while self.q.unfinished_tasks and time.monotonic() < deadline:
            time.sleep(0.01)


def disable_quick_edit():
    """Windows: a click in the server's console window starts a text selection, and while it lasts every write to
    the console (each log line) blocks the server. Turn QuickEdit off for this console; True when it was on."""
    if sys.platform != "win32":
        return False
    import ctypes
    k32 = ctypes.windll.kernel32
    h = k32.GetStdHandle(-10)  # STD_INPUT_HANDLE
    mode = ctypes.c_uint32()
    if not k32.GetConsoleMode(h, ctypes.byref(mode)) or not mode.value & 0x40:  # no console / already off
        return False
    return bool(k32.SetConsoleMode(h, (mode.value & ~0x40) | 0x80))  # ENABLE_EXTENDED_FLAGS keeps it applied


def normalize_code(text):
    """Invite codes are shown as P3-XXXX-XXXX; case, spaces and dashes do not matter."""
    return re.sub(r"[\s-]", "", str(text)).upper()


def code_hash(text):
    return hashlib.sha256(normalize_code(text).encode()).hexdigest()


def parse_utc(text):
    return calendar.timegm(time.strptime(text, "%Y-%m-%dT%H:%M:%SZ"))


def iso_utc(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


# A login without a known code while the beta is public.
PUBLIC_ENTRY = {"label": "público", "enabled": True, "staff": False}


def load_beta(path):
    """The beta config: window (UTC), minimum protocol, invite codes by SHA-256 (the codes themselves are never
    stored here), "public" (anyone logs in without a code during the window; known codes keep their label and staff). Raises OSError / ValueError / KeyError / TypeError on a bad file."""
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    cfg = {"starts": raw["starts"], "ends": raw["ends"], "t0": parse_utc(raw["starts"]), "t1": parse_utc(raw["ends"]),
           "min_proto": int(raw.get("min_proto", INFRA_PROTO)), "max_per_code": int(raw.get("max_per_code", 2)),
           "public": bool(raw.get("public", False)), "direct": bool(raw.get("direct", True)), "codes": {}}
    for c in raw.get("codes", []):
        sha = str(c["sha256"]).lower()
        if not re.fullmatch(r"[0-9a-f]{64}", sha):
            raise ValueError("bad sha256 for %r" % c.get("label"))
        cfg["codes"][sha] = {"label": str(c["label"]), "enabled": bool(c.get("enabled", True)),
                             "staff": bool(c.get("staff", False))}
    return cfg


IMPAIR_KEYS = {"delay": int, "jitter": int, "stall": float, "stall_ms": int}


def parse_impair(text):
    """--impair "delay=MS,jitter=MS,stall=PCT,stall_ms=MS" (lab only) -> dict; every key optional, at least one."""
    out = {}
    for part in text.split(",") if text else []:
        key, sep, val = part.partition("=")
        kind = IMPAIR_KEYS.get(key.strip())
        if not sep or kind is None or key.strip() in out:
            raise argparse.ArgumentTypeError("--impair: %r (keys: delay, jitter, stall, stall_ms)" % part)
        try:
            v = kind(val.strip())
        except ValueError:
            raise argparse.ArgumentTypeError("--impair: %r is not a number of the right kind" % part)
        if v < 0 or (key.strip() == "stall" and v > 100):
            raise argparse.ArgumentTypeError("--impair: %r out of range" % part)
        out[key.strip()] = v
    if not out:
        raise argparse.ArgumentTypeError("--impair: empty")
    return out


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
        # Signaling between two members: (to, about) waits until `about` has bound its game
        # socket (then ESTABLISHED to `to`) or its deadline passes (then DEAD).
        self.waiting = {}           # (to Member, about Member) -> deadline
        self.established = set()    # (to Member, about Member)

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
        self.console = None         # console id from the login, None for older runtimes
        self.proto = None           # wire protocol version from the login
        self.code_label = None      # beta invite code label ("invitado 07"); the code itself is never kept
        self.code_sha = None
        self.staff = False
        self.sid = None             # metrics session id
        self.session = None
        self.vip = 0
        self.port = 0
        self.rooms = {}             # room id -> Room this connection is a member of
        self.dgram_window = 0.0
        self.dgram_count = 0
        self.dgram_dropped = 0
        self.stream_window = 0.0
        self.stream_bytes = 0
        self.last_due = 0.0         # --impair: due time of the last frame queued to this connection
        self.caps_direct = False    # protocol 3 login caps: speaks the direct path
        self.cands = None           # its last direct_offer candidates; never logged or stored
        self.offer_times = []

    def label(self):
        vip = socket.inet_ntoa(struct.pack(">I", self.vip)) if self.vip else "-"
        return "%s(%s %s %s)" % (self.online_id or "?", self.kind, self.addr_key or self.peer_ip, vip)


class Server:
    def __init__(self, host="127.0.0.1", port=27320, ws_port=27321, ws_path="/infra", max_rooms=64,
                 idle=30.0, stats_every=60.0, delay_ms=0, max_per_addr=4, max_conns=512, signal_timeout=15.0,
                 loss=0, logger=log, beta_path=None, clock=time.time, db_path=None, impair=None, status_path=None,
                 status_every=2.0, no_direct=False, stun=None):
        self.log = logger
        # --status: the public lobby summary (tools/beta_report_server.py serves it at beta.montick.app/salas)
        self.status_path = status_path
        self.status_every = status_every
        self.no_direct = no_direct
        self.stun = stun or DEFAULT_STUN
        self.pairs = {}         # frozenset({Conn, Conn}) -> (pair id, 32-byte key); one per connection pair
        self.status_t = 0.0
        # Lab only (--impair): every frame to a connection is delayed, jittered and sometimes stalled like a TCP
        # link that lost a segment, keeping that connection's order. The public server never runs it.
        self.impair = impair or {}
        self.irng = random.Random(2)
        if self.impair and sys.platform == "win32":  # 1 ms timer: frames leave at their due time, not on a 15.6 ms tick
            import ctypes
            ctypes.windll.winmm.timeBeginPeriod(1)
        self.metrics = MetricsDb(db_path, logger)
        self.clock = clock
        self.beta_path = beta_path
        self.beta = self.beta_mtime = None
        if beta_path:
            self.beta_mtime = os.stat(beta_path).st_mtime_ns
            self.beta = load_beta(beta_path)
        self.beta_checked = time.monotonic()
        self.checks = {}        # addr -> times of POST /beta/check in the last minute
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
        # Without a beta config, max_per_addr caps connections as before; with one, connections get a wide cap (staff
        # load tests) and max_per_addr caps invitee sessions at the login instead.
        self.conn_cap = max(BETA_CONNS_PER_ADDR, max_per_addr) if self.beta else max_per_addr
        self.max_conns = max_conns
        self.signal_timeout = signal_timeout
        self.loss = loss / 100.0
        self.rng = random.Random(1)
        self.sel = selectors.DefaultSelector()
        for ls in self.listeners:
            self.sel.register(ls, selectors.EVENT_READ)
        self.conns = {}         # sock -> Conn
        self.per_addr = {}      # addr key -> live connections
        self.by_vip = {}        # vip -> Conn
        self.rooms = {}         # room id -> Room
        # Ids from the start time, so a room list cached by a game across a restart cannot name a new room.
        self.next_room_id = (int(time.time() * 1000) & 0xFFFFFFFFFF) << 8
        self.delayed = []       # heap of (due, seq, conn, frame bytes, impaired)
        self.seq = 0
        self.running = True
        self.stats_t = time.monotonic()
        self.stats_in = self.stats_out = 0

    # ---- connection plumbing ----
    def count_addr(self, conn, key):
        if self.per_addr.get(key, 0) >= self.conn_cap:
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
        if self.impair:
            return self.impaired_send(conn, data)
        self.raw_send(conn, ws_wrap(data) if conn.kind == "ws" else data)

    def impaired_send(self, conn, data):
        """Due time max(now + delay + U(-jitter, jitter), the connection's last due); with probability stall % the
        connection first jumps stall_ms (head-of-line blocking after a lost segment), holding every later frame."""
        now = time.monotonic()
        imp = self.impair
        if imp.get("stall") and self.irng.random() < imp["stall"] / 100.0:
            conn.last_due = max(conn.last_due, now) + imp.get("stall_ms", 0) / 1000.0
        jitter = imp.get("jitter", 0)
        due = max(now + (imp.get("delay", 0) + self.irng.uniform(-jitter, jitter)) / 1000.0, conn.last_due)
        conn.last_due = due
        self.seq += 1
        heapq.heappush(self.delayed, (due, self.seq, conn, data, True))

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
        for room in list(conn.rooms.values()):
            self.leave_room(conn, room, CAUSE_DISAPPEARED, b"")
        self.prune_pairs()
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
        if conn.sid is not None:
            reason = {"server stopping": "server_stop", "connection closed": "closed", "idle": "idle",
                      "beta closed": "beta_closed"}.get(why, why[:64])
            self.metrics.put("UPDATE sessions SET end_utc=?, end_reason=?, bytes_in=?, bytes_out=?, dgrams_dropped=? "
                             "WHERE id=?", (self.now_utc(), reason, conn.bytes_in, conn.bytes_out, conn.dgram_dropped,
                                            conn.sid))
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
        lines = head.split("\r\n")
        parts = lines[0].split(" ")
        headers = {}
        for line in lines[1:]:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()
        body = b""
        if parts[0] == "POST":
            try:
                n = int(headers.get("content-length", "0"))
            except ValueError:
                n = -1
            if not 0 <= n <= MAX_HTTP_BODY:
                del conn.rx[:]
                return self.http_answer(conn, "413 Payload Too Large", {"error": "body"})
            if len(conn.rx) < end + 4 + n:
                return  # the rest of the body is still coming
            body = bytes(conn.rx[end + 4:end + 4 + n])
            del conn.rx[:end + 4 + n]
        else:
            del conn.rx[:end + 4]
        path = parts[1].split("?", 1)[0] if len(parts) >= 3 else ""
        upgrade = headers.get("upgrade", "").lower() == "websocket"
        key = headers.get("sec-websocket-key", "")
        # The tunnel connects from loopback and names the real client; a direct client cannot pick its address.
        addr = conn.peer_ip
        if conn.peer_ip in ("127.0.0.1", "::1") and headers.get("cf-connecting-ip"):
            addr = headers["cf-connecting-ip"][:64]
        if len(parts) >= 3 and not upgrade and path == "/beta" and parts[0] == "GET":
            return self.http_answer(conn, "200 OK", self.beta_status())
        if len(parts) >= 3 and not upgrade and path == "/beta/check" and parts[0] == "POST":
            return self.http_check(conn, addr, body)
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

    # ---- beta: GET /beta, POST /beta/check on the WebSocket listener (online.montick.app) ----
    def http_answer(self, conn, status, obj):
        body = json.dumps(obj).encode()
        conn.close_after_flush = True
        self.raw_send(conn, b"HTTP/1.1 %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
                            b"Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (status.encode(), len(body), body))

    def now_utc(self):
        return iso_utc(self.clock())

    def event(self, kind, conn=None, room=None, detail=None):
        self.metrics.put("INSERT INTO events(t_utc, session, kind, room, detail) VALUES (?, ?, ?, ?, ?)",
                         (self.now_utc(), conn.sid if conn else None, kind, room.id if room else None, detail))

    def beta_open(self):
        return self.beta is None or self.beta["t0"] <= self.clock() <= self.beta["t1"]

    def beta_status(self):
        b = self.beta or {}
        return {"open": self.beta_open(), "starts": b.get("starts"), "ends": b.get("ends"), "now": iso_utc(self.clock()),
                "min_proto": b.get("min_proto", INFRA_PROTO), "public": bool(b.get("public", False))}

    def beta_code(self, invite):
        if self.beta is None or not isinstance(invite, str) or not normalize_code(invite):
            return None, None
        sha = code_hash(invite)
        return sha, self.beta["codes"].get(sha)

    def http_check(self, conn, addr, body):
        now = time.monotonic()
        if len(self.checks) > 10000:
            self.checks.clear()
        recent = [t for t in self.checks.get(addr, ()) if now - t < 60]
        if len(recent) >= CHECKS_PER_MIN:
            self.checks[addr] = recent
            return self.http_answer(conn, "429 Too Many Requests", {"error": "too many checks"})
        self.checks[addr] = recent + [now]
        try:
            code = json.loads(body.decode("utf-8"))["code"]
            if not isinstance(code, str):
                raise TypeError
        except (UnicodeDecodeError, ValueError, KeyError, TypeError):
            return self.http_answer(conn, "400 Bad Request", {"error": "body"})
        if self.beta is None:
            return self.http_answer(conn, "200 OK", {"result": "ok"})
        _, e = self.beta_code(code)
        if e is None:
            res = {"result": "unknown"}
        elif not e["enabled"]:
            res = {"result": "disabled"}
        elif not e["staff"] and not self.beta_open():
            res = {"result": "closed"}
        else:
            res = {"result": "ok", "label": e["label"]}
        self.http_answer(conn, "200 OK", res)

    def beta_reload(self):
        try:
            mtime = os.stat(self.beta_path).st_mtime_ns
            if mtime == self.beta_mtime:
                return
            self.beta_mtime = mtime
            was_on = self.direct_on()
            self.beta = load_beta(self.beta_path)
            self.log("beta config reloaded: %d codes" % len(self.beta["codes"]))
            if self.direct_on() and not was_on:
                self.pair_all()
            elif was_on and not self.direct_on():
                for key in list(self.pairs):
                    self.unpair(key)
        except (OSError, ValueError, KeyError, TypeError) as e:
            self.log("beta config not read (%s: %s): keeping the previous one" % (type(e).__name__, e))

    def beta_login(self, conn, msg, stale):
        """Raises the refusal for a login the beta config does not allow; fills the session's code fields. The stale
        session this login replaces does not count against the limits."""
        label = "?"
        try:
            proto = msg.get("proto")
            if type(proto) is not int or proto < self.beta["min_proto"]:
                raise Err(OUTDATED)
            sha, e = self.beta_code(msg.get("invite"))
            if e is None and self.beta["public"]:
                sha, e = None, PUBLIC_ENTRY
            if e is None:
                raise Err(INVITE_UNKNOWN)
            label = e["label"]
            if not e["enabled"]:
                raise Err(INVITE_DISABLED)
            if not e["staff"]:
                if not self.beta_open():
                    raise Err(BETA_CLOSED)
                live = [c for c in self.conns.values() if c.session and not c.closed and c is not stale]
                if sha is not None and sum(1 for c in live if c.code_sha == sha) >= self.beta["max_per_code"]:
                    raise Err(INVITE_IN_USE)
                if sum(1 for c in live if not c.staff and c.addr_key == conn.addr_key) >= self.max_per_addr:
                    raise Err(SERVICE_UNAVAILABLE)
        except Err as err:
            reason = REFUSAL.get(err.code, "0x%08X" % err.code)
            self.log("login refused %s: %s (%s)" % (label, reason, conn.label()))
            self.event("refused", detail="%s: %s" % (reason, label))
            raise
        conn.code_label, conn.code_sha, conn.staff = label, sha, e["staff"]

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
        if kind == 3:
            return self.on_stream(conn, body)
        if kind == 4 and conn.caps_direct:
            return self.on_channel(conn, body)
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
            if not (op == "login" and self.beta is not None and e.code in REFUSAL):  # those are "refused"
                self.event("error", conn if conn.sid is not None else None, detail="%s 0x%08X" % (op, e.code))
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
        heapq.heappush(self.delayed, (time.monotonic() + self.delay, self.seq, conn, data, False))

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
        if dst is None or dst is conn or not self.share_room(conn, dst):
            conn.dgram_dropped += 1
            return
        if self.loss and self.rng.random() < self.loss:
            return
        out = frame(2, struct.pack("<IHH", conn.vip, dst_port, src_port) + body[8:])
        if self.delay:
            self.later(dst, out)
        else:
            self.send_frame(dst, out)

    def on_stream(self, conn, body):
        """A segment of a game stream socket: forwarded in order like a datagram, never dropped (the
        game's TCP stream would break); a flood closes the session instead."""
        if conn.session is None or len(body) < 9:
            return self.drop(conn, "bad stream segment")
        now = time.monotonic()
        if now - conn.stream_window >= 1.0:
            conn.stream_window, conn.stream_bytes = now, 0
        conn.stream_bytes += len(body)
        if conn.stream_bytes > STREAM_BYTES_PER_S:
            return self.drop(conn, "stream flood")
        vip, dst_port, src_port = struct.unpack_from("<IHH", body)
        dst = self.by_vip.get(vip)
        if dst is None or dst is conn or not self.share_room(conn, dst):
            return
        out = frame(3, struct.pack("<IHH", conn.vip, dst_port, src_port) + body[8:])
        if self.delay:
            self.later(dst, out)
        else:
            self.send_frame(dst, out)

    def on_channel(self, conn, body):
        """A direct-path packet relayed between two paired players (opaque, sealed by them): forwarded in order like
        a stream segment and counted in the same byte budget; without a pairing, or over MAX_CHANNEL, dropped."""
        if conn.session is None or len(body) < 4 or len(body) - 4 > MAX_CHANNEL:
            conn.dgram_dropped += 1
            return
        now = time.monotonic()
        if now - conn.stream_window >= 1.0:
            conn.stream_window, conn.stream_bytes = now, 0
        conn.stream_bytes += len(body)
        if conn.stream_bytes > STREAM_BYTES_PER_S:
            return self.drop(conn, "stream flood")
        dst = self.by_vip.get(struct.unpack_from("<I", body)[0])
        if dst is None or frozenset((conn, dst)) not in self.pairs:
            conn.dgram_dropped += 1
            return
        out = frame(4, struct.pack("<I", conn.vip) + body[4:])
        if self.delay:
            self.later(dst, out)
        else:
            self.send_frame(dst, out)

    # ---- direct path signaling (no address or key is ever logged or stored) ----
    def direct_on(self):
        return not self.no_direct and (self.beta is None or self.beta["direct"])

    @staticmethod
    def parse_cands(cands):
        if not isinstance(cands, list) or not 1 <= len(cands) <= MAX_CANDS:
            raise Err(BAD_REQUEST)
        for c in cands:
            if (not isinstance(c, dict) or set(c) != {"t", "a"} or c["t"] not in ("srflx", "host")
                    or not isinstance(c["a"], str)):
                raise Err(BAD_REQUEST)
            ip, _, port = c["a"].partition(":")
            try:
                addr = ipaddress.IPv4Address(ip)
            except ValueError:
                raise Err(BAD_REQUEST)
            if (addr.is_multicast or addr.is_unspecified or addr.is_reserved or str(addr) != ip
                    or not port.isdigit() or not 1 <= int(port) <= 65535):
                raise Err(BAD_REQUEST)
        return [{"t": c["t"], "a": c["a"]} for c in cands]

    @staticmethod
    def cands_for(to, about):
        """about's candidates as sent to `to`: host ones only when both sit behind one NAT (same srflx IP)."""
        def srflx_ips(conn):
            return {c["a"].partition(":")[0] for c in conn.cands if c["t"] == "srflx"}
        same_nat = bool(srflx_ips(to) & srflx_ips(about))
        return [c for c in about.cands if c["t"] == "srflx" or same_nat]

    def maybe_pair(self, a, b):
        key = frozenset((a, b))
        if (a is b or key in self.pairs or not self.direct_on() or a.closed or b.closed or not a.caps_direct
                or not b.caps_direct or not a.cands or not b.cands or not self.share_room(a, b)):
            return
        live = {p for p, _ in self.pairs.values()}
        pair_id = 0
        while pair_id == 0 or pair_id in live:
            pair_id = secrets.randbits(32)
        secret = secrets.token_bytes(32)
        self.pairs[key] = (pair_id, secret)
        for to, about in ((a, b), (b, a)):
            self.send_json(to, {"ev": "direct_peer", "vip": about.vip, "pair": pair_id, "key": secret.hex(),
                                "cands": self.cands_for(to, about)})
        self.event("direct_pair", a)
        self.log("direct: pair %s <-> %s" % (a.online_id, b.online_id))

    def unpair(self, key):
        # A send that fails below drops a connection, and that drop's own prune can end a pairing a caller is
        # still iterating over.
        if key not in self.pairs:
            return
        pair_id, _ = self.pairs.pop(key)
        a, b = tuple(key)
        for to, about in ((a, b), (b, a)):
            if not to.closed:
                self.send_json(to, {"ev": "direct_drop", "vip": about.vip, "pair": pair_id})
        self.event("direct_drop", a)
        self.log("direct: drop %s <-> %s" % (a.online_id, b.online_id))

    def prune_pairs(self):
        """Pairings whose two connections no longer share a room (or one is gone) end."""
        for key in [k for k in self.pairs if any(c.closed for c in k) or not self.share_room(*k)]:
            self.unpair(key)

    def pair_all(self):
        for a in list(self.conns.values()):
            for room in list(a.rooms.values()):
                for m in room.members:
                    self.maybe_pair(a, m.conn)

    def op_direct_offer(self, conn, msg):
        if not conn.caps_direct:
            raise Err(BAD_REQUEST)
        cands = self.parse_cands(msg.get("cands"))
        now = time.monotonic()
        conn.offer_times = [t for t in conn.offer_times if now - t < 60]
        if len(conn.offer_times) >= OFFERS_PER_MIN:
            raise Err(SERVICE_UNAVAILABLE)
        conn.offer_times.append(now)
        conn.cands = cands
        for key in [k for k in self.pairs if conn in k]:  # a new offer replaces the pairing: drop, then a fresh key
            self.unpair(key)
        for room in list(conn.rooms.values()):
            for m in room.members:
                self.maybe_pair(conn, m.conn)
        return {}

    def op_direct_stats(self, conn, msg):
        # Per-pair direct-path numbers (client direct::Link::stats_json): the path kind and numbers only.
        pairs = msg.get("pairs")
        if set(msg) - {"id", "op"} != {"pairs"} or not isinstance(pairs, list) or len(pairs) > MAX_DIRECT_PAIRS:
            raise Err(BAD_REQUEST)
        # u7 clients send the DIRECT_COLUMNS form, u8 adds RELAY_COLUMNS.
        forms = ({"path", *DIRECT_COLUMNS}, {"path", *DIRECT_COLUMNS, *RELAY_COLUMNS})
        for p in pairs:
            if (not isinstance(p, dict) or set(p) not in forms or p["path"] not in ("direct", "relay")
                    or not all(type(p[k]) is int and 0 <= p[k] <= LINK_MAX for k in set(p) - {"path"})):
                raise Err(BAD_REQUEST)
        cols = DIRECT_COLUMNS + RELAY_COLUMNS
        for p in pairs:
            self.metrics.put("INSERT INTO direct_stats(session, path, %s) VALUES (?, ?%s)"
                             % (", ".join(cols), ", ?" * len(cols)), (conn.sid, p["path"], *(p.get(k) for k in cols)))
            relay = " relay p50 %d p95 %d %d s" % tuple(p[k] for k in RELAY_COLUMNS) if "relay_s" in p else ""
            self.log("%s direct_stats: %s p50 %d p95 %d loss %d/1000 %d s fallbacks %d%s"
                     % (conn.online_id, p["path"], *(p[k] for k in DIRECT_COLUMNS), relay))
        return {}

    @staticmethod
    def share_room(a, b):
        return not a.rooms.keys().isdisjoint(b.rooms.keys())

    # ---- requests ----
    def op_login(self, conn, msg):
        if conn.session is not None:
            raise Err(FORBIDDEN)
        oid = msg["online_id"]
        if not isinstance(oid, str) or not ONLINE_ID.fullmatch(oid):
            raise Err(BAD_REQUEST)
        console = msg.get("console")
        if console is not None and (not isinstance(console, str) or not CONSOLE_ID.fullmatch(console)):
            raise Err(BAD_REQUEST)
        # A game that froze or crashed is back within seconds, but its old session lives until the idle timeout and
        # keeps its console id in the room (DUPLICATE_CONSOLE): the same player from the same address replaces it.
        stale = next((c for c in self.conns.values() if c.session and not c.closed and c.online_id == oid
                      and console and c.console == console and conn.addr_key and c.addr_key == conn.addr_key), None)
        if self.beta is not None:
            self.beta_login(conn, msg, stale)
        if stale is not None:
            self.drop(stale, "replaced")
        proto = msg.get("proto")
        conn.proto = proto if type(proto) is int else None
        n = 1
        while VIP_BASE + n in self.by_vip:
            n += 1
        if n > 0xFFFE:
            raise Err(SERVICE_UNAVAILABLE)
        conn.online_id, conn.session, conn.vip = oid, secrets.token_hex(8), VIP_BASE + n
        conn.console = console
        caps = msg.get("caps")
        conn.caps_direct = isinstance(caps, dict) and caps.get("direct") == 1
        self.by_vip[conn.vip] = conn
        conn.sid = self.metrics.session_id()
        self.metrics.put("INSERT INTO sessions(id, label, staff, online_id, transport, proto, start_utc) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?)",
                         (conn.sid, conn.code_label, int(conn.staff), oid, conn.kind, conn.proto, self.now_utc()))
        self.log("login %s" % conn.label())
        answer = {"session": conn.session, "vip": conn.vip}
        if isinstance(caps, dict):
            answer["direct"] = {"on": self.direct_on(), "stun": self.stun}
        return answer

    def op_ping(self, conn, msg):
        answer = {"id": msg.get("id"), "ok": True, "t": msg.get("t")}
        if self.delay:
            self.later(conn, json_frame(answer))
            return None
        return {"t": msg.get("t")}

    def op_link_stats(self, conn, msg):
        # The session's relay-link numbers (client infra::LinkStats); a later report overwrites. Numbers only.
        if set(msg) - {"id", "op"} != set(LINK_COLUMNS):
            raise Err(BAD_REQUEST)
        vals = [msg[k] for k in LINK_COLUMNS]
        if not all(type(v) is int and 0 <= v <= LINK_MAX for v in vals):
            raise Err(BAD_REQUEST)
        self.metrics.put("UPDATE sessions SET " + ", ".join("%s=?" % k for k in LINK_COLUMNS) + " WHERE id=?",
                         tuple(vals) + (conn.sid,))
        n = dict(zip(LINK_COLUMNS, vals))
        self.log("%s link: p50 %d p95 %d max %d slow %d/%d gap %d" % (
            conn.label(), n["rtt_p50"], n["rtt_p95"], n["rtt_max"], n["slow_pings"], n["pings"], n["max_rx_gap_ms"]))
        return {}

    def op_bound(self, conn, msg):
        port = int(msg["port"])
        if not 1 <= port <= 0xFFFF:
            raise Err(BAD_REQUEST)
        conn.port = port
        self.log("%s bound game port %d" % (conn.label(), port))
        for room in conn.rooms.values():
            self.resolve(room)
        return {}

    def match(self, room, filters, flag_filter=0, flag_attr=0):
        if room.flag_attr & FLAG_HIDDEN or room.flag_attr & flag_filter != flag_attr & flag_filter:
            return False
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
        flag_filter, flag_attr = u32(msg.get("flag_filter", 0)), u32(msg.get("flag_attr", 0))
        found = [r for r in self.rooms.values() if self.match(r, filters, flag_filter, flag_attr)]
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
        if len(conn.rooms) >= MAX_ROOMS_PER_CONN or len(self.rooms) >= self.max_rooms:
            raise Err(SERVICE_UNAVAILABLE)
        if self.beta is not None and not conn.staff and sum(
                1 for r in self.rooms.values()
                if not r.owner.conn.staff and r.owner.conn.addr_key == conn.addr_key) >= MAX_ROOMS_PER_ADDR:
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
        room.next_member_id += 1
        room.owner = owner
        room.members.append(owner)
        self.rooms[room.id] = room
        conn.rooms[room.id] = room
        k = room.ints.get(0x4D, 0)
        kind = "vs" if k == 3 else "hideout" if k & 0x30000 else "other"
        self.metrics.put("INSERT INTO rooms(id, kind, owner_session, created_utc, max_members) VALUES (?, ?, ?, ?, 1)",
                         (room.id, kind, conn.sid, self.now_utc()))
        self.log("%s create room %d (%d slots, %d int, %d bin attrs)"
                 % (conn.label(), room.id, max_slot, len(ints), len(room.bins)))
        return {"room": room.internal(owner)}

    def op_join(self, conn, msg):
        room = self.rooms.get(int(msg["room_id"]))
        if room is None:
            raise Err(NO_SUCH_ROOM)
        if room.id in conn.rooms:
            raise Err(ALREADY_JOINED)
        if len(conn.rooms) >= MAX_ROOMS_PER_CONN:
            raise Err(SERVICE_UNAVAILABLE)
        if len(room.members) >= room.max_slot or room.flag_attr & (FLAG_CLOSED | FLAG_FULL):
            raise Err(ROOM_FULL)
        # The game tells players apart by the OpenPSID built from the console
        # MAC: a second member with the same one would also be "player 1".
        twin = next((m for m in room.members if conn.console and m.conn.console == conn.console), None)
        if twin is not None:
            self.log("%s join room %d refused: same console id as %s" % (conn.label(), room.id, twin.conn.label()))
            raise Err(DUPLICATE_CONSOLE)
        opt = hex_bytes(msg.get("opt", ""), 16)
        member = Member(conn, room.next_member_id, bin_attrs(msg.get("member_bins", [])))
        room.next_member_id += 1
        olds = list(room.members)
        room.members.append(member)
        conn.rooms[room.id] = room
        self.event("join", conn, room)
        self.metrics.put("UPDATE rooms SET max_members=MAX(max_members, ?) WHERE id=?", (len(room.members), room.id))
        self.log("%s join room %d as 0x%X (%d/%d)" % (conn.label(), room.id, member.member_id,
                                                         len(room.members), room.max_slot))
        answer = {"id": msg.get("id"), "ok": True, "room": room.internal(member)}
        self.send_json(conn, answer)
        deadline = time.monotonic() + self.signal_timeout
        for m in olds:
            self.send_json(m.conn, {"ev": "member_joined", "room_id": room.id, "member": member.json(),
                                    "opt": opt.hex()})
            room.waiting[(member, m)] = deadline
            room.waiting[(m, member)] = deadline
            self.maybe_pair(conn, m.conn)
        self.resolve(room)
        return None

    def signal(self, to, room, about, event):
        self.send_json(to, {"ev": "signaling", "room_id": room.id, "member_id": about.member_id,
                            "event": event, "vip": about.conn.vip, "port": about.conn.port})

    def resolve(self, room):
        """ESTABLISHED for every waiting pair whose `about` member has bound its game socket."""
        for to, about in list(room.waiting):
            if about.conn.port and room.waiting.pop((to, about), None) is not None:
                room.established.add((to, about))
                self.signal(to.conn, room, about, SIG_ESTABLISHED)

    def leave_room(self, conn, room, cause, opt):
        del conn.rooms[room.id]
        member = room.member_of(conn)
        room.members.remove(member)
        told = {to for to, about in room.established if about is member}
        room.waiting = {k: v for k, v in room.waiting.items() if member not in k}
        room.established = {k for k in room.established if member not in k}
        self.event("kick" if cause == CAUSE_KICKOUT else "leave", conn, room,
                   "disappeared" if cause == CAUSE_DISAPPEARED else None)
        if member is room.owner:
            self.metrics.put("UPDATE rooms SET closed_utc=? WHERE id=?", (self.now_utc(), room.id))
            del self.rooms[room.id]
            for m in room.members:
                m.conn.rooms.pop(room.id, None)
                self.send_json(m.conn, {"ev": "room_destroyed", "room_id": room.id, "cause": cause,
                                        "opt": opt.hex()})
            self.log("%s left room %d: room destroyed (%d members told)"
                     % (conn.label(), room.id, len(room.members)))
            return self.prune_pairs()
        for m in room.members:
            self.send_json(m.conn, {"ev": "member_left", "room_id": room.id, "member": member.json(),
                                    "cause": cause, "opt": opt.hex()})
            # SIGNALING_DEAD about a member that left goes to the owner only: a visitor's game answers it with a
            # kick and "Se ha producido un error de red / Se ha disuelto el escondite" (beta 1, 23 Oct).
            if m in told and m is room.owner:
                self.signal(m.conn, room, member, SIG_DEAD)
        self.log("%s left room %d (cause %d, %d left)" % (conn.label(), room.id, cause, len(room.members)))
        self.prune_pairs()

    def op_leave(self, conn, msg):
        room = conn.rooms.get(int(msg["room_id"]))
        if room is None:
            raise Err(NO_SUCH_ROOM)
        self.leave_room(conn, room, CAUSE_LEAVE, hex_bytes(msg.get("opt", ""), 16))
        return {}

    def op_kick(self, conn, msg):
        room = conn.rooms.get(int(msg["room_id"]))
        if room is None:
            raise Err(NO_SUCH_ROOM)
        if room.owner.conn is not conn:
            raise Err(FORBIDDEN)
        target = next((m for m in room.members if m.member_id == int(msg["member_id"])), None)
        if target is None or target is room.owner:
            raise Err(NO_SUCH_USER)
        opt = hex_bytes(msg.get("opt", ""), 16)
        self.send_json(target.conn, {"ev": "kicked", "room_id": room.id, "cause": CAUSE_KICKOUT, "opt": opt.hex()})
        self.leave_room(target.conn, room, CAUSE_KICKOUT, opt)
        return {}

    def op_set_internal(self, conn, msg):
        room = conn.rooms.get(int(msg["room_id"]))
        if room is None:
            raise Err(NO_SUCH_ROOM)
        room.bins_internal.update(bin_attrs(msg.get("bins", [])))
        if "flag_filter" in msg:
            # PS3 semantics: only the bits in the filter change.
            flag_filter = u32(msg["flag_filter"])
            room.flag_attr = (room.flag_attr & ~flag_filter) | (u32(msg.get("flag_attr", 0)) & flag_filter)
            self.log("%s room %d flags 0x%08X" % (conn.label(), room.id, room.flag_attr))
        elif "flag_attr" in msg:
            room.flag_attr = u32(msg["flag_attr"])
        return {}

    # ---- loop ----
    def tick(self, now):
        if self.beta_path and now - self.beta_checked >= 1.0:
            self.beta_checked = now
            self.beta_reload()
        if self.beta is not None and not self.beta_open():
            for conn in list(self.conns.values()):
                if conn.session and not conn.staff:
                    self.drop(conn, "beta closed")
        for room in list(self.rooms.values()):
            for (to, about), deadline in list(room.waiting.items()):
                if deadline <= now and room.waiting.pop((to, about), None) is not None:
                    self.log("%s: no game socket from %s, signaling DEAD" % (to.conn.label(), about.conn.label()))
                    self.event("signal_dead", to.conn, room, "no game socket from session %s" % about.conn.sid)
                    self.signal(to.conn, room, about, SIG_DEAD)
        while self.delayed and self.delayed[0][0] <= now:
            _, _, conn, data, impaired = heapq.heappop(self.delayed)
            if impaired:
                self.raw_send(conn, ws_wrap(data) if conn.kind == "ws" else data)
            else:
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
        if self.status_path and now - self.status_t >= self.status_every:
            self.status_t = now
            self.write_status()

    def lobby_status(self):
        """Players and rooms for the public lobby page: online ids and game settings only, never addresses.
        A hideout carries bin attrs (hero, online id); a VS room has none and its ints are the VS settings
        (0x4D mission, 0x4F max level, 0x50 rule type, 0x51 rule value, as the game's setupSearchIntAttr logs)."""
        rooms = []
        # A party leaving its hideout hides it, then closes it at departure, for a co-op mission and for VS alike;
        # only VS also puts its players in a VS room.
        in_vs = {m.conn for room in self.rooms.values() if not room.bins for m in room.members}
        for room in sorted(self.rooms.values(), key=lambda r: r.id):
            full = len(room.members) >= room.max_slot
            r = {"kind": "coop" if room.bins else "vs", "owner": room.owner.conn.online_id,
                 "members": len(room.members), "max_slot": room.max_slot}
            if room.bins:
                r["state"] = ("in_vs" if any(m.conn in in_vs for m in room.members)
                              else "in_mission" if room.flag_attr & FLAG_CLOSED
                              else "departing" if room.flag_attr & FLAG_HIDDEN
                              else "full" if full else "open")
            else:
                r["state"] = "playing" if full else "searching"
                r.update(mission=room.ints.get(0x4D, 0), level=room.ints.get(0x4F, 0),
                         rule_type=room.ints.get(0x50, 0), rule_value=room.ints.get(0x51, 0))
            rooms.append(r)
        return {"t": round(self.clock()), "players": sum(1 for c in self.conns.values() if c.session),
                "rooms": rooms}

    def write_status(self):
        tmp = self.status_path + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.lobby_status(), f, ensure_ascii=False, separators=(",", ":"))
            os.replace(tmp, self.status_path)
        except OSError:
            pass  # a reader holding the file open on Windows: the next tick writes it again

    def serve(self):
        while self.running:
            wait = min(0.02, max(0.0, self.delayed[0][0] - time.monotonic())) if self.delayed else 0.2
            for key, mask in self.sel.select(timeout=wait):
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
        self.metrics.flush()
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
        self.events, self.dgrams, self.streams, self.answers = [], [], [], {}
        self.channels = []  # kind 4: (src vip, opaque bytes)
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

    def dgram(self, vip, dst_port, src_port, payload, kind=2):
        self.send_raw_frame(frame(kind, struct.pack("<IHH", vip, dst_port, src_port) + payload))

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
        if kind == 4:
            self.channels.append((struct.unpack_from("<I", body)[0], body[4:]))
        elif kind in (2, 3):
            vip, dst, src = struct.unpack_from("<IHH", body)
            (self.dgrams if kind == 2 else self.streams).append((vip, dst, src, body[8:]))
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
    # Stream socket segments (kind 3): forwarded in order, never rate limited like datagrams.
    for i in range(600):
        a.dgram(0x0A500002, 7000, 50000, bytes([3]) + struct.pack("<H", i), kind=3)
    deadline = time.monotonic() + 3
    while len(b.streams) < 600:
        assert time.monotonic() < deadline, "stream segments: %d of 600" % len(b.streams)
        b.pump(0.1)
    assert all(seg == (0x0A500001, 7000, 50000, bytes([3]) + struct.pack("<H", i)) for i, seg in enumerate(b.streams))
    assert not b.dgrams, "segments stay kind 3"

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
    # Visitors get member_left only: a SIGNALING_DEAD about a member that left made a visitor's game kick it, show
    # "Se ha producido un error de red / Se ha disuelto el escondite" and leave (beta 1, 23 Oct; host unaffected).
    for cl in (c, dd):
        cl.pump(0.3)
        dead = [x for x in cl.events if x["ev"] == "signaling" and x["event"] == SIG_DEAD]
        assert not dead, ("visitor got SIGNALING_DEAD for a member that left", dead)
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
    g.ok("join", room_id=r["id"])
    again = g.req("join", room_id=r["id"])
    assert not again["ok"] and again["error"] == ALREADY_JOINED, "one membership per room"
    h.event(lambda ev: ev["ev"] == "member_joined")
    # A host in a full room (3 peers) sent over 400 datagrams/s at the beta: 1200 in one second all go through.
    burst = b"".join(frame(2, struct.pack("<IHH", 0x0A500001, 1, 1) + b"d") for _ in range(DGRAM_PER_S + 200))
    g.send_raw_frame(burst)
    deadline = time.monotonic() + 0.5  # well inside --idle 1: H sends nothing meanwhile
    while time.monotonic() < deadline:
        h.pump(0.05)
    assert 1200 <= len(h.dgrams) <= DGRAM_PER_S, "rate limit forwarded %d" % len(h.dgrams)
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

    # ---- --loss drops that share of game datagrams (signal-meter tests); streams are never dropped ----
    srv = Server("127.0.0.1", 0, None, loss=50, logger=quiet)
    th = run(srv)
    p, q = TestClient(srv.port), TestClient(srv.port)
    p.ok("login", online_id="LOSSA")
    q.ok("login", online_id="LOSSB")
    rl = p.ok("create", max_slot=4)["room"]
    q.ok("join", room_id=rl["id"])
    for i in range(200):
        p.dgram(0x0A500002, 1, 1, b"d")
    for i in range(50):
        p.dgram(0x0A500002, 7000, 50000, bytes([3]) + struct.pack("<H", i), kind=3)
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        q.pump(0.05)
    assert 50 <= len(q.dgrams) <= 150, "about half the datagrams: %d" % len(q.dgrams)
    assert len(q.streams) == 50, "every stream segment"
    p.close()
    q.close()
    srv.running = False
    th.join(timeout=2)

    # ---- members whose game socket never binds get SIGNALING_DEAD, not an endless wait ----
    srv = Server("127.0.0.1", 0, None, signal_timeout=0.5, logger=quiet)
    th = run(srv)
    p, q = TestClient(srv.port), TestClient(srv.port)
    p.ok("login", online_id="HOST2")
    q.ok("login", online_id="JOIN2")
    r3 = p.ok("create", max_slot=4)["room"]
    assert len({room["id"], r["id"], r3["id"]}) == 3, "room ids differ across server restarts"
    q.ok("join", room_id=r3["id"])
    dead_q = q.event(lambda x: x["ev"] == "signaling", what="joiner: signaling about the host")
    dead_p = p.event(lambda x: x["ev"] == "signaling", what="host: signaling about the joiner")
    assert dead_q["event"] == SIG_DEAD and dead_q["member_id"] == 0x10, dead_q
    assert dead_p["event"] == SIG_DEAD and dead_p["member_id"] == 0x11, dead_p
    p.ok("bound", port=5000)
    assert not any(x["ev"] == "signaling" for x in q.events) and not q.pump(0.3), "no late ESTABLISHED"
    big = bytes([3]) + b"z" * 60000  # 40 x 60 kB in one second is over the 2 MiB/s stream cap
    try:
        for _ in range(40):
            q.dgram(0x0A500001, 7000, 50000, big, kind=3)
    except (ConnectionError, OSError):
        pass  # the server may close it while the flood is still being written
    assert q.is_closed(), "stream flood closes the session"
    p.close()
    q.close()
    srv.running = False
    th.join(timeout=2)

    # ---- one console id per room: a copied MAC file must not make two player 1s ----
    srv = Server("127.0.0.1", 0, None, max_per_addr=16, logger=quiet)
    th = run(srv)
    ca, cb, cc, cd, ce = (TestClient(srv.port) for _ in range(5))
    ca.ok("login", online_id="CONA", console="02000000000a")
    cb.ok("login", online_id="CONB", console="02000000000a")
    cc.ok("login", online_id="CONC", console="02000000000b")
    cd.ok("login", online_id="COND")  # an older runtime sends no console id
    r4 = ca.ok("create", max_slot=4)["room"]
    dup = cb.req("join", room_id=r4["id"])
    assert not dup["ok"] and dup["error"] == DUPLICATE_CONSOLE, dup
    assert not ca.is_closed(0.3), "another nickname with the same console does not replace the host"
    assert any("refused: same console id as CONA" in l for l in lines), "refusal logged"
    cc.ok("join", room_id=r4["id"])
    cd.ok("join", room_id=r4["id"])
    bad = ce.req("login", online_id="CONE", console="xyz")
    assert not bad["ok"] and bad["error"] == BAD_REQUEST, bad
    for cl in (ca, cb, cc, cd, ce):
        cl.close()
    srv.running = False
    th.join(timeout=2)

    # ---- a game that froze or crashed logs in again: the same player from the same address replaces the stale
    # session ----
    srv = Server("127.0.0.1", 0, 0, max_per_addr=16, logger=quiet)
    th = run(srv)

    def at(ip):
        return TestClient(srv.ws_port, ws=True, headers="CF-Connecting-IP: %s\r\n" % ip)
    host, old, new = at("203.0.113.20"), at("203.0.113.21"), at("203.0.113.21")
    host.ok("login", online_id="RHOST", console="02000000001a")
    old.ok("login", online_id="RBACK", console="02000000001b")
    room_id = host.ok("create", max_slot=4)["room"]["id"]
    old.ok("join", room_id=room_id)
    host.event(lambda x: x["ev"] == "member_joined")
    new.ok("login", online_id="RBACK", console="02000000001b")
    assert old.is_closed(), "stale session closed by the new login"
    host.event(lambda x: x["ev"] == "member_left" and x["room_id"] == room_id, what="stale member left")
    new.ok("join", room_id=room_id)
    assert any("close RBACK" in l and ": replaced" in l for l in lines), "replacement logged"
    # The stale session was the host: its room goes, and the new login still gets in and hosts again.
    h1, v, h2 = at("203.0.113.30"), at("203.0.113.31"), at("203.0.113.30")
    h1.ok("login", online_id="RHOST2", console="02000000002a")
    v.ok("login", online_id="RVIS2", console="02000000002b")
    v.ok("join", room_id=h1.ok("create", max_slot=4)["room"]["id"])
    h2.ok("login", online_id="RHOST2", console="02000000002a")
    assert h1.is_closed(), "stale host closed by the new login"
    v.event(lambda x: x["ev"] == "room_destroyed", what="room_destroyed")
    h2.ok("create", max_slot=4)
    # The same player from another address (a phone hotspot) is not replaced and is still refused.
    c1, c2 = at("203.0.113.40"), at("198.51.100.40")
    c1.ok("login", online_id="RFAR", console="02000000003a")
    c1.ok("join", room_id=room_id)
    c2.ok("login", online_id="RFAR", console="02000000003a")
    assert not c1.is_closed(0.3), "another address does not replace the session"
    far = c2.req("join", room_id=room_id)
    assert not far["ok"] and far["error"] == DUPLICATE_CONSOLE, far
    # An older runtime sends no console id: never replaced.
    d1, d2 = at("203.0.113.50"), at("203.0.113.50")
    d1.ok("login", online_id="RNOCON")
    d2.ok("login", online_id="RNOCON")
    assert not d1.is_closed(0.3), "no console id, no replacement"
    for cl in (host, old, new, h1, v, h2, c1, c2, d1, d2):
        cl.close()
    srv.running = False
    th.join(timeout=2)

    # A log that cannot be written (a console in QuickEdit selection, a stalled disk) never stops the server.
    gate = threading.Event()
    got = []

    def stuck(line):
        gate.wait()
        got.append(line)
    alog = AsyncLog(sink=stuck)
    t0 = time.monotonic()
    for i in range(2000):
        alog("line %d" % i)
    assert time.monotonic() - t0 < 0.5, "logging waited for a blocked sink"
    srv5 = Server("127.0.0.1", 0, None, logger=alog)
    th5 = run(srv5)
    q = TestClient(srv5.port)
    assert q.ok("login", online_id="P3Q")["vip"] == 0x0A500001, "served while the log is blocked"
    q.close()
    gate.set()
    alog.flush(timeout=3)
    assert len(got) >= 2001 and got[0].endswith(" line 0") and any("login P3Q" in l for l in got), got[:2]
    srv5.running = False
    th5.join(timeout=2)

    # --log FILE: every line also goes to the file, appended across restarts, even when the console fails.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "server.log")
        echoed = []
        first = tee_sink(path, echo=echoed.append)
        alog = AsyncLog(sink=first)
        alog("first run")
        alog.flush(timeout=3)
        first.close()

        def broken(line):
            raise OSError("console gone")
        second = tee_sink(path, echo=broken)
        alog = AsyncLog(sink=second)
        alog("second run")
        alog.flush(timeout=3)
        second.close()
        with open(path, encoding="utf-8") as f:
            kept = f.read().splitlines()
        assert len(kept) == 2 and kept[0].endswith(" first run") and kept[1].endswith(" second run"), kept
        assert echoed == kept[:1], echoed

    # ---- VS: a player is in its hideout room and a VS room at once (up to 4 rooms), room flags ----
    srv = Server("127.0.0.1", 0, None, max_per_addr=16, logger=quiet)
    th = run(srv)
    p1, p2, p3, p4 = (TestClient(srv.port) for _ in range(4))
    for i, cl in enumerate((p1, p2, p3, p4)):
        cl.ok("login", online_id="VS%d" % (i + 1))  # vips 10.80.0.1 .. 10.80.0.4

    def next_dgram(cl, timeout=3):
        deadline = time.monotonic() + timeout
        while not cl.dgrams and time.monotonic() < deadline:
            cl.pump(0.1)
        return cl.dgrams.pop(0) if cl.dgrams else None

    hide = p1.ok("create", max_slot=4, flag_attr=0x04000000, ints=GAME_INTS)["room"]
    p2.ok("join", room_id=hide["id"])
    p1.event(lambda x: x["ev"] == "member_joined" and x["room_id"] == hide["id"])
    vs = p1.ok("create", max_slot=4, ints=GAME_INTS)["room"]  # two_rooms
    assert vs["id"] != hide["id"] and vs["me"] == 0x10, vs
    p3.ok("join", room_id=vs["id"])
    p1.event(lambda x: x["ev"] == "member_joined" and x["room_id"] == vs["id"])
    extra = [p1.ok("create", max_slot=4)["room"]["id"] for _ in range(2)]
    fifth = p1.req("create", max_slot=4)
    assert not fifth["ok"] and fifth["error"] == SERVICE_UNAVAILABLE, fifth
    again = p1.req("join", room_id=hide["id"])
    assert not again["ok"] and again["error"] == ALREADY_JOINED, again
    for rid in extra:
        p1.ok("leave", room_id=rid)
    nope = p2.req("leave", room_id=vs["id"])
    assert not nope["ok"] and nope["error"] == NO_SUCH_ROOM, nope
    # dgram_needs_a_shared_room: P2 (hideout only) cannot reach P3 (VS only); P1 shares the VS room with P3.
    p2.dgram(0x0A500003, 1, 1, b"no shared room")
    assert next_dgram(p3, 0.4) is None, "datagram between players who share no room"
    p1.dgram(0x0A500003, 2, 1, b"via vs")
    assert next_dgram(p3) == (0x0A500001, 2, 1, b"via vs")
    p3.dgram(0x0A500001, 1, 2, b"back")
    assert next_dgram(p1) == (0x0A500003, 1, 2, b"back")
    # flags: SetRoomDataInternal's filter/attr pair as the game sends it on VS, then back.
    hroom = srv.rooms[hide["id"]]
    p1.ok("set_internal", room_id=hide["id"], flag_filter=0x54000000, flag_attr=0x14000000)
    assert hroom.flag_attr == 0x14000000, hex(hroom.flag_attr)
    # hidden_rooms_not_found
    ids = [x["id"] for x in p4.ok("search")["rooms"]]
    assert hide["id"] not in ids and vs["id"] in ids, ids
    p1.ok("set_internal", room_id=hide["id"], flag_filter=0x54000000, flag_attr=0x04000000)
    assert hroom.flag_attr == 0x04000000, hex(hroom.flag_attr)
    assert hide["id"] in [x["id"] for x in p4.ok("search")["rooms"]], "found again after HIDDEN is cleared"
    # search_flag_filter + closed_refuses_join
    p1.ok("set_internal", room_id=vs["id"], flag_filter=0x40000000, flag_attr=0x40000000)
    ids = [x["id"] for x in p4.ok("search", flag_filter=0x40000000, flag_attr=0)["rooms"]]
    assert hide["id"] in ids and vs["id"] not in ids, ids
    closed = p4.req("join", room_id=vs["id"])
    assert not closed["ok"] and closed["error"] == ROOM_FULL, closed
    p1.ok("set_internal", room_id=vs["id"], flag_filter=0x40000000, flag_attr=0)
    # leave_one_room: P3 leaves the VS room and stays in P4's room W.
    w = p4.ok("create", max_slot=4)["room"]
    p3.ok("join", room_id=w["id"])
    p3.ok("leave", room_id=vs["id"])
    left = p1.event(lambda x: x["ev"] == "member_left", what="member_left of the VS room")
    assert left["room_id"] == vs["id"], left
    p4.dgram(0x0A500003, 3, 4, b"via w")
    assert next_dgram(p3) == (0x0A500004, 3, 4, b"via w")
    p3.ok("join", room_id=vs["id"])
    # drop_leaves_every_room: P1 owns H and V; its connection closes.
    p1.close()
    gone = p2.event(lambda x: x["ev"] == "room_destroyed", what="P2: hideout destroyed")
    assert gone["room_id"] == hide["id"] and gone["cause"] == CAUSE_DISAPPEARED, gone
    gone = p3.event(lambda x: x["ev"] == "room_destroyed", what="P3: VS room destroyed")
    assert gone["room_id"] == vs["id"], gone
    deadline = time.monotonic() + 3
    while hide["id"] in srv.rooms or vs["id"] in srv.rooms:
        assert time.monotonic() < deadline, "rooms left behind: %r" % list(srv.rooms)
        time.sleep(0.05)
    assert list(srv.rooms) == [w["id"]], list(srv.rooms)
    p4.dgram(0x0A500003, 3, 4, b"still w")
    assert next_dgram(p3) == (0x0A500004, 3, 4, b"still w")
    for cl in (p2, p3, p4):
        cl.close()
    srv.running = False
    th.join(timeout=2)

    beta_selftest(run, quiet, lines)
    link_stats_selftest(run, quiet, lines)
    direct_selftest(run, quiet, lines)
    impair_selftest(run, quiet)
    lobby_status_selftest(run, quiet)
    print("selftest OK")


def lobby_status_selftest(run, quiet):
    """--status: the public lobby summary (players, rooms, VS settings) is written to a file, without addresses."""
    import os
    import tempfile

    path = os.path.join(tempfile.mkdtemp(prefix="p3status_"), "salas.json")
    srv = Server("127.0.0.1", 0, None, logger=quiet, status_path=path, status_every=0.05)
    th = run(srv)

    def status(pred, what):
        deadline = time.monotonic() + 3
        while True:
            try:
                with open(path, encoding="utf-8") as f:
                    s = json.load(f)
                if pred(s):
                    return s
            except (OSError, ValueError):
                s = None
            assert time.monotonic() < deadline, "no status with %s (have %r)" % (what, s)
            time.sleep(0.05)

    a, b = TestClient(srv.port), TestClient(srv.port)
    a.ok("login", online_id="P3A")
    b.ok("login", online_id="P3B")
    hero = "Edson".encode("utf-16-le").ljust(94, b"\0").hex()
    hide = a.ok("create", max_slot=4, flag_attr=0x04000000, ints=GAME_INTS, bins=[[0x55, hero]],
                member_bins=[])["room"]
    vs_ints = [[0x4C, 0], [0x4D, 3], [0x4E, 0x30001], [0x4F, 32], [0x50, 0], [0x51, 300]]
    vs = b.ok("create", max_slot=2, flag_attr=0, ints=vs_ints, member_bins=[])["room"]
    # status_rooms: a hideout (bin attrs) is co-op, a room without bin attrs is VS with its settings
    s = status(lambda s: len(s["rooms"]) == 2, "two rooms")
    assert s["players"] == 2 and abs(s["t"] - time.time()) < 5, s
    assert s["rooms"][0] == {"kind": "coop", "owner": "P3A", "members": 1, "max_slot": 4, "state": "open"}, s
    assert s["rooms"][1] == {"kind": "vs", "owner": "P3B", "members": 1, "max_slot": 2, "state": "searching",
                             "mission": 3, "level": 32, "rule_type": 0, "rule_value": 300}, s
    # status_coop_mission: a co-op party leaving hides its hideout, then closes it at departure (no VS room)
    a.ok("set_internal", room_id=hide["id"], flag_filter=0x54000000, flag_attr=0x14000000)
    status(lambda s: s["rooms"][0]["state"] == "departing", "co-op party choosing a mission")
    a.ok("set_internal", room_id=hide["id"], flag_filter=0x54000000, flag_attr=0x54000000)
    status(lambda s: s["rooms"][0]["state"] == "in_mission", "co-op mission")
    # status_vs_playing: a hideout whose member is in a VS room is in VS; a full VS room is a match being played
    a.ok("set_internal", room_id=hide["id"], flag_filter=0x54000000, flag_attr=0x14000000)
    a.ok("join", room_id=vs["id"], member_bins=[])
    s = status(lambda s: s["rooms"][1]["state"] == "playing", "VS playing")
    assert s["rooms"][0]["state"] == "in_vs" and s["rooms"][1]["members"] == 2, s
    a.ok("set_internal", room_id=hide["id"], flag_filter=0x54000000, flag_attr=0x54000000)
    s = status(lambda s: s["rooms"][0]["state"] == "in_vs", "VS match under way")
    # status_no_addresses: online ids only, never addresses, virtual IPs, console ids or codes
    text = open(path, encoding="utf-8").read()
    for bad in ("127.0.0.1", "10.80.", "addr", "console", "label"):
        assert bad not in text, (bad, text)
    # status_players_leave: closing a connection takes its rooms and its player out of the count
    a.close()
    b.close()
    s = status(lambda s: s["players"] == 0 and not s["rooms"], "nobody left")
    srv.running = False
    th.join(timeout=2)


def impair_selftest(run, quiet):
    """--impair: delay, jitter and TCP-stall emulation on every frame to a connection, in order."""

    def pair(impair):
        srv = Server("127.0.0.1", 0, None, logger=quiet, impair=impair)
        th = run(srv)
        a, b = TestClient(srv.port), TestClient(srv.port)
        a.ok("login", online_id="IMP_A")
        vb = b.ok("login", online_id="IMP_B")["vip"]
        room = a.ok("create", max_slot=4, ints=GAME_INTS)["room"]
        b.ok("join", room_id=room["id"])
        return srv, th, a, b, vb

    def stop(srv, th, *clients):
        for cl in clients:
            cl.close()
        srv.running = False
        th.join(timeout=5)

    def first_dgram(cl, timeout=5):
        deadline = time.monotonic() + timeout
        while not cl.dgrams:
            assert time.monotonic() < deadline, "no datagram"
            cl.pump(0.05)
        return cl.dgrams.pop(0)

    # impair_delay: a datagram and a ping answer wait the delay
    srv, th, a, b, vb = pair({"delay": 50})
    t0 = time.monotonic()
    a.dgram(vb, 3, 4, b"late")
    assert first_dgram(b)[3] == b"late"
    assert time.monotonic() - t0 >= 0.045, time.monotonic() - t0
    t0 = time.monotonic()
    a.ok("ping", t=1)
    assert time.monotonic() - t0 >= 0.045, "ping answer delayed too"
    stop(srv, th, a, b)
    # impair_order: under jitter the frames to one connection never overtake each other (TCP)
    srv, th, a, b, vb = pair({"delay": 40, "jitter": 30})
    for i in range(200):
        a.dgram(vb, 3, 4, struct.pack("<I", i))
    seen = [struct.unpack("<I", first_dgram(b)[3])[0] for _ in range(200)]
    assert seen == list(range(200)), seen[:20]
    stop(srv, th, a, b)
    # impair_stall: a stall holds the connection's frames stall_ms more
    srv, th, a, b, vb = pair({"stall": 100, "stall_ms": 200})
    t0 = time.monotonic()
    a.dgram(vb, 3, 4, b"stalled")
    assert first_dgram(b)[3] == b"stalled"
    assert time.monotonic() - t0 >= 0.19, time.monotonic() - t0
    stop(srv, th, a, b)
    # impair_parse
    assert parse_impair("delay=90,jitter=15,stall=2,stall_ms=400") == \
        {"delay": 90, "jitter": 15, "stall": 2.0, "stall_ms": 400}
    assert parse_impair("stall=0.5") == {"stall": 0.5}
    for bad in ("delay=x", "delay", "speed=3", "delay=-1", "stall=101", "jitter=1.5", "delay=1,delay=2", ""):
        try:
            parse_impair(bad)
            raise AssertionError("accepted --impair %r" % bad)
        except argparse.ArgumentTypeError:
            pass
    import contextlib
    import io
    # a malformed value, and the lab option on a beta (public) server, stop main() with an argparse error
    for args in (["--impair", "delay=x"], ["--impair", "delay=90", "--beta", "beta_config.json"]):
        argv = sys.argv
        sys.argv = ["infra_server.py"] + args
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                main()
            raise AssertionError("main() took %r" % args)
        except SystemExit as e:
            assert e.code == 2, (args, e.code)
        finally:
            sys.argv = argv
    # impair_timing: the loop wakes at the due time, not on a 20 ms tick
    srv, th, a, b, vb = pair({"delay": 30})
    late = []
    for i in range(20):
        t0 = time.monotonic()
        a.dgram(vb, 3, 4, b"t")
        first_dgram(b)
        late.append(time.monotonic() - t0 - 0.030)
    late.sort()
    assert late[10] < 0.008, "median lateness %.1f ms" % (late[10] * 1000)
    stop(srv, th, a, b)


def direct_selftest(run, quiet, lines):
    """Direct-path signaling (protocol 3): caps, offers, one pairing per connection pair with a fresh key, the
    host-candidate rule, kind 4 between paired connections only, direct_stats, and no address or key in the logs
    or the database."""
    import os
    import sqlite3
    import tempfile

    tmp = tempfile.mkdtemp(prefix="p3direct_")
    db = os.path.join(tmp, "beta.db")
    first = len(lines)
    offered = ["203.0.113.5", "198.51.100.7", "192.168.1.20", "192.168.1.21", "203.0.113.9", "10.0.0.5"]
    srv = Server("127.0.0.1", 0, None, max_per_addr=16, logger=quiet, db_path=db)
    th = run(srv)
    caps = {"direct": 1}

    def srflx(a):
        return {"t": "srflx", "a": a}

    def direct_events(*cls):
        for cl in cls:
            cl.pump(0.1)
        return [e for cl in cls for e in cl.events if e["ev"].startswith("direct")]

    # direct_caps_answer
    a, b, c, old = (TestClient(srv.port) for _ in range(4))
    la = a.ok("login", online_id="DIR_A", proto=3, caps=caps)
    assert la["direct"] == {"on": True, "stun": DEFAULT_STUN}, la
    b.ok("login", online_id="DIR_B", proto=3, caps=caps)
    c.ok("login", online_id="DIR_C", proto=3, caps=caps)
    lo = old.ok("login", online_id="DIR_OLD", proto=2)
    assert "direct" not in lo, lo
    # offer_validation
    for bad in ([srflx("203.0.113.5:%d" % (4000 + i)) for i in range(5)], [srflx("999.1.1.1:5")],
                [{"t": "host", "a": "10.0.0.1:0"}], [srflx("224.0.0.1:5")], [srflx("0.0.0.0:5")],
                [srflx("255.255.255.255:5")], [{"t": "relay", "a": "203.0.113.5:5"}], [srflx(5)],
                ["203.0.113.5:5"], [], "203.0.113.5:5", [srflx("203.0.113.5:65536")], [srflx("203.0.113.5")],
                [dict(srflx("203.0.113.5:5"), x=1)], [srflx("203.0.113.05:5")]):
        ans = a.req("direct_offer", cands=bad)
        assert not ans["ok"] and ans["error"] == BAD_REQUEST, (bad, ans)
    ans = old.req("direct_offer", cands=[srflx("203.0.113.5:5")])
    assert not ans["ok"] and ans["error"] == BAD_REQUEST, "no caps, no offer"
    # pair_on_join: A creates, both offer, B joins
    hide = a.ok("create", max_slot=4, ints=GAME_INTS)["room"]
    a.ok("direct_offer", cands=[srflx("203.0.113.5:4000"), {"t": "host", "a": "192.168.1.20:4001"}])
    b.ok("direct_offer", cands=[srflx("198.51.100.7:5000"), {"t": "host", "a": "192.168.1.21:5001"}])
    assert not direct_events(a, b), "no pairing before a shared room"
    b.ok("join", room_id=hide["id"])
    pa = a.event(lambda e: e["ev"] == "direct_peer", what="A: direct_peer")
    pb = b.event(lambda e: e["ev"] == "direct_peer", what="B: direct_peer")
    assert pa["pair"] == pb["pair"] and pa["pair"] and pa["key"] == pb["key"], (pa, pb)
    assert re.fullmatch(r"[0-9a-f]{64}", pa["key"]), pa
    assert (pa["vip"], pb["vip"]) == (0x0A500002, 0x0A500001), (pa, pb)
    assert pa["cands"] == [srflx("198.51.100.7:5000")], pa
    assert pb["cands"] == [srflx("203.0.113.5:4000")], pb
    # no_caps_no_pair: a protocol-2 client joins the same room; C (caps, no offer yet) too
    old.ok("join", room_id=hide["id"])
    c.ok("join", room_id=hide["id"])
    time.sleep(0.3)
    assert not direct_events(a, b, c, old), "nobody paired with OLD or with C before its offer"
    assert len(srv.pairs) == 1, srv.pairs
    # host_rule_same_nat: C behind A's NAT (same srflx IP) offers -> pairs with A (hosts included) and B (not)
    c.ok("direct_offer", cands=[srflx("203.0.113.5:4100"), {"t": "host", "a": "10.0.0.5:4101"}])
    ca = c.event(lambda e: e["ev"] == "direct_peer" and e["vip"] == 0x0A500001, what="C: peer A")
    cb = c.event(lambda e: e["ev"] == "direct_peer" and e["vip"] == 0x0A500002, what="C: peer B")
    ac = a.event(lambda e: e["ev"] == "direct_peer" and e["vip"] == 0x0A500003, what="A: peer C")
    b.event(lambda e: e["ev"] == "direct_peer" and e["vip"] == 0x0A500003, what="B: peer C")
    assert ca["cands"] == [srflx("203.0.113.5:4000"), {"t": "host", "a": "192.168.1.20:4001"}], ca
    assert ac["cands"] == [srflx("203.0.113.5:4100"), {"t": "host", "a": "10.0.0.5:4101"}], ac
    assert cb["cands"] == [srflx("198.51.100.7:5000")], cb
    assert len({ca["key"], cb["key"], pa["key"]}) == 3 and len({ca["pair"], cb["pair"], pa["pair"]}) == 3
    # kind4_only_when_paired: A->B delivered byte for byte with A's vip; A->OLD (no pairing) and an oversize frame
    # are dropped and counted
    a.send_raw_frame(frame(4, struct.pack("<I", 0x0A500002) + b"sealed bytes"))
    a.send_raw_frame(frame(4, struct.pack("<I", 0x0A500004) + b"to old"))
    a.send_raw_frame(frame(4, struct.pack("<I", 0x0A500002) + b"x" * 1501))
    deadline = time.monotonic() + 3
    while not b.channels:
        assert time.monotonic() < deadline, "B: no kind 4"
        b.pump(0.1)
    time.sleep(0.3)
    b.pump(0.1)
    old.pump(0.2)
    assert b.channels == [(0x0A500001, b"sealed bytes")], b.channels
    assert not old.channels
    conn_a = next(x for x in srv.conns.values() if x.online_id == "DIR_A")
    assert conn_a.dgram_dropped == 2, conn_a.dgram_dropped
    # pair_once_across_rooms: A and B also share a VS room; leaving it sends no drop
    vs = a.ok("create", max_slot=2, ints=GAME_INTS)["room"]
    b.ok("join", room_id=vs["id"])
    b.ok("leave", room_id=vs["id"])
    time.sleep(0.3)
    assert not direct_events(a, b), "one pairing across rooms"
    assert len(srv.pairs) == 3, srv.pairs
    # reoffer_rekeys: B offers again -> drop, then a new peer with a new key and pair id
    b.ok("direct_offer", cands=[srflx("198.51.100.7:5002")])
    seen = []
    deadline = time.monotonic() + 3
    while len(seen) < 2:
        assert time.monotonic() < deadline, a.events
        a.pump(0.2)
        seen = [e for e in a.events if e["ev"] in ("direct_drop", "direct_peer")]
    assert [e["ev"] for e in seen] == ["direct_drop", "direct_peer"], seen
    assert seen[0]["pair"] == pa["pair"] and seen[1]["pair"] != pa["pair"] and seen[1]["key"] != pa["key"], seen
    assert seen[1]["cands"] == [srflx("198.51.100.7:5002")], seen
    a.events.clear()
    # leaving the last shared room sends direct_drop to the one left, and kind 4 stops
    b.ok("leave", room_id=hide["id"])
    d = a.event(lambda e: e["ev"] == "direct_drop", what="A: drop B")
    assert d["vip"] == 0x0A500002 and d["pair"] == seen[1]["pair"], d
    c.event(lambda e: e["ev"] == "direct_drop" and e["vip"] == 0x0A500002, what="C: drop B")
    a.send_raw_frame(frame(4, struct.pack("<I", 0x0A500002) + b"after drop"))
    time.sleep(0.3)
    b.pump(0.2)
    assert b.channels == [(0x0A500001, b"sealed bytes")], "kind 4 after the drop"
    # offer rate: A offered once; three more pass, the fifth within a minute is refused
    for _ in range(3):
        a.ok("direct_offer", cands=[srflx("203.0.113.5:4000")])
    ans = a.req("direct_offer", cands=[srflx("203.0.113.5:4000")])
    assert not ans["ok"] and ans["error"] == SERVICE_UNAVAILABLE, ans
    # one disconnects: C closes -> A gets direct_drop for C
    a.events.clear()
    c.close()
    a.event(lambda e: e["ev"] == "direct_drop" and e["vip"] == 0x0A500003, what="A: drop C on disconnect")
    deadline = time.monotonic() + 3
    while srv.pairs:
        assert time.monotonic() < deadline, srv.pairs
        time.sleep(0.05)
    # direct_stats_stored, invalid forms refused
    good = {"path": "direct", "rtt_p50": 40, "rtt_p95": 90, "loss_x10": 12, "direct_s": 300, "fallbacks": 1}
    a.ok("direct_stats", pairs=[good, dict(good, path="relay", direct_s=0)])
    a.ok("direct_stats", pairs=[dict(good, relay_p50=210, relay_p95=300, relay_s=95)])
    assert any(line.endswith("relay p50 210 p95 300 95 s") for line in lines[first:]), "relay numbers logged"
    for bad in ([dict(good, relay_p50=210)], [dict(good, relay_p50=1, relay_p95=1, relay_s=-1)],
                [dict(good, relay_p50=1, relay_p95="x", relay_s=1)],
                [dict(good, path="udp")], [dict(good, rtt_p50=-1)], [dict(good, rtt_p95=600001)],
                [dict(good, loss_x10=1.5)], [dict(good, ip="203.0.113.5")], [good] * 9, good,
                [dict(good, fallbacks=True)], [{k: v for k, v in good.items() if k != "direct_s"}]):
        ans = a.req("direct_stats", pairs=bad)
        assert not ans["ok"] and ans["error"] == BAD_REQUEST, (bad, ans)
    srv.metrics.flush(3)
    with sqlite3.connect(db) as cx:
        rows = cx.execute("SELECT path, rtt_p50, rtt_p95, loss_x10, direct_s, fallbacks, relay_p50, relay_p95, relay_s"
                          " FROM direct_stats").fetchall()
        assert rows == [("direct", 40, 90, 12, 300, 1, None, None, None), ("relay", 40, 90, 12, 0, 1, None, None, None),
                        ("direct", 40, 90, 12, 300, 1, 210, 300, 95)], rows
        kinds = [r[0] for r in cx.execute("SELECT kind FROM events WHERE kind LIKE 'direct%'")]
        assert "direct_pair" in kinds and "direct_drop" in kinds, kinds
        assert not cx.execute("SELECT COUNT(*) FROM events WHERE kind LIKE 'direct%' AND detail IS NOT NULL"
                              ).fetchone()[0]
        dump = "\n".join(str(r) for t in ("sessions", "rooms", "events", "direct_stats")
                         for r in cx.execute("SELECT * FROM " + t))
    # privacy: no offered address and no key in any log line or table
    keys = [pa["key"], ca["key"], cb["key"], seen[1]["key"]]
    for line in lines[first:] + [dump]:
        assert not any(ip in line for ip in offered) and not any(k in line for k in keys), line
    assert any("direct" in line for line in lines[first:]), "direct-path log lines exist"
    for cl in (a, b, old):
        cl.close()
    srv.running = False
    th.join(timeout=5)
    # --no-direct: caps answered with on false and the --stun list; offers accepted, nobody paired
    srv = Server("127.0.0.1", 0, None, logger=quiet, no_direct=True, stun=["stun.example.org:3478"])
    th = run(srv)
    # a send failure inside unpair drops a connection, whose own prune may end the next pairing first: the outer
    # unpair must then be a no-op (KeyError here took the beta server down, 30 Oct)
    srv.unpair(frozenset((object(), object())))
    a, b = TestClient(srv.port), TestClient(srv.port)
    ans = a.ok("login", online_id="ND_A", proto=3, caps=caps)
    assert ans["direct"] == {"on": False, "stun": ["stun.example.org:3478"]}, ans
    b.ok("login", online_id="ND_B", proto=3, caps=caps)
    hide = a.ok("create", max_slot=4, ints=GAME_INTS)["room"]
    b.ok("join", room_id=hide["id"])
    a.ok("direct_offer", cands=[srflx("203.0.113.5:4000")])
    b.ok("direct_offer", cands=[srflx("198.51.100.7:5000")])
    time.sleep(0.3)
    assert not srv.pairs and not direct_events(a, b)
    for cl in (a, b):
        cl.close()
    srv.running = False
    th.join(timeout=5)
    # beta config "direct": false -> off; the reload that turns it on pairs the waiting offers, and off drops them
    cfg = os.path.join(tmp, "beta.json")

    def write_cfg(on):
        with open(cfg, "w", encoding="utf-8") as f:
            json.dump({"starts": "2026-01-01T00:00:00Z", "ends": "2099-01-01T00:00:00Z", "public": True,
                       "direct": on}, f)
        os.utime(cfg, ns=(time.time_ns(), time.time_ns() + (1 if on else 0)))
    write_cfg(False)
    srv = Server("127.0.0.1", 0, None, logger=quiet, beta_path=cfg)
    th = run(srv)
    a, b = TestClient(srv.port), TestClient(srv.port)
    assert a.ok("login", online_id="BD_A", proto=3, caps=caps)["direct"]["on"] is False
    b.ok("login", online_id="BD_B", proto=3, caps=caps)
    hide = a.ok("create", max_slot=4, ints=GAME_INTS)["room"]
    b.ok("join", room_id=hide["id"])
    a.ok("direct_offer", cands=[srflx("203.0.113.5:4000")])
    b.ok("direct_offer", cands=[srflx("198.51.100.7:5000")])
    write_cfg(True)
    a.event(lambda e: e["ev"] == "direct_peer", timeout=4, what="A: paired after the reload")
    b.event(lambda e: e["ev"] == "direct_peer", what="B: paired after the reload")
    write_cfg(False)
    a.event(lambda e: e["ev"] == "direct_drop", timeout=4, what="A: dropped when direct turns off")
    for cl in (a, b):
        cl.close()
    srv.running = False
    th.join(timeout=5)


def link_stats_selftest(run, quiet, lines):
    """Relay-link telemetry: link_stats stored on the session row, strict numbers, old databases migrated."""
    import os
    import sqlite3
    import tempfile

    tmp = tempfile.mkdtemp(prefix="p3link_")
    db = os.path.join(tmp, "beta.db")

    def q(sql, *params):
        with sqlite3.connect(db) as c:
            return c.execute(sql, params).fetchall()

    def columns(path):
        with sqlite3.connect(path) as c:
            return [r[1] for r in c.execute("PRAGMA table_info(sessions)")]

    six = ("pings", "slow_pings", "rtt_p50", "rtt_p95", "rtt_max", "max_rx_gap_ms")
    # link_migration: a beta night 1/2 database (no link columns) gets them once, rows kept
    with sqlite3.connect(db) as c:
        c.executescript("""
CREATE TABLE sessions(id INTEGER PRIMARY KEY, label TEXT, staff INT, online_id TEXT, transport TEXT,
    proto INT, start_utc TEXT, end_utc TEXT, end_reason TEXT, bytes_in INT, bytes_out INT, dgrams_dropped INT);
INSERT INTO sessions(id, label, online_id, end_utc) VALUES (1, 'invitado 01', 'OLD', '2026-10-23T03:00:00Z');
CREATE TABLE direct_stats(session INT, path TEXT, rtt_p50 INT, rtt_p95 INT, loss_x10 INT, direct_s INT, fallbacks INT);
INSERT INTO direct_stats VALUES (1, 'direct', 225, 1299, 0, 2580, 0);
""")
    MetricsDb(db, quiet)
    cols = columns(db)
    assert all(cols.count(k) == 1 for k in six), cols
    MetricsDb(db, quiet)
    assert columns(db) == cols, "a migrated file is left alone"
    assert q("SELECT online_id, pings, rtt_p95 FROM sessions WHERE id=1") == [("OLD", None, None)]
    assert q("SELECT rtt_p95, relay_p50, relay_s FROM direct_stats") == [(1299, None, None)], "u7 rows kept"

    srv = Server("127.0.0.1", 0, None, logger=quiet, db_path=db)
    th = run(srv)
    a = TestClient(srv.port)
    early = a.req("link_stats", pings=1, slow_pings=0, rtt_p50=1, rtt_p95=1, rtt_max=1, max_rx_gap_ms=1)
    assert not early["ok"] and early["error"] == FORBIDDEN, early
    a.ok("login", online_id="LINK_A", proto=2)
    good = dict(pings=120, slow_pings=3, rtt_p50=48, rtt_p95=310, rtt_max=900, max_rx_gap_ms=650)
    assert a.ok("link_stats", **good) == {"id": a.next_id, "ok": True}
    srv.metrics.flush(3)

    def row():
        return q("SELECT " + ", ".join(six) + " FROM sessions WHERE online_id='LINK_A'")
    assert row() == [(120, 3, 48, 310, 900, 650)], row()
    assert any(" link: p50 48 p95 310 max 900 slow 3/120 gap 650" in l for l in lines), lines[-5:]
    # link_overwrite: a second report replaces the first, no new row
    again = dict(good, pings=200, rtt_max=950)
    a.ok("link_stats", **again)
    srv.metrics.flush(3)
    assert row() == [(200, 3, 48, 310, 950, 650)], row()
    assert q("SELECT COUNT(*) FROM sessions") == [(2,)]
    # link_refuses_bad_numbers: refused with BAD_REQUEST and nothing stored
    bad_forms = [dict(good, rtt_p50=-1), dict(good, rtt_max=600001), dict(good, rtt_p95=1.5),
                 dict(good, pings="10"), dict(good, slow_pings=True), dict(good, ip="1.2.3.4"),
                 {k: v for k, v in good.items() if k != "max_rx_gap_ms"}, dict(good, pings=None)]
    for form in bad_forms:
        ans = a.req("link_stats", **form)
        assert not ans["ok"] and ans["error"] == BAD_REQUEST, (form, ans)
    srv.metrics.flush(3)
    assert row() == [(200, 3, 48, 310, 950, 650)], row()
    dump = "\n".join(str(r) for r in q("SELECT * FROM sessions"))
    assert "127.0.0.1" not in dump and "1.2.3.4" not in dump, "no address stored"
    a.close()
    srv.running = False
    th.join(timeout=5)


def beta_selftest(run, quiet, lines):
    """Beta gate: invite codes, window, protocol version, limits, /beta routes."""
    import datetime
    import http.client
    import os
    import tempfile

    def http_req(port, method, path, body=None):
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        headers = {"Content-Type": "application/json"} if body is not None else {}
        c.request(method, path, json.dumps(body) if body is not None else None, headers)
        r = c.getresponse()
        data = r.read()
        c.close()
        if (r.getheader("Content-Type") or "").startswith("application/json"):
            data = json.loads(data)
        return r.status, data

    def refused(cl, code, **kw):
        a = cl.req("login", **kw)
        assert not a["ok"] and a["error"] == code, (kw, a, "want 0x%08X" % code)

    # no_config_unchanged
    srv = Server("127.0.0.1", 0, 0, logger=quiet)
    th = run(srv)
    n = TestClient(srv.port)
    n.ok("login", online_id="NOCFG")
    st, body = http_req(srv.ws_port, "GET", "/beta")
    assert st == 200 and body["open"] is True and body["starts"] is None and body["ends"] is None, (st, body)
    four = [TestClient(srv.port) for _ in range(3)]
    assert TestClient(srv.port).is_closed(), "without --beta the 5th connection from an address is refused as before"
    for cl in [n] + four:
        cl.close()
    srv.running = False
    th.join(timeout=2)

    tmp = tempfile.mkdtemp(prefix="p3beta_")
    cfg_path = os.path.join(tmp, "beta_config.json")
    starts, ends = "2026-10-24T03:00:00Z", "2026-10-26T05:59:59Z"
    t_start = datetime.datetime(2026, 10, 24, 3, 0, 0, tzinfo=datetime.timezone.utc).timestamp()
    t_end = datetime.datetime(2026, 10, 26, 5, 59, 59, tzinfo=datetime.timezone.utc).timestamp()
    codes = {"P3-K7QF-2MXD": ["invitado 07", True, False], "P3-AAAA-BBBB": ["invitado 08", True, False],
             "P3-CCCC-DDDD": ["invitado 10", True, False], "P3-EEEE-FFFF": ["invitado 11", True, False],
             "P3-STAF-F222": ["staff 01", True, True], "P3-NNNN-PPPP": ["invitado 09", False, False]}

    def write_cfg(bump=0, public=False):
        cfg = {"starts": starts, "ends": ends, "min_proto": 2, "max_per_code": 2, "public": public,
               "codes": [{"label": lab, "sha256": hashlib.sha256(c.replace("-", "").encode()).hexdigest(),
                          "enabled": en, "staff": staff} for c, (lab, en, staff) in codes.items()]}
        with open(cfg_path, "w", encoding="utf-8") as f:
            json.dump(cfg, f)
        if bump:
            st_ = os.stat(cfg_path)
            os.utime(cfg_path, ns=(st_.st_atime_ns, st_.st_mtime_ns + bump * 1_000_000_000))
    write_cfg()
    now = [t_start + 3600]
    srv = Server("127.0.0.1", 0, 0, logger=quiet, beta_path=cfg_path, clock=lambda: now[0])
    th = run(srv)
    by_id = lambda oid: next(c for c in list(srv.conns.values()) if c.online_id == oid)

    # unknown_code + outdated_proto
    u = TestClient(srv.port)
    refused(u, INVITE_UNKNOWN, online_id="UNK", proto=2, invite="P3-ZZZZ-ZZZZ")
    refused(u, INVITE_UNKNOWN, online_id="UNK", proto=2)
    refused(u, OUTDATED, online_id="UNK", invite="P3-K7QF-2MXD")
    refused(u, OUTDATED, online_id="UNK", proto=1, invite="P3-K7QF-2MXD")
    assert any("login refused" in l and "outdated" in l for l in lines), "refusals logged"
    # code_normalized
    k = TestClient(srv.port)
    k.ok("login", online_id="P3K1", proto=2, invite=" p3-k7qf-2mxd ")
    kc = by_id("P3K1")
    assert (kc.code_label, kc.staff, kc.proto) == ("invitado 07", False, 2), (kc.code_label, kc.staff, kc.proto)
    s = TestClient(srv.port)
    s.ok("login", online_id="STAFF1", proto=2, invite="P3STAFF222")
    assert by_id("STAFF1").staff is True
    # window_closed: before the start and after the end invitees are refused and staff is not; an invitee's open
    # session ends with the window.
    for t in (t_start - 1, t_end + 1):
        now[0] = t
        refused(u, BETA_CLOSED, online_id="LATE", proto=2, invite="P3-AAAA-BBBB")
        w = TestClient(srv.port)
        w.ok("login", online_id="STAFF2", proto=2, invite="P3-STAF-F222")
        w.close()
    assert k.is_closed(), "an invitee's session closes when the window ends"
    assert s.ok("ping", t=1)["t"] == 1, "staff stays"
    now[0] = t_start + 7200
    # max_two_per_code
    c1, c2, c3 = TestClient(srv.port), TestClient(srv.port), TestClient(srv.port)
    c1.ok("login", online_id="P3B1", proto=2, invite="P3-AAAA-BBBB")
    c2.ok("login", online_id="P3B2", proto=2, invite="P3-AAAA-BBBB")
    refused(c3, INVITE_IN_USE, online_id="P3B3", proto=2, invite="P3-AAAA-BBBB")
    c1.close()
    deadline = time.monotonic() + 3
    while True:
        a = c3.req("login", online_id="P3B3", proto=2, invite="P3-AAAA-BBBB", console="02000000004a")
        if a["ok"]:
            break
        assert time.monotonic() < deadline, "the code is free again after one session closes: %r" % a
        time.sleep(0.05)
    # The code is at its limit and P3B3's game froze: its re-login does not count the stale session and replaces it.
    c4 = TestClient(srv.port)
    c4.ok("login", online_id="P3B3", proto=2, invite="P3-AAAA-BBBB", console="02000000004a")
    assert c3.is_closed(), "stale session of a code at its limit replaced"
    # disable_live: the open session stays, the next login is refused.
    k2 = TestClient(srv.port)
    k2.ok("login", online_id="P3K2", proto=2, invite="P3-K7QF-2MXD")
    codes["P3-K7QF-2MXD"][1] = False
    write_cfg(bump=2)
    k3 = TestClient(srv.port)
    deadline = time.monotonic() + 2
    while True:
        a = k3.req("login", online_id="P3K3", proto=2, invite="P3-K7QF-2MXD")
        if not a["ok"] and a["error"] == INVITE_DISABLED:
            break
        assert time.monotonic() < deadline, "disabled code still logs in: %r" % a
        if a["ok"]:
            k3.close()
            k3 = TestClient(srv.port)
        time.sleep(0.1)
    assert k2.ok("ping", t=2)["t"] == 2, "the open session of a disabled code stays"
    # a broken file keeps the previous config
    with open(cfg_path, "w", encoding="utf-8") as f:
        f.write("{broken")
    st_ = os.stat(cfg_path)
    os.utime(cfg_path, ns=(st_.st_atime_ns, st_.st_mtime_ns + 4_000_000_000))
    time.sleep(1.3)
    assert c2.ok("ping", t=3)["t"] == 3
    v = TestClient(srv.port)
    refused(v, INVITE_DISABLED, online_id="P3K4", proto=2, invite="P3-K7QF-2MXD")
    assert any("beta config" in l and "keeping" in l for l in lines), "broken config logged"
    write_cfg(bump=6)
    for cl in (k2, k3, v, c3, c4):
        cl.close()
    time.sleep(0.3)
    # two_codes_one_address: 4 sessions of two codes from 127.0.0.1 (c2 + three more), the 5th is refused.
    d1, d2, d3, d4 = (TestClient(srv.port) for _ in range(4))
    d1.ok("login", online_id="P3B4", proto=2, invite="P3-AAAA-BBBB")
    d2.ok("login", online_id="P3C1", proto=2, invite="P3-CCCC-DDDD")
    d3.ok("login", online_id="P3C2", proto=2, invite="P3-CCCC-DDDD")
    refused(d4, SERVICE_UNAVAILABLE, online_id="P3E1", proto=2, invite="P3-EEEE-FFFF")
    far = TestClient(srv.ws_port, ws=True, headers="CF-Connecting-IP: 203.0.113.9\r\n")
    assert far.status == 101
    far.ok("login", online_id="P3E2", proto=2, invite="P3-EEEE-FFFF")
    # rooms_per_address: the four invitee sessions of 127.0.0.1 own 8 rooms at most; staff is not counted.
    made = 0
    for cl in (c2, d1, d2, d3):
        for _ in range(2):
            cl.ok("create", max_slot=4, ints=GAME_INTS)
            made += 1
    ninth = d1.req("create", max_slot=4, ints=GAME_INTS)
    assert made == 8 and not ninth["ok"] and ninth["error"] == SERVICE_UNAVAILABLE, ninth
    s.ok("create", max_slot=4, ints=GAME_INTS)
    far.ok("create", max_slot=4, ints=GAME_INTS)
    # http_beta_status
    st, body = http_req(srv.ws_port, "GET", "/beta")
    assert st == 200 and body["open"] is True and (body["starts"], body["ends"]) == (starts, ends), (st, body)
    assert body["min_proto"] == 2 and body["now"].endswith("Z"), body
    now[0] = t_start - 60
    st, body = http_req(srv.ws_port, "GET", "/beta")
    assert body["open"] is False, body
    # http_beta_check
    checks = 0

    def check(code):
        nonlocal checks
        checks += 1
        return http_req(srv.ws_port, "POST", "/beta/check", {"code": code})
    assert check("P3-AAAA-BBBB") == (200, {"result": "closed"})
    assert check("p3 staf f222") == (200, {"result": "ok", "label": "staff 01"})
    now[0] = t_start + 60
    assert check(" p3-aaaa-bbbb ") == (200, {"result": "ok", "label": "invitado 08"})
    assert check("P3-K7QF-2MXD") == (200, {"result": "disabled"})
    assert check("P3-ZZZZ-ZZZZ") == (200, {"result": "unknown"})
    assert check("") == (200, {"result": "unknown"})
    st, _ = http_req(srv.ws_port, "POST", "/beta/check", None)
    checks += 1
    assert st == 400, st
    while checks < 30:
        assert check("P3-ZZZZ-ZZZZ")[0] == 200, checks
    assert check("P3-ZZZZ-ZZZZ")[0] == 429, "31st check in a minute"
    st, body = http_req(srv.ws_port, "GET", "/")
    assert st == 200 and body == b"Patapon 3 infrastructure server\n", (st, body)
    assert http_req(srv.ws_port, "GET", "/beta")[1]["public"] is False
    # public: no code (or an unknown one) logs in as "público" during the window; codes keep their label and staff.
    write_cfg(bump=8, public=True)
    deadline = time.monotonic() + 3
    while http_req(srv.ws_port, "GET", "/beta")[1].get("public") is not True:
        assert time.monotonic() < deadline, "public config not reloaded"
        time.sleep(0.1)
    pub = [TestClient(srv.ws_port, ws=True, headers="CF-Connecting-IP: 198.51.100.%d\r\n" % i) for i in range(1, 6)]
    pub[0].ok("login", online_id="PUB1", proto=2)
    pub[1].ok("login", online_id="PUB2", proto=2, invite="P3-ZZZZ-ZZZZ")
    pub[2].ok("login", online_id="PUB3", proto=2, invite="P3-STAF-F222")
    assert (by_id("PUB1").code_label, by_id("PUB1").staff) == ("público", False)
    assert (by_id("PUB2").code_label, by_id("PUB2").staff) == ("público", False)
    assert by_id("PUB3").staff is True, "a staff code stays staff"
    refused(pub[3], OUTDATED, online_id="PUB4", proto=1)
    refused(pub[3], INVITE_DISABLED, online_id="PUB4", proto=2, invite="P3-NNNN-PPPP")
    now[0] = t_end + 1
    refused(pub[4], BETA_CLOSED, online_id="PUB5", proto=2)
    now[0] = t_start + 60
    for cl in pub:
        cl.close()
    write_cfg(bump=10)
    for cl in (u, s, c2, d1, d2, d3, d4, far):
        cl.close()
    srv.running = False
    th.join(timeout=2)

    # ---- metrics in SQLite ----
    import sqlite3
    db = os.path.join(tmp, "beta.db")

    def q(sql, *params):
        with sqlite3.connect(db) as c:
            return c.execute(sql, params).fetchall()
    now[0] = t_start + 3600
    srv = Server("127.0.0.1", 0, None, logger=quiet, beta_path=cfg_path, clock=lambda: now[0], db_path=db)
    th = run(srv)
    # metrics_rows
    ma, mb, mr = TestClient(srv.port), TestClient(srv.port), TestClient(srv.port)
    ma.ok("login", online_id="MET_A", proto=2, invite="P3-AAAA-BBBB")
    mb.ok("login", online_id="MET_B", proto=2, invite="P3-CCCC-DDDD")
    refused(mr, INVITE_UNKNOWN, online_id="MET_R", proto=2, invite="P3-ZZZZ-ZZZZ")
    hide = ma.ok("create", max_slot=4, flag_attr=0x04000000, ints=GAME_INTS)["room"]
    mb.ok("join", room_id=hide["id"])
    vs = ma.ok("create", max_slot=4, ints=[[0x4D, 3]])["room"]
    mb.ok("leave", room_id=hide["id"])
    bad = mb.req("join", room_id=12345)
    assert not bad["ok"] and bad["error"] == NO_SUCH_ROOM, bad
    srv.metrics.flush(3)
    sess = q("SELECT id, label, staff, online_id, transport, proto, start_utc, end_utc FROM sessions ORDER BY id")
    assert [r[1:6] for r in sess] == [("invitado 08", 0, "MET_A", "tcp", 2), ("invitado 10", 0, "MET_B", "tcp", 2)], sess
    assert all(r[6].endswith("Z") and r[7] is None for r in sess), sess
    sid_a, sid_b = sess[0][0], sess[1][0]
    rooms = q("SELECT id, kind, owner_session, max_members, closed_utc FROM rooms ORDER BY created_utc, id")
    assert rooms == [(hide["id"], "hideout", sid_a, 2, None), (vs["id"], "vs", sid_a, 1, None)], rooms
    ev = q("SELECT session, kind, room, detail FROM events ORDER BY rowid")
    kinds = [e[1] for e in ev]
    assert ("refused" in kinds and "join" in kinds and "leave" in kinds and "error" in kinds), ev
    assert any(e[1] == "refused" and "invite unknown" in e[3] for e in ev), ev
    assert (sid_b, "join", hide["id"]) in [e[:3] for e in ev] and (sid_b, "leave", hide["id"]) in [e[:3] for e in ev]
    dump = "\n".join(str(r) for t in ("sessions", "rooms", "events") for r in q("SELECT * FROM " + t))
    assert "127.0.0.1" not in dump and "P3AAAABBBB" not in dump, "no address or code stored"
    # metrics_never_block: the writer is stuck behind another process's lock on the file.
    lock = sqlite3.connect(db, isolation_level=None)
    lock.execute("BEGIN EXCLUSIVE")
    t0 = time.monotonic()
    for i in range(2000):
        srv.metrics.put("INSERT INTO events(t_utc, kind, detail) VALUES (?, ?, ?)", ("x", "error", "nb%d" % i))
    assert time.monotonic() - t0 < 0.5, "metrics waited for the database"
    assert ma.ok("ping", t=5)["t"] == 5, "served while the database is locked"
    lock.execute("ROLLBACK")
    lock.close()
    srv.metrics.flush(10)
    assert q("SELECT COUNT(*) FROM events WHERE detail LIKE 'nb%'") == [(2000,)]
    # metrics_survive_restart: the server stops with two sessions open; a second server appends.
    srv.running = False
    th.join(timeout=5)
    assert q("SELECT end_reason FROM sessions WHERE id IN (?, ?)", sid_a, sid_b) == [("server_stop",)] * 2
    assert q("SELECT COUNT(*) FROM rooms WHERE closed_utc IS NULL") == [(0,)]
    for cl in (ma, mb, mr):
        cl.close()
    srv = Server("127.0.0.1", 0, None, logger=quiet, beta_path=cfg_path, clock=lambda: now[0], db_path=db)
    th = run(srv)
    mc = TestClient(srv.port)
    mc.ok("login", online_id="MET_C", proto=2, invite="P3-AAAA-BBBB")
    srv.metrics.flush(3)
    assert [r[0] for r in q("SELECT online_id FROM sessions ORDER BY id")] == ["MET_A", "MET_B", "MET_C"]
    # a server killed without stopping: the next start closes what was left open
    srv.metrics.flush(3)
    MetricsDb(db, quiet)
    assert q("SELECT end_reason FROM sessions WHERE online_id='MET_C'") == [("server_stop",)]
    mc.close()
    srv.running = False
    th.join(timeout=5)
    # metrics are a no-op without --db
    assert MetricsDb(None).put("anything") is None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27320, help="raw TCP listener")
    ap.add_argument("--ws-port", type=int, default=27321, help="WebSocket listener (behind the tunnel)")
    ap.add_argument("--ws-path", default="/infra")
    ap.add_argument("--max-rooms", type=int, default=256)
    ap.add_argument("--idle", type=float, default=30.0, help="seconds without a frame before a session closes")
    ap.add_argument("--stats-every", type=float, default=60.0)
    ap.add_argument("--delay-ms", type=int, default=0, help="extra latency on datagrams and pings (tests)")
    ap.add_argument("--loss", type=float, default=0, help="percent of game datagrams dropped (tests)")
    ap.add_argument("--impair", type=parse_impair, default=None, metavar="delay=MS,jitter=MS,stall=PCT,stall_ms=MS",
                    help="lab only: every frame to a connection waits delay +- jitter ms in order; stall %% of them "
                         "first hold the connection stall_ms more (TCP head-of-line after a lost segment)")
    ap.add_argument("--max-per-addr", type=int, default=4)
    ap.add_argument("--signal-timeout", type=float, default=15.0,
                    help="seconds a member may take to bind its game socket before its peers get SIGNALING_DEAD")
    ap.add_argument("--beta", help="beta config (invite codes, window, minimum protocol); none = no gate")
    ap.add_argument("--db", help="SQLite file for beta metrics (sessions, rooms, events); none = no metrics")
    ap.add_argument("--log", help="also append every log line to this file (the console keeps them too)")
    ap.add_argument("--status", help="write the public lobby summary (players, rooms) to this JSON file every 2 s")
    ap.add_argument("--no-direct", action="store_true",
                    help="no direct-path pairing: every client stays on the relay (login answers direct.on false)")
    ap.add_argument("--stun", type=lambda v: [x for x in v.split(",") if x], default=None, metavar="HOST:PORT,...",
                    help="STUN servers sent to clients for the direct path (default %s)" % ",".join(DEFAULT_STUN))
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.impair and args.beta:
        ap.error("--impair is a lab option: never on the beta (public) server")
    alog = AsyncLog(sink=tee_sink(args.log) if args.log else None)
    srv = Server(args.host, args.port, args.ws_port, args.ws_path, args.max_rooms, args.idle, args.stats_every,
                 args.delay_ms, args.max_per_addr, signal_timeout=args.signal_timeout, loss=args.loss, logger=alog,
                 beta_path=args.beta, db_path=args.db, impair=args.impair, status_path=args.status,
                 no_direct=args.no_direct, stun=args.stun)
    if disable_quick_edit():
        alog("console QuickEdit turned off: a click in this window no longer pauses the server")
    alog("infrastructure server: tcp %s:%d, ws %s:%d%s" % (args.host, srv.port, args.host, srv.ws_port, args.ws_path))
    if args.impair:
        alog("LAB: link impairment on every connection: %s" % ", ".join("%s=%s" % kv for kv in args.impair.items()))
    if srv.beta:
        b = srv.beta
        alog("beta gate: %d codes (%d staff), open %s .. %s UTC, min proto %d, %d per code"
             % (len(b["codes"]), sum(1 for c in b["codes"].values() if c["staff"]), b["starts"], b["ends"],
                b["min_proto"], b["max_per_code"]))
    try:
        srv.serve()
    except KeyboardInterrupt:
        srv.running = False


if __name__ == "__main__":
    sys.exit(main())
