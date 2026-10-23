#!/usr/bin/env python3
"""Load test of the infrastructure server (tools/infra_server.py) with synthetic players (stdlib only).

Clients log in (with a staff invite code on a gated server), form groups of 4 like the game's hideouts (the first
of each group creates a room with the game's ints, the other three join), and each pair of groups plays two VS
matches: the first members of both groups share one VS room and the second members another (a player in its
hideout room and a VS room at once, as the game does). Every client then sends 30 datagrams a second of 150 bytes
to each player it shares a room with, carrying a send timestamp, and pings every 2 s. The report counts
connections, refusals, errors and datagrams and gives the delivery delay and ping percentiles.

    python -I tools/infra_loadtest.py --server tcp://127.0.0.1:27320 --clients 50 --minutes 10 --code P3-XXXX-XXXX
    python -I tools/infra_loadtest.py --server wss://online.montick.app/infra --clients 25 --minutes 2 --code ...
    python -I tools/infra_loadtest.py --selftest
"""
import argparse
import base64
import json
import os
import secrets
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.parse

GAME_INTS = [[0x4C, 0], [0x4D, 0x30005], [0x4E, 1], [0x4F, 1], [0x50, 40], [0x51, 1], [0x52, 1]]
VS_INTS = [[0x4C, 0], [0x4D, 3], [0x4E, 1], [0x4F, 1], [0x50, 40], [0x51, 1], [0x52, 1]]
DGRAM_HZ, DGRAM_BYTES, PING_S = 30, 150, 2.0
GAME_PORT = 0x1F90
BUCKET_MS = 0.1
BUCKETS = 20000  # 0..2000 ms


class Hist:
    def __init__(self):
        self.n = [0] * (BUCKETS + 1)
        self.count = 0
        self.max = 0.0
        self.lock = threading.Lock()

    def add(self, ms):
        with self.lock:
            self.n[min(BUCKETS, max(0, int(ms / BUCKET_MS)))] += 1
            self.count += 1
            self.max = max(self.max, ms)

    def pct(self, p):
        if not self.count:
            return None
        want = self.count * p / 100.0
        acc = 0
        for i, k in enumerate(self.n):
            acc += k
            if acc >= want:
                return round((i + 1) * BUCKET_MS, 1)
        return round(self.max, 1)

    def summary(self):
        return {"p50": self.pct(50), "p99": self.pct(99), "max": round(self.max, 1), "samples": self.count}


