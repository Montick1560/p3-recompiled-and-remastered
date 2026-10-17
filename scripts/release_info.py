#!/usr/bin/env python3
"""Write release.json for a game build: which disc this exe was built for.

Usage: python -I scripts/release_info.py <disc0> <id> <name> <out.json> [<saves.json>]
The launcher recognises a player's ISO by eboot_sha256, the SHA-256 of the (encrypted)
PSP_GAME/SYSDIR/EBOOT.BIN; disc_id comes from PSP_GAME/PARAM.SFO.
saves.json (optional): the game's save facts for the launcher, copied as "saves".
"""
import hashlib
import json
import os
import struct
import sys


def sfo_strings(data: bytes) -> dict:
    if data[:4] != b"\0PSF":
        raise ValueError("PARAM.SFO has no PSF header")
    kstart, dstart, count = struct.unpack_from("<III", data, 8)
    out = {}
    for i in range(count):
        koff, fmt, length, _max, doff = struct.unpack_from("<HHIII", data, 20 + i * 16)
        key = data[kstart + koff:data.index(b"\0", kstart + koff)].decode()
        if fmt in (0x0004, 0x0204):
            out[key] = data[dstart + doff:dstart + doff + length].rstrip(b"\0").decode("utf-8", "replace")
    return out


def main() -> int:
    if len(sys.argv) not in (5, 6):
        print(__doc__, file=sys.stderr)
        return 2
    disc0, rid, name, out = sys.argv[1:5]
    with open(os.path.join(disc0, "PSP_GAME", "SYSDIR", "EBOOT.BIN"), "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    with open(os.path.join(disc0, "PSP_GAME", "PARAM.SFO"), "rb") as f:
        sfo = sfo_strings(f.read())
    info = {"id": rid, "name": name, "disc_id": sfo.get("DISC_ID", ""), "eboot_sha256": sha}
    if len(sys.argv) == 6:
        with open(sys.argv[5], encoding="utf-8") as f:
            info["saves"] = json.load(f)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(info, f, indent=2)
        f.write("\n")
    print(f"release.json: {rid} ({info['disc_id']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
