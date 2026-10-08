#!/usr/bin/env python3
"""Extract every file of an ISO9660 image (e.g. a PSP UMD dump) to a directory.

Usage: python -I scripts/iso_extract.py <image.iso> <out_dir>
Pure standard library; no Joliet/Rock Ridge (PSP UMDs are plain ISO9660).
"""
import os
import struct
import sys

SECTOR = 2048


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    iso_path, out_dir = sys.argv[1], sys.argv[2]
    with open(iso_path, "rb") as iso:
        def read(lba: int, size: int) -> bytes:
            iso.seek(lba * SECTOR)
            return iso.read(size)

        pvd = read(16, SECTOR)
        if pvd[1:6] != b"CD001":
            print(f"error: {iso_path} is not an ISO9660 image", file=sys.stderr)
            return 1
        root = pvd[156:190]
        count = 0

        def walk(lba: int, size: int, rel: str) -> None:
            nonlocal count
            os.makedirs(os.path.join(out_dir, rel), exist_ok=True)
            data = read(lba, size)
            i = 0
            while i < len(data):
                length = data[i]
                if length == 0:  # records never straddle sectors; skip padding
                    i = (i // SECTOR + 1) * SECTOR
                    continue
                rec = data[i:i + length]
                i += length
                ext_lba = struct.unpack("<I", rec[2:6])[0]
                ext_size = struct.unpack("<I", rec[10:14])[0]
                name = rec[33:33 + rec[32]]
                if name in (b"\x00", b"\x01"):
                    continue
                name = name.decode("ascii").split(";")[0]
                if rec[25] & 2:
                    walk(ext_lba, ext_size, os.path.join(rel, name))
                    continue
                with open(os.path.join(out_dir, rel, name), "wb") as out:
                    iso.seek(ext_lba * SECTOR)
                    left = ext_size
                    while left:
                        chunk = iso.read(min(left, 1 << 22))
                        if not chunk:
                            raise IOError(f"truncated image while reading {rel}/{name}")
                        out.write(chunk)
                        left -= len(chunk)
                count += 1

        walk(struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0], "")
    print(f"extracted {count} files to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