class Conn:
    """One player's connection: raw TCP or WebSocket (ws:// / wss://), frames of tools/infra_server.py."""

    def __init__(self, url, timeout=10):
        u = urllib.parse.urlparse(url)
        self.ws = u.scheme in ("ws", "wss")
        port = u.port or {"tcp": 27320, "ws": 80, "wss": 443}[u.scheme]
        sock = socket.create_connection((u.hostname, port), timeout=timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if u.scheme == "wss":
            sock = ssl.create_default_context().wrap_socket(sock, server_hostname=u.hostname)
        self.sock = sock
        self.rx = bytearray()
        self.wlock = threading.Lock()
        if self.ws:
            key = base64.b64encode(secrets.token_bytes(16))
            host = u.hostname if u.port is None else "%s:%d" % (u.hostname, u.port)
            sock.sendall(b"GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         b"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
                         b"User-Agent: p3-infra-loadtest\r\n\r\n" % ((u.path or "/infra").encode(), host.encode(), key))
            while b"\r\n\r\n" not in self.rx:
                chunk = sock.recv(4096)
                if not chunk:
                    raise ConnectionError("no HTTP answer")
                self.rx += chunk
            end = self.rx.index(b"\r\n\r\n")
            status = bytes(self.rx[:end]).split(b" ")[1]
            del self.rx[:end + 4]
            if status != b"101":
                raise ConnectionError("HTTP %s" % status.decode())

    def send(self, kind, body):
        data = struct.pack("<IB", len(body) + 1, kind) + body
        if self.ws:
            mask = secrets.token_bytes(4)
            n = len(data)
            head = bytes([0x82]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
            data = head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data))
        with self.wlock:
            self.sock.sendall(data)

    def send_json(self, obj):
        self.send(1, json.dumps(obj, separators=(",", ":")).encode())

    def frames(self, timeout):
        """Frames that arrived within timeout: [(kind, body)]; raises ConnectionError when closed."""
        self.sock.settimeout(max(0.0005, timeout))
        try:
            chunk = self.sock.recv(65536)
        except (socket.timeout, ssl.SSLWantReadError):
            return []
        if not chunk:
            raise ConnectionError("closed by the server")
        self.rx += chunk
        out = []
        while True:
            if self.ws:
                if len(self.rx) < 2:
                    break
                op, n, off = self.rx[0] & 0x0F, self.rx[1] & 0x7F, 2
                if n == 126:
                    if len(self.rx) < 4:
                        break
                    n, off = struct.unpack_from(">H", self.rx, 2)[0], 4
                elif n == 127:
                    if len(self.rx) < 10:
                        break
                    n, off = struct.unpack_from(">Q", self.rx, 2)[0], 10
                if len(self.rx) < off + n:
                    break
                msg = bytes(self.rx[off:off + n])
                del self.rx[:off + n]
                if op == 8:
                    raise ConnectionError("WebSocket closed")
                if op != 2:
                    continue
                out.append((msg[4], msg[5:]))
            else:
                if len(self.rx) < 4:
                    break
                n = struct.unpack_from("<I", self.rx)[0]
                if len(self.rx) < 4 + n:
                    break
                out.append((self.rx[4], bytes(self.rx[5:4 + n])))
                del self.rx[:4 + n]
        return out

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Run:
    def __init__(self, server, clients, seconds, code):
        self.server, self.clients, self.seconds, self.code = server, clients, seconds, code
        self.lock = threading.Lock()
        self.vip = {}             # client index -> vip
        self.rooms = {}           # name -> room id
        self.members = {}         # room name -> [client index]
        self.logged_in = self.refused = 0
        self.errors = []
        self.sent = self.received = 0
        self.delay = Hist()
        self.ping = Hist()
        self.barrier = threading.Barrier(clients, timeout=120)

    def err(self, i, what):
        with self.lock:
            self.errors.append("client %d: %s" % (i, what))

    def plan(self, i):
        """(hideout room name, own index in group, VS room name or None, creates the VS room?)"""
        g, k = divmod(i, 4)
        vs = None
        if k in (0, 1):
            pair = g // 2
            partner = g ^ 1
            if partner * 4 + k < self.clients:
                vs = "vs%d.%d" % (pair, k)
        return "h%d" % g, k, vs, g % 2 == 0

    def client(self, i):
        c = None
        answers, next_id = {}, [0]

        def req(op, **kw):
            next_id[0] += 1
            rid = next_id[0]
            kw.update(op=op, id=rid)
            c.send_json(kw)
            deadline = time.monotonic() + 15
            while rid not in answers:
                if time.monotonic() > deadline:
                    raise TimeoutError("no answer to %s" % op)
                for kind, body in c.frames(0.2):
                    if kind == 1:
                        m = json.loads(body)
                        if "id" in m:
                            answers[m["id"]] = m
            return answers.pop(rid)

        def phase():
            try:
                self.barrier.wait()
            except threading.BrokenBarrierError:
                pass

        hide, k, vs, vs_owner = self.plan(i)
        ok = False
        try:
            c = Conn(self.server)
            login = {"online_id": "LT%03d" % i, "console": "02%010x" % (0xA0000 + i), "proto": 2}
            if self.code:
                login["invite"] = self.code
            a = req("login", **login)
            if a.get("ok"):
                ok = True
                with self.lock:
                    self.logged_in += 1
                    self.vip[i] = a["vip"]
                req("bound", port=GAME_PORT)
            else:
                with self.lock:
                    self.refused += 1
                self.err(i, "login refused 0x%08X" % a.get("error", 0))
        except Exception as e:  # noqa: BLE001 - every failure is a counted error, the run goes on
            self.err(i, "connect/login: %r" % e)
        phase()

        def create(name, ints):
            r = req("create", max_slot=4, flag_attr=0x04000000, ints=ints)
            if not r.get("ok"):
                raise RuntimeError("create %s: 0x%08X" % (name, r.get("error", 0)))
            with self.lock:
                self.rooms[name] = r["room"]["id"]
                self.members.setdefault(name, []).append(i)

        def join(name):
            rid = self.rooms.get(name)
            if rid is None:
                raise RuntimeError("no room %s to join" % name)
            r = req("join", room_id=rid)
            if not r.get("ok"):
                raise RuntimeError("join %s: 0x%08X" % (name, r.get("error", 0)))
            with self.lock:
                self.members.setdefault(name, []).append(i)

        steps = [lambda: k == 0 and create(hide, GAME_INTS),
                 lambda: k != 0 and join(hide),
                 lambda: vs and vs_owner and create(vs, VS_INTS),
                 lambda: vs and not vs_owner and join(vs)]
        for step in steps:
            if ok:
                try:
                    step()
                except Exception as e:  # noqa: BLE001
                    self.err(i, "setup: %r" % e)
            phase()
        if not ok:
            return
        with self.lock:
            peers = sorted({self.vip[j] for name, ms in self.members.items() if i in ms for j in ms if j != i})
        pad = b"\0" * (DGRAM_BYTES - 12)
        t0 = time.perf_counter()
        end = t0 + self.seconds
        next_send, next_ping, n = t0, t0 + PING_S * (i % 10) / 10, 0
        sent = received = 0
        try:
            while True:
                now = time.perf_counter()
                if now >= end:
                    break
                if now >= next_send:
                    for vip in peers:
                        c.send(2, struct.pack("<IHHId", vip, GAME_PORT, GAME_PORT, i, time.perf_counter()) + pad)
                        sent += 1
                    n += 1
                    next_send = t0 + n / DGRAM_HZ
                if now >= next_ping:
                    next_id[0] += 1
                    c.send_json({"op": "ping", "id": next_id[0], "t": time.perf_counter()})
                    next_ping += PING_S
                for kind, body in c.frames(min(next_send, next_ping, end) - time.perf_counter()):
                    t = time.perf_counter()
                    if kind == 2 and len(body) >= 20:
                        received += 1
                        self.delay.add((t - struct.unpack_from("<d", body, 12)[0]) * 1000)
                    elif kind == 1:
                        m = json.loads(body)
                        if "t" in m and m.get("ok") and isinstance(m["t"], float):
                            self.ping.add((t - m["t"]) * 1000)
                        elif m.get("ok") is False:
                            self.err(i, "request failed: %r" % m)
                        elif m.get("ev") == "signaling" and m.get("event") == 0x5101:
                            self.err(i, "signaling DEAD from member %s" % m.get("member_id"))
                        elif m.get("ev") in ("room_destroyed", "kicked"):
                            self.err(i, "event %s" % m["ev"])
            # drain what is still in flight
            deadline = time.perf_counter() + 1.5
            while time.perf_counter() < deadline:
                for kind, body in c.frames(0.1):
                    if kind == 2 and len(body) >= 20:
                        received += 1
                        self.delay.add((time.perf_counter() - struct.unpack_from("<d", body, 12)[0]) * 1000)
        except Exception as e:  # noqa: BLE001
            self.err(i, "traffic: %r" % e)
        finally:
            with self.lock:
                self.sent += sent
                self.received += received
            phase()
            c.close()

    def go(self, stagger=0.05):
        threads = []
        for i in range(self.clients):
            th = threading.Thread(target=self.client, args=(i,), daemon=True)
            th.start()
            threads.append(th)
            time.sleep(stagger)
        for th in threads:
            th.join(self.seconds + 300)
        kinds = {}
        for name in self.rooms:
            kinds[name[0]] = kinds.get(name[0], 0) + 1
        return {"server": self.server, "clients": self.clients, "seconds": self.seconds,
                "logged_in": self.logged_in, "refused": self.refused, "errors": len(self.errors),
                "first_errors": self.errors[:20], "rooms": {"hideout": kinds.get("h", 0), "vs": kinds.get("v", 0)},
                "dgrams_sent": self.sent, "dgrams_received": self.received,
                "delay_ms": self.delay.summary(), "ping_ms": self.ping.summary()}


