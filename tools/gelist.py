"""gelist.py rt|pp <list_hex> [port] : walk a GE display list in live memory and print its PRIMs.

rt = this runtime's debug socket (TCP 9999, R command); pp = PPSSPP WebSocket debugger
(memory.read; port = PPSSPP's web server port). Follows CALL/JUMP/RET, BASE, OFFSETADDR.
One line per PRIM: index, type, count, vtype, vaddr, tex addr/size/format, clut, material.
"""
import base64, json, os, socket, struct, sys

class RtMem:
    def __init__(self): self.cache = {}
    def page(self, p):
        if p not in self.cache:
            s = socket.create_connection(('127.0.0.1', 9999), timeout=10)
            s.sendall(f'R {p:08X} 4096\n'.encode()); f = s.makefile('rb'); f.readline()
            self.cache[p] = f.read(4096); s.close()
        return self.cache[p]
    def u32(self, a):
        a &= 0x0FFFFFFF; pg = self.page(a & ~0xFFF); return struct.unpack_from('<I', pg, a & 0xFFF)[0]

class PpMem(RtMem):
    def __init__(self, port):
        super().__init__(); self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f'GET /debugger HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
                        f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n'
                        'Sec-WebSocket-Protocol: debugger.ppsspp.org\r\n\r\n').encode())
        hdr = b''
        while b'\r\n\r\n' not in hdr: hdr += self.s.recv(1)
        if b' 101 ' not in hdr.split(b'\r\n')[0]: raise SystemExit('handshake failed: %r' % hdr[:200])
    def send(self, obj):
        data = json.dumps(obj).encode(); mask = os.urandom(4)
        h = bytes([0x81]) + (bytes([0x80 | len(data)]) if len(data) < 126 else bytes([0x80 | 126]) + struct.pack('>H', len(data)))
        self.s.sendall(h + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
    def recv_exact(self, n):
        b = b''
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c: raise SystemExit('ws closed')
            b += c
        return b
    def recv(self, want=('memory.read', 'error')):
        buf = b''
        while True:
            b0, b1 = self.recv_exact(2); n = b1 & 0x7F
            if n == 126: n = struct.unpack('>H', self.recv_exact(2))[0]
            elif n == 127: n = struct.unpack('>Q', self.recv_exact(8))[0]
            buf += self.recv_exact(n)
            if not b0 & 0x80: continue  # fragmented: wait for the FIN frame
            op = b0 & 0x0F
            if op in (0, 1) and buf:
                msg = json.loads(buf)
                if msg.get('event') in want: return msg
            buf = b''
    def call(self, obj, want):
        self.send(obj); return self.recv((want, 'error'))
    def find_list(self, stub=0x08A51D60):
        """Breakpoint on the sceGeListEnQueue import stub, read a0, resume."""
        self.call({'event': 'cpu.breakpoint.add', 'address': stub, 'enabled': True}, 'cpu.breakpoint.add')
        self.recv(('cpu.stepping',))
        a0 = self.call({'event': 'cpu.getReg', 'name': 'a0'}, 'cpu.getReg')['uintValue']
        self.call({'event': 'cpu.breakpoint.remove', 'address': stub}, 'cpu.breakpoint.remove')
        self.call({'event': 'cpu.resume'}, 'cpu.resume')
        return a0
    def page(self, p):
        if p not in self.cache:
            self.send({'event': 'memory.read', 'address': p | 0x08000000 if p < 0x08000000 and p >= 0x04800000 else p, 'size': 4096})
            m = self.recv()
            if m.get('event') == 'error': raise SystemExit('ppsspp error: %s' % m)
            self.cache[p] = base64.b64decode(m['base64'])
        return self.cache[p]

def walk(mem, start, max_cmds=200000):
    pc = start & 0x0FFFFFFF; base = 0; offset = 0; stack = []; st = {}; prims = []
    def rel(d): return ((((base & 0x0F0000) << 8) | (d & 0xFFFFFF)) + offset) & 0x0FFFFFFF
    for _ in range(max_cmds):
        w = mem.u32(pc); cmd = w >> 24; d = w & 0xFFFFFF; cpc = pc; pc += 4
        if cmd == 0x10: base = d
        elif cmd == 0x13: offset = d << 8
        elif cmd == 0x01: st['va'] = rel(d)
        elif cmd == 0x02: st['ia'] = rel(d)
        elif cmd == 0x08: pc = rel(d & ~3)
        elif cmd == 0x0A: stack.append((pc, offset)); pc = rel(d & ~3)
        elif cmd == 0x0B:
            if not stack: break
            pc, offset = stack.pop()
        elif cmd == 0x0C: break
        elif cmd == 0x04:
            ta = (st.get(0xA0, 0) & 0xFFFFF0) | ((st.get(0xA8, 0) << 8) & 0x0F000000)
            prims.append(('pc=%08X call=%s ' % (cpc, '>'.join('%08X' % (r - 4) for r, _ in stack[-3:])) if os.environ.get('GELIST_PC') else '') + '%4d p=%d n=%d vt=%06X va=%08X thr=%d tex=%d ta=%08X tsz=%04X tf=%d clut=%06X mat=%06X/%02X fb=%06X clr=%d blend=%d bm=%03X'
                         % (len(prims), (d >> 16) & 7, d & 0xFFFF, st.get(0x12, 0), st.get('va', 0), (st.get(0x12, 0) >> 23) & 1,
                            st.get(0x1E, 0) & 1, ta, st.get(0xB8, 0), st.get(0xC3, 0) & 0xF, st.get(0xB0, 0), st.get(0x55, 0),
                            st.get(0x58, 0) & 0xFF, st.get(0x9C, 0), st.get(0xD3, 0) & 1, st.get(0x21, 0) & 1, st.get(0xDF, 0)))
            vt = st.get(0x12, 0); n = d & 0xFFFF
            # advance vertex address like the GE (stride from vtype)
            st['va'] = st.get('va', 0) + n * vstride(vt)
        else: st[cmd] = d
    return prims

def vstride(vt):
    tc = vt & 3; col = (vt >> 2) & 7; nrm = (vt >> 5) & 3; pos = (vt >> 7) & 3; wt = (vt >> 9) & 3; idx = (vt >> 11) & 3
    nw = ((vt >> 14) & 7) + 1; morph = ((vt >> 18) & 7) + 1
    sz = {0: 0, 1: 1, 2: 2, 3: 4}; csz = {0: 0, 4: 2, 5: 2, 6: 2, 7: 4}
    off = 0; al = 1
    def add(n, a):
        nonlocal off, al
        if n == 0: return
        off = (off + a - 1) // a * a; off += n; al = max(al, a)
    add(sz[wt] * nw if wt else 0, sz[wt] if wt else 1)
    add(sz[tc] * 2 if tc else 0, sz[tc] if tc else 1)
    add(csz.get(col, 0), csz.get(col, 1) or 1)
    add(sz[nrm] * 3 if nrm else 0, sz[nrm] if nrm else 1)
    add(sz[pos] * 3 if pos else 0, sz[pos] if pos else 1)
    off = (off + al - 1) // al * al
    return off * morph

if __name__ == '__main__':
    mem = RtMem() if sys.argv[1] == 'rt' else PpMem(int(sys.argv[3]))
    start = int(sys.argv[2], 16)
    if start == 0 and sys.argv[1] == 'pp':  # 0 = find the next enqueued list
        start = mem.find_list(); print('# list 0x%08X' % start); mem.cache.clear()
    for line in walk(mem, start): print(line)
