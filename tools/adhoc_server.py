#!/usr/bin/env python3
"""Minimal PRO ad hoc server for tests (PPSSPP Core/HLE/proAdhocServer.cpp subset).

Speaks the protocol of PPSSPP's built-in ad hoc server: TCP, packed packets,
opcodes PING 0, LOGIN 1, CONNECT 2, DISCONNECT 3, SCAN 4, SCAN_COMPLETE 5,
CONNECT_BSSID 6, CHAT 7. Enough to put two players of the same game in a
group and tell each about the other; no database, no status page.

    python -I tools/adhoc_server.py [--port 27312] [--host 0.0.0.0]
    python -I tools/adhoc_server.py --selftest
"""
import argparse
import selectors
import socket
import struct
import sys
import threading
import time

PING, LOGIN, CONNECT, DISCONNECT, SCAN, SCAN_COMPLETE, CONNECT_BSSID, CHAT = range(8)
C2S_SIZE = {PING: 1, LOGIN: 1 + 6 + 128 + 9, CONNECT: 1 + 8, DISCONNECT: 1, SCAN: 1, CHAT: 1 + 64}
USER_TIMEOUT = 15.0  # seconds without any packet (clients ping every 2 s)

# Region crosslinks from PPSSPP's server, so every Patapon 3 copy meets in one lobby.
CROSSLINKS = {"UCES01421": "UCUS98751", "NPJG00122": "UCUS98751"}


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def fixed(data):
    return data.split(b"\0", 1)[0].decode("latin-1")


def mac_str(mac):
    return ":".join("%02x" % b for b in mac)


class User:
    def __init__(self, sock, ip):
        self.sock = sock
        self.ip = ip  # 4 wire bytes
        self.rx = b""
        self.mac = None
        self.name = b""
        self.game = None
        self.group = None  # group key (game, name)
        self.last = time.monotonic()

    def label(self):
        return "%s(%s %s)" % (fixed(self.name) or "?", mac_str(self.mac or b"\0" * 6), socket.inet_ntoa(self.ip))


class Server:
    def __init__(self, host="0.0.0.0", port=27312):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind((host, port))
        self.listener.listen(64)
        self.port = self.listener.getsockname()[1]
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.listener, selectors.EVENT_READ)
        self.users = {}   # sock -> User
        self.groups = {}  # (game, group name bytes) -> [User], newest first like PPSSPP
        self.running = True

    def send(self, user, data):
        try:
            user.sock.sendall(data)
        except OSError as e:
            log("send to %s failed: %s" % (user.label(), e))

    def logout(self, user, why):
        if user.group is not None:
            self.leave(user)
        self.sel.unregister(user.sock)
        user.sock.close()
        self.users.pop(user.sock, None)
        log("logout %s: %s" % (user.label(), why))

    def leave(self, user):
        members = self.groups[user.group]
        members.remove(user)
        for peer in members:
            self.send(peer, bytes([DISCONNECT]) + user.ip)
        log("%s left group %s" % (user.label(), user.group[1].rstrip(b"\0").decode("latin-1")))
        if not members:
            del self.groups[user.group]
        user.group = None

    def on_login(self, user, pkt):
        mac, name, game = pkt[1:7], pkt[7:135], pkt[135:144]
        valid = all(48 <= c <= 57 or 65 <= c <= 90 for c in game)
        if not valid or mac in (b"\0" * 6, b"\xff" * 6) or name[0] == 0:
            return self.logout(user, "invalid login")
        game = game.decode()
        user.mac, user.name, user.game = mac, name, CROSSLINKS.get(game, game)
        log("login %s playing %s" % (user.label(), user.game))

    def on_connect(self, user, pkt):
        name = pkt[1:9]
        text = name.split(b"\0", 1)[0]
        if not all(chr(c).isascii() and chr(c).isalnum() for c in text):
            return self.logout(user, "invalid group name")
        if user.group is not None:
            return self.logout(user, "connect while in a group")
        key = (user.game, name)
        members = self.groups.setdefault(key, [])
        bssid = user.mac
        for peer in members:
            self.send(peer, bytes([CONNECT]) + user.name + user.mac + user.ip)
            self.send(user, bytes([CONNECT]) + peer.name + peer.mac + peer.ip)
        if members:
            bssid = members[-1].mac  # the founder (oldest member)
        members.insert(0, user)
        user.group = key
        self.send(user, bytes([CONNECT_BSSID]) + bssid)
        log("%s joined group %s (%d members)" % (user.label(), text.decode("latin-1"), len(members)))

    def on_scan(self, user):
        if user.group is not None:
            return self.logout(user, "scan while in a group")
        for (game, name), members in self.groups.items():
            if game == user.game and members:
                self.send(user, bytes([SCAN]) + name + members[-1].mac)
        self.send(user, bytes([SCAN_COMPLETE]))

    def on_chat(self, user, pkt):
        if user.group is None:
            return
        for peer in self.groups[user.group]:
            if peer is not user:
                self.send(peer, pkt[:65] + user.name)

    def handle(self, user):
        while user.rx:
            op = user.rx[0]
            size = C2S_SIZE.get(op)
            if size is None:
                return self.logout(user, "unknown opcode %d" % op)
            if len(user.rx) < size:
                return
            pkt, user.rx = user.rx[:size], user.rx[size:]
            if op != LOGIN and user.mac is None:
                return self.logout(user, "packet before login")
            if op == LOGIN:
                self.on_login(user, pkt)
            elif op == CONNECT:
                self.on_connect(user, pkt)
            elif op == DISCONNECT:
                if user.group is None:
                    return self.logout(user, "disconnect outside a group")
                self.leave(user)
            elif op == SCAN:
                self.on_scan(user)
            elif op == CHAT:
                self.on_chat(user, pkt)
            if user.sock not in self.users:
                return

    def serve(self):
        while self.running:
            for key, _ in self.sel.select(timeout=0.2):
                if key.fileobj is self.listener:
                    sock, addr = self.listener.accept()
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    user = User(sock, socket.inet_aton(addr[0]))
                    self.users[sock] = user
                    self.sel.register(sock, selectors.EVENT_READ)
                    continue
                user = self.users.get(key.fileobj)
                if user is None:
                    continue
                try:
                    data = user.sock.recv(4096)
                except OSError:
                    data = b""
                if not data:
                    self.logout(user, "connection closed")
                    continue
                user.last = time.monotonic()
                user.rx += data
                self.handle(user)
            now = time.monotonic()
            for user in list(self.users.values()):
                if now - user.last > USER_TIMEOUT:
                    self.logout(user, "timed out")
        for user in list(self.users.values()):
            self.logout(user, "server stopping")
        self.sel.close()
        self.listener.close()


