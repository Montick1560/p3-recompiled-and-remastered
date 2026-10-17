# PPSSPP PRX decrypter (vendored)

`Core/ELF/PrxDecrypter.{h,cpp}` and `ext/libkirk/` are copied unmodified from PPSSPP
(https://github.com/hrydgard/ppsspp, commit 0665ea0febd20954d194797130614eabacd9e720); the
original headers are kept in every file. `shim/Common/` replaces the four PPSSPP `Common/` headers
these files include (types, little-endian typedefs, no-op logging).

Licences (from the file headers and PPSSPP's licence):
- `Core/ELF/PrxDecrypter.*`: PPSSPP, GPL-2.0-or-later.
- `ext/libkirk/kirk_engine.*` (Draan): GPL-3.0-or-later.
- `ext/libkirk/bn.c`, `ec.c` (Segher Boessenkool): GPL version 2 or version 3.
- `ext/libkirk/AES.*`: public domain (OpenBSD rijndael); `SHA1.*`, `amctrl.*`: no licence header.

psprecomp itself is GPL-2.0-or-later, so it can be combined with these files; an executable that
links this library (`kirk`) is distributed under GPL-3.0.

psprecomp uses only `pspDecryptPRX`, to decrypt a game's `~PSP` boot file in memory at boot
(`runtime/src/psp_eboot_loader.cpp`, `[image] source = "disc"`). Nothing decrypted is written
to disk.
