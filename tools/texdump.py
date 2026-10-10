# texdump.py <out.png> <tex_hex> <w> <h> <bufw> <clut_hex> <fmt 4|5> <swz 0|1>
# Reads a CLUT4/CLUT8 texture + 8888 CLUT from the live game (debug socket R) -> PNG (RGBA).
import socket, struct, sys, zlib
out, ta, w, h, bufw, ca, fmt, swz = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6], 16), int(sys.argv[7]), int(sys.argv[8])
def rd(a, n):
    buf = b''
    while len(buf) < n:
        k = min(0x8000, n - len(buf))
        s = socket.create_connection(('127.0.0.1', 9999), timeout=10); s.sendall(f'R {a + len(buf):08X} {k}\n'.encode())
        f = s.makefile('rb'); f.readline(); d = f.read(k); s.close(); buf += d
    return buf
bpp = 4 if fmt == 4 else 8
pitch = bufw * bpp // 8
raw = rd(ta, pitch * h)
if swz:  # PSP swizzle: 16-byte x 8-row blocks
    bw = pitch // 16; un = bytearray(len(raw)); i = 0
    for by in range(h // 8):
        for bx in range(bw):
            for r in range(8):
                o = (by * 8 + r) * pitch + bx * 16
                un[o:o + 16] = raw[i:i + 16]; i += 16
    raw = bytes(un)
ncl = 16 if fmt == 4 else 256
clut = struct.unpack('<%dI' % ncl, rd(ca, ncl * 4))
rows = []
for y in range(h):
    row = bytearray(b'\x00')
    for x in range(w):
        if fmt == 4:
            b = raw[y * pitch + x // 2]; idx = (b >> 4) if x & 1 else (b & 15)
        else:
            idx = raw[y * pitch + x]
        c = clut[idx]; row += bytes([c & 255, (c >> 8) & 255, (c >> 16) & 255, (c >> 24) & 255])
    rows.append(bytes(row))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
open(out, 'wb').write(png); print('ok', out, 'clut', ' '.join('%08X' % c for c in clut[:16]))
