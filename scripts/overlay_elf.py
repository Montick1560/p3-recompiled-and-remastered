#!/usr/bin/env python3
"""Build a "combined" ELF: the main EXEC image with one code overlay loaded
into its fixed overlay window, so the regular analyze/recompile pipeline can
see the overlay's code exactly where the game runs it.

Usage: python -I scripts/overlay_elf.py <main.elf> <overlay.bin> <section> <out.elf>
  <section>  name of the (empty) overlay section in the main ELF, e.g. OL_Azito.bin

The main ELF declares each overlay as an empty SHT_PROGBITS section at the
window base plus a PT_NULL program header carrying the overlay's memsz, and
its first PT_LOAD's memsz spans the whole window as BSS. This tool:
  * appends the overlay file bytes to the end of the ELF,
  * points the named section at them (offset/size),
  * turns the PT_NULL header whose memsz matches into a PT_LOAD for the window,
  * trims the first PT_LOAD's memsz so it ends at the window base (no overlap).
"""
import struct
import sys

PT_NULL, PT_LOAD = 0, 1
PF_RWX = 7


def pick_placeholder(phdrs, load_addr, memsz):
    """Index of the PT_NULL placeholder for an overlay of `memsz` bytes at
    `load_addr`: the smallest one that fits (overlays share the window, so
    "first large enough" would hand a small overlay a larger one's slot).
    `phdrs` is a list of (p_type, p_vaddr, p_memsz). None if none fits."""
    best = None
    for i, (p_type, p_vaddr, p_memsz) in enumerate(phdrs):
        # 0x100 of slack for the linker's end-of-section alignment.
        if p_type == PT_NULL and p_vaddr == load_addr and p_memsz >= memsz - 0x100:
            if best is None or p_memsz < phdrs[best][2]:
                best = i
    return best


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    elf_path, ovl_path, section, out_path = sys.argv[1:]
    elf = bytearray(open(elf_path, "rb").read())
    ovl = open(ovl_path, "rb").read()
    if elf[:4] != b"\x7fELF" or ovl[:4] != b"MWo3":
        print("error: expected an ELF and an MWo3 overlay", file=sys.stderr)
        return 1

    load_addr, text_size, data_size, bss_size = struct.unpack("<IIII", ovl[8:24])
    phoff, shoff = struct.unpack("<II", elf[0x1C:0x24])
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack("<HHHHH", elf[0x2A:0x34])

    # Sections: find the overlay's placeholder by name.
    def sh(i):
        return list(struct.unpack("<10I", elf[shoff + i * shentsize:shoff + i * shentsize + 40]))

    def put_sh(i, v):
        elf[shoff + i * shentsize:shoff + i * shentsize + 40] = struct.pack("<10I", *v)

    strtab = sh(shstrndx)
    names = {}
    for i in range(shnum):
        n = sh(i)[0]
        s = strtab[4] + n
        names[elf[s:elf.index(b"\0", s)].decode()] = i
    if section not in names:
        print(f"error: section {section} not found", file=sys.stderr)
        return 1
    sec = sh(names[section])
    if sec[3] != load_addr:
        print(f"error: section addr 0x{sec[3]:08X} != overlay load 0x{load_addr:08X}", file=sys.stderr)
        return 1

    # Append the overlay (16-byte aligned) and point the section at it.
    while len(elf) % 16:
        elf.append(0)
    ovl_off = len(elf)
    elf += ovl
    sec[4], sec[5] = ovl_off, len(ovl)
    put_sh(names[section], sec)

    # Program headers: trim the main PT_LOAD, promote this overlay's PT_NULL.
    memsz = len(ovl) + bss_size
    headers = [list(struct.unpack("<8I", elf[phoff + i * phentsize:phoff + i * phentsize + 32]))
               for i in range(phnum)]
    slot = pick_placeholder([(p[0], p[2], p[5]) for p in headers], load_addr, memsz)
    promoted = slot is not None
    trimmed = False
    for i, p in enumerate(headers):
        p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = p
        if p_type == PT_LOAD and p_vaddr < load_addr < p_vaddr + p_memsz and not trimmed:
            p[5] = load_addr - p_vaddr
            trimmed = True
        elif i == slot:
            p = [PT_LOAD, ovl_off, load_addr, load_addr, len(ovl), max(p_memsz, memsz), PF_RWX, 16]
        o = phoff + i * phentsize
        elf[o:o + 32] = struct.pack("<8I", *p)
    if not (promoted and trimmed):
        print(f"error: could not rewrite program headers (promoted={promoted}, trimmed={trimmed})",
              file=sys.stderr)
        return 1

    open(out_path, "wb").write(elf)
    print(f"{out_path}: {section} {len(ovl)} bytes at 0x{load_addr:08X} "
          f"(text 0x{text_size:X}, data 0x{data_size:X}, bss 0x{bss_size:X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