def selftest():
    import importlib.util
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("infra_server", os.path.join(here, "infra_server.py"))
    infra = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(infra)
    logs = []
    srv = infra.Server("127.0.0.1", 0, 0, max_per_addr=64, logger=logs.append, stats_every=0)
    th = threading.Thread(target=srv.serve, daemon=True)
    th.start()
    try:
        for url in ("tcp://127.0.0.1:%d" % srv.port, "ws://127.0.0.1:%d/infra" % srv.ws_port):
            rep = Run(url, 8, 5, None).go(stagger=0.01)
            # loadtest_small
            assert rep["logged_in"] == 8 and rep["refused"] == 0, rep
            assert rep["errors"] == 0, rep["first_errors"]
            assert rep["rooms"] == {"hideout": 2, "vs": 2}, rep["rooms"]
            # 8 players: 6 of them in a hideout of 4 only (3 peers), the 4 VS players have one more peer
            want = 30 * 5 * (8 * 3 + 4)
            assert want * 0.9 <= rep["dgrams_sent"] <= want * 1.1, (rep["dgrams_sent"], want)
            assert rep["dgrams_received"] == rep["dgrams_sent"], (rep["dgrams_received"], rep["dgrams_sent"])
            assert rep["delay_ms"]["samples"] == rep["dgrams_received"] and rep["ping_ms"]["samples"] >= 8, rep
    finally:
        srv.running = False
        th.join(timeout=3)
    print("selftest OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--server", default="tcp://127.0.0.1:27320")
    ap.add_argument("--clients", type=int, default=50)
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--code", help="invite code for a gated server (a staff code: one code, many clients)")
    ap.add_argument("--report", help="write the report as JSON here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    t = time.time()
    rep = Run(args.server, args.clients, args.minutes * 60, args.code).go()
    rep["started"] = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(t))
    text = json.dumps(rep, indent=1)
    print(text)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0 if rep["errors"] == 0 and rep["refused"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
