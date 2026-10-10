# Mini MIPS disassembler for EBOOT/overlay ELF images (common ops only).
# Usage: python tools/mdis.py <hex start> <hex end> <elf>
"""mdis.py <hex start> <hex end> <elf>: tiny MIPS disassembler for EBOOT/overlay ELF images (common ops only)."""
import struct, sys

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def load(path):
    d = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', d, 0x1c)[0]
    phnum = struct.unpack_from('<H', d, 0x2c)[0]
    segs = [struct.unpack_from('<8I', d, phoff + i * 32) for i in range(phnum)]
    shoff = struct.unpack_from('<I', d, 0x20)[0]
    shnum = struct.unpack_from('<H', d, 0x30)[0]
    for i in range(shnum):  # overlay ELFs: OL_*.bin section at the window
        s = struct.unpack_from('<10I', d, shoff + i * 40)
        if s[3] == 0x08ABB180 and s[5] > 0:
            segs.append((1, s[4], s[3], s[3], s[5], s[5], 0, 0))
    def word(va):
        for t, off, v, pa, fs, ms, fl, al in segs:
            if t == 1 and v <= va < v + fs:
                return struct.unpack_from('<I', d, off + va - v)[0]
        return None
    return word


def dis(a, w):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xffff
    simm = imm - 0x10000 if imm & 0x8000 else imm
    tgt = ((w & 0x3ffffff) << 2) | (a & 0xF0000000)
    br = a + 4 + simm * 4
    if w == 0:
        return 'nop'
    if op == 0:
        names = {0x21: 'addu', 0x23: 'subu', 0x24: 'and', 0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2A: 'slt', 0x2B: 'sltu'}
        if fn in names:
            return f'{names[fn]} {R[rd]},{R[rs]},{R[rt]}'
        if fn == 0x08:
            return f'jr {R[rs]}'
        if fn == 0x09:
            return f'jalr {R[rd]},{R[rs]}'
        if fn in (0, 2, 3):
            return f'{["sll", "", "srl", "sra"][fn]} {R[rd]},{R[rt]},{sa}'
        return f'special.{fn:02x} {w:08X}'
    ops = {2: f'j {tgt:08X}', 3: f'jal {tgt:08X}', 4: f'beq {R[rs]},{R[rt]},{br:08X}', 5: f'bne {R[rs]},{R[rt]},{br:08X}',
           6: f'blez {R[rs]},{br:08X}', 7: f'bgtz {R[rs]},{br:08X}', 9: f'addiu {R[rt]},{R[rs]},{simm}',
           10: f'slti {R[rt]},{R[rs]},{simm}', 11: f'sltiu {R[rt]},{R[rs]},{simm}', 12: f'andi {R[rt]},{R[rs]},0x{imm:x}',
           13: f'ori {R[rt]},{R[rs]},0x{imm:x}', 15: f'lui {R[rt]},0x{imm:x}', 20: f'beql {R[rs]},{R[rt]},{br:08X}',
           21: f'bnel {R[rs]},{R[rt]},{br:08X}', 0x20: f'lb {R[rt]},{simm}({R[rs]})', 0x21: f'lh {R[rt]},{simm}({R[rs]})',
           0x23: f'lw {R[rt]},{simm}({R[rs]})', 0x24: f'lbu {R[rt]},{simm}({R[rs]})', 0x25: f'lhu {R[rt]},{simm}({R[rs]})',
           0x28: f'sb {R[rt]},{simm}({R[rs]})', 0x29: f'sh {R[rt]},{simm}({R[rs]})', 0x2B: f'sw {R[rt]},{simm}({R[rs]})',
           0x31: f'lwc1 f{rt},{simm}({R[rs]})', 0x39: f'swc1 f{rt},{simm}({R[rs]})'}
    if op in ops:
        return ops[op]
    if op == 1:
        return f'regimm.{rt} {R[rs]},{br:08X}'
    return f'op{op:02x} {w:08X}'


if __name__ == '__main__':
    if len(sys.argv) < 4:
        print('usage: python tools/mdis.py <hex start> <hex end> <elf>')
        sys.exit(1)
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    word = load(sys.argv[3])
    for a in range(lo, hi, 4):
        w = word(a)
        print(f'{a:08X} {w:08X}  {dis(a, w)}' if w is not None else f'{a:08X} --')
