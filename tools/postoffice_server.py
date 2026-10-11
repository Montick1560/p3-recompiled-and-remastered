"""Minimal aemu_postoffice relay server (PPSSPP "server-provided packet relay") for local tests.

Ad hoc PDP datagrams and PTP streams of every client go through this server (TCP 27313)
instead of peer to peer. Written from the protocol description in
docs/superpowers/plans/2026-10-11-adhoc-relay.md. One TCP connection per PSP socket; the first
24 bytes are the init packet (type, src mac[8], sport, dst mac[8], dport; little endian).

    python tools/postoffice_server.py [--host 0.0.0.0] [--port 27313]
    python tools/postoffice_server.py --selftest
"""
import argparse
import selectors
import socket
import struct
import sys
import threading
import time

PDP, LISTEN, CONNECT, ACCEPT = 0, 1, 2, 3
INIT_SIZE, PDP_HEADER, PTP_HEADER = 24, 14, 4
PDP_MAX, PTP_MAX = 10 * 1024, 50 * 1024
CONNECT_RETRIES, CONNECT_RETRY_S, ACCEPT_WAIT_S = 8, 0.25, 20.0


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def mac_str(mac):
    return ":".join("%02x" % b for b in mac)


def name_str(name):
    return " ".join(mac_str(x) if isinstance(x, bytes) else str(x) for x in name)


def addr_packet(mac, port):
    """mac[8] (6 used) + u16 port: a PTP connect request or ack."""
    return mac + b"\0\0" + struct.pack("<H", port)


class Session:
    def __init__(self, sock, addr):
        self.sock, self.addr = sock, addr
        self.rx = bytearray()
        self.kind = None      # PDP, LISTEN, CONNECT, ACCEPT once the init arrived
        self.name = None
        self.src = self.dst = b""
        self.sport = self.dport = 0
        self.peer = None      # paired PTP session
        self.data = False     # PTP data mode
        self.retries = 0
        self.retry_at = None  # CONNECT waiting for a listener
        self.wait_until = None  # CONNECT waiting for the accept

    def label(self):
        return name_str(self.name) if self.name else "%s:%d" % self.addr


class Server:
    def __init__(self, host="0.0.0.0", port=27313):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind((host, port))
        self.listener.listen(64)
        self.port = self.listener.getsockname()[1]
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.listener, selectors.EVENT_READ)
        self.sessions = {}  # sock -> Session
        self.named = {}     # name -> Session
        self.running = True

    # -- session table --
    def close(self, s, why):
        if s.sock not in self.sessions:
            return
        del self.sessions[s.sock]
        if s.name and self.named.get(s.name) is s:
            del self.named[s.name]
        try:
            self.sel.unregister(s.sock)
        except (KeyError, ValueError):
            pass
        s.sock.close()
        log("closed %s: %s" % (s.label(), why))
        if s.peer is not None:
            peer, s.peer, peer.peer = s.peer, None, None
            self.close(peer, "peer closed")

    def register(self, s, name):
        old = self.named.get(name)
        if old is not None and old is not s:
            self.close(old, "replaced")
        s.name = name
        self.named[name] = s
        log("session %s" % name_str(name))

    def send(self, s, data):
        try:
            s.sock.setblocking(True)
            s.sock.sendall(data)
            s.sock.setblocking(False)
        except OSError as e:
            self.close(s, "send failed: %s" % e)

    # -- protocol --
    def on_init(self, s, pkt):
        kind, = struct.unpack_from("<i", pkt, 0)
        s.src, s.sport = bytes(pkt[4:10]), struct.unpack_from("<H", pkt, 12)[0]
        s.dst, s.dport = bytes(pkt[14:20]), struct.unpack_from("<H", pkt, 22)[0]
        s.kind = kind
        if kind == PDP:
            self.register(s, ("PDP", s.src, s.sport))
        elif kind == LISTEN:
            self.register(s, ("LISTEN", s.src, s.sport))
        elif kind == CONNECT:
            self.try_connect(s)
        elif kind == ACCEPT:
            want = ("CONNECT", s.dst, s.dport, s.src, s.sport)
            other = self.named.get(want)
            if other is None or other.wait_until is None:
                return self.close(s, "no pending connect %s" % name_str(want))
            self.register(s, ("ACCEPT", s.src, s.sport, s.dst, s.dport))
            other.wait_until = None
            s.peer, other.peer = other, s
            s.data = other.data = True
            self.send(other, addr_packet(s.src, s.sport))
            self.send(s, addr_packet(s.dst, s.dport))
        else:
            self.close(s, "bad init type %d" % kind)

    def try_connect(self, s):
        listener = self.named.get(("LISTEN", s.dst, s.dport))
        if listener is None:
            if s.retries < CONNECT_RETRIES:
                s.retries += 1
                s.retry_at = time.monotonic() + CONNECT_RETRY_S
                return
            return self.close(s, "no listener %s port %d" % (mac_str(s.dst), s.dport))
        s.retry_at = None
        self.register(s, ("CONNECT", s.src, s.sport, s.dst, s.dport))
        s.wait_until = time.monotonic() + ACCEPT_WAIT_S
        self.send(listener, addr_packet(s.src, s.sport))

    def on_data(self, s):
        while s.sock in self.sessions:
            if s.kind is None:
                if len(s.rx) < INIT_SIZE:
                    return
                pkt, s.rx = s.rx[:INIT_SIZE], s.rx[INIT_SIZE:]
                self.on_init(s, pkt)
                continue
            if s.kind == PDP:
                if len(s.rx) < PDP_HEADER:
                    return
                size, = struct.unpack_from("<I", s.rx, 10)
                if size > PDP_MAX:
                    return self.close(s, "PDP datagram of %d bytes" % size)
                if len(s.rx) < PDP_HEADER + size:
                    return
                dst, dport = bytes(s.rx[0:6]), struct.unpack_from("<H", s.rx, 8)[0]
                payload = bytes(s.rx[PDP_HEADER:PDP_HEADER + size])
                del s.rx[:PDP_HEADER + size]
                target = self.named.get(("PDP", dst, dport))
                if target is not None:
                    self.send(target, s.src + b"\0\0" + struct.pack("<HI", s.sport, size) + payload)
                continue
            if s.data and s.peer is not None:
                if len(s.rx) < PTP_HEADER:
                    return
                size, = struct.unpack_from("<I", s.rx, 0)
                if size > PTP_MAX:
                    return self.close(s, "PTP frame of %d bytes" % size)
                if len(s.rx) < PTP_HEADER + size:
                    return
                frame = bytes(s.rx[:PTP_HEADER + size])
                del s.rx[:PTP_HEADER + size]
                self.send(s.peer, frame)
                continue
            return  # LISTEN / waiting CONNECT: nothing is expected from the client

    def tick(self):
        now = time.monotonic()
        for s in list(self.sessions.values()):
            if s.retry_at is not None and now >= s.retry_at:
                self.try_connect(s)
            elif s.wait_until is not None and now >= s.wait_until:
                self.close(s, "connect not accepted in %d s" % ACCEPT_WAIT_S)

    def serve(self):
        while self.running:
            for key, _ in self.sel.select(timeout=0.05):
                if key.fileobj is self.listener:
                    sock, addr = self.listener.accept()
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    sock.setblocking(False)
                    self.sessions[sock] = Session(sock, addr)
                    self.sel.register(sock, selectors.EVENT_READ)
                    continue
                s = self.sessions.get(key.fileobj)
                if s is None:
                    continue
                try:
                    data = s.sock.recv(65536)
                except BlockingIOError:
                    continue
                except OSError:
                    data = b""
                if not data:
                    self.close(s, "connection closed")
                    continue
                s.rx += data
                self.on_data(s)
            self.tick()
        for s in list(self.sessions.values()):
            self.close(s, "server stopping")
        self.sel.close()
        self.listener.close()


