"""tgadiff.py <a.tga> <b.tga>: count differing pixels between two uncompressed TGAs
(as written by the runtime's `S` screenshot). Exit 2 if the sizes differ."""
import struct
import sys


def load(path):
    d = open(path, 'rb').read()
    idl, bpp = d[0], d[16]
    w, h = struct.unpack('<HH', d[12:16])
    n = w * h * (bpp // 8)
    return w, h, bpp // 8, d[18 + idl:18 + idl + n]


def main():
    wa, ha, ba, pa = load(sys.argv[1])
    wb, hb, bb, pb = load(sys.argv[2])
    if (wa, ha, ba) != (wb, hb, bb):
        print(f'size differs: {wa}x{ha}x{ba} vs {wb}x{hb}x{bb}')
        sys.exit(2)
    diff = sum(1 for i in range(0, len(pa), ba) if pa[i:i + ba] != pb[i:i + ba])
    print(f'differing pixels: {diff} of {wa * ha}')


if __name__ == '__main__':
    main()