def selftest():
    srv = Server("127.0.0.1", 0)
    th = threading.Thread(target=srv.serve, daemon=True)
    th.start()

    def client():
        s = socket.create_connection(("127.0.0.1", srv.port), timeout=3)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return s

    def recv_exact(s, n):
        buf = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            assert chunk, "server closed the connection"
            buf += chunk
        return buf

    def login(mac, nick, game):
        return bytes([LOGIN]) + mac + nick.ljust(128, b"\0") + game

    mac_a, mac_b = bytes.fromhex("00000000000a"), bytes.fromhex("00000000000b")
    nick_a, nick_b = b"A".ljust(128, b"\0"), b"B".ljust(128, b"\0")
    ip = socket.inet_aton("127.0.0.1")
    a, b = client(), client()
    a.sendall(login(mac_a, b"A", b"UCES01421"))
    b.sendall(login(mac_b, b"B", b"UCUS98751"))  # other region, same lobby
    time.sleep(0.2)
    a.sendall(bytes([CONNECT]) + b"G".ljust(8, b"\0"))
    assert recv_exact(a, 7) == bytes([CONNECT_BSSID]) + mac_a, "founder gets its own BSSID"
    b.sendall(bytes([SCAN]))
    assert recv_exact(b, 15) == bytes([SCAN]) + b"G".ljust(8, b"\0") + mac_a, "scan lists the group"
    assert recv_exact(b, 1) == bytes([SCAN_COMPLETE])
    b.sendall(bytes([CONNECT]) + b"G".ljust(8, b"\0"))
    assert recv_exact(a, 139) == bytes([CONNECT]) + nick_b + mac_b + ip, "A told about B"
    assert recv_exact(b, 139) == bytes([CONNECT]) + nick_a + mac_a + ip, "B told about A"
    assert recv_exact(b, 7) == bytes([CONNECT_BSSID]) + mac_a, "B gets the founder's BSSID"
    b.sendall(bytes([CHAT]) + b"hola".ljust(64, b"\0"))
    assert recv_exact(a, 193) == bytes([CHAT]) + b"hola".ljust(64, b"\0") + nick_b, "chat relayed"
    b.close()
    assert recv_exact(a, 5) == bytes([DISCONNECT]) + ip, "A told B left"
    a.sendall(bytes([DISCONNECT]))
    time.sleep(0.2)
    assert not srv.groups, "empty group removed"
    a.close()
    srv.running = False
    th.join(timeout=2)
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=27312)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    srv = Server(args.host, args.port)
    log("PRO ad hoc server listening on %s:%d" % (args.host, srv.port))
    try:
        srv.serve()
    except KeyboardInterrupt:
        srv.running = False


if __name__ == "__main__":
    sys.exit(main())