def selftest():
    srv = Server("127.0.0.1", 0)
    try:
        Server("127.0.0.1", srv.port)
        raise AssertionError("a second server bound the same port")
    except OSError:
        pass
    th = threading.Thread(target=srv.serve, daemon=True)
    th.start()
    A, B = bytes([2, 0, 0, 0, 0, 0x0A]), bytes([2, 0, 0, 0, 0, 0x0B])

    def conn(kind, src, sport, dst=b"\0" * 6, dport=0):
        c = socket.create_connection(("127.0.0.1", srv.port), timeout=5)
        c.sendall(struct.pack("<i", kind) + src + b"\0\0" + struct.pack("<H", sport) +
                  dst + b"\0\0" + struct.pack("<H", dport))
        return c

    def read(c, n):
        b = b""
        while len(b) < n:
            chunk = c.recv(n - len(b))
            assert chunk, "connection closed"
            b += chunk
        return b

    # PDP: B -> A arrives with B's address; to an unknown port it is dropped.
    pa, pb = conn(PDP, A, 13000), conn(PDP, B, 13000)
    time.sleep(0.2)
    pb.sendall(A + b"\0\0" + struct.pack("<HI", 13001, 2) + b"xx")  # nobody on 13001
    pb.sendall(A + b"\0\0" + struct.pack("<HI", 13000, 3) + b"abc")
    hdr = read(pa, PDP_HEADER)
    assert hdr[:6] == B and struct.unpack_from("<HI", hdr, 8) == (13000, 3), hdr
    assert read(pa, 3) == b"abc"
    # PTP: A listens, B connects, A accepts, data both ways, close propagates.
    la = conn(LISTEN, A, 13100)
    time.sleep(0.2)
    cb = conn(CONNECT, B, 13200, A, 13100)
    req = read(la, 10)
    assert req[:6] == B and struct.unpack_from("<H", req, 8)[0] == 13200, req
    aa = conn(ACCEPT, A, 13100, B, 13200)
    ack_a, ack_b = read(aa, 10), read(cb, 10)
    assert ack_a[:6] == B and struct.unpack_from("<H", ack_a, 8)[0] == 13200, ack_a
    assert ack_b[:6] == A and struct.unpack_from("<H", ack_b, 8)[0] == 13100, ack_b
    cb.sendall(struct.pack("<I", 4) + b"ping")
    assert read(aa, 8) == struct.pack("<I", 4) + b"ping"
    aa.sendall(struct.pack("<I", 4) + b"pong")
    assert read(cb, 8) == struct.pack("<I", 4) + b"pong"
    aa.close()
    cb.settimeout(5)
    assert cb.recv(1) == b"", "connect side closed with its peer"
    # A connect to nobody is closed after the retries (~2 s).
    lone = conn(CONNECT, B, 13300, A, 13999)
    lone.settimeout(5)
    assert lone.recv(1) == b""
    for c in (pa, pb, la, lone):
        c.close()
    srv.running = False
    th.join(timeout=2)
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=27313)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    srv = Server(args.host, args.port)
    log("postoffice relay listening on %s:%d" % (args.host, srv.port))
    try:
        srv.serve()
    except KeyboardInterrupt:
        srv.running = False


if __name__ == "__main__":
    sys.exit(main())
