# M3c — HD texture replacement (PPSSPP-format packs): design

Date: 2026-10-10. Status: approved in conversation with the user ("adelante con el plan … tienes
total autonomía"). Roadmap source: `docs/superpowers/plans/2026-10-09-m3-graphics-rtt.md`,
"Roadmap after M3a", item 4. Follows M3b (internal render scale, `PSPRECOMP_RENDER_SCALE`).

## Goal

Load an existing PPSSPP texture-replacement pack and draw its PNGs instead of the textures the game
decodes from guest RAM, so the port shows the community HD art at render scale 4. Target pack:
<https://github.com/Lin-zl522/Patapon-3-HD-Texture-Pack> (lists the DxD build `INFN00001`; about 99%
coverage, more than 70% hand-remastered). The user keeps it at
`DECO/PPSSPP/memstick/PSP/TEXTURES/INFN00001/`, the same folder their PPSSPP (our oracle) reads.

## Constraints

- **Never vendor the pack.** Its licence is not stated. The runtime reads it from a folder given by
  `PSPRECOMP_TEXTURES=<dir>`; `DECO/jugar.bat` points it at the user's folder.
- **Generic runtime code.** No Patapon addresses or names in `runtime/src`/`runtime/include`
  (DEBUGGING.md §2). The pack format is PPSSPP's, so the loader is game-agnostic.
- **Off = identical.** With `PSPRECOMP_TEXTURES` unset, every frame is pixel-identical to today
  (`tools/tgadiff.py` against a scale-1 baseline: 0 differing pixels).
- **GL on the render thread only** (CLAUDE.md §7). PNG decoding may run on another thread later;
  GL uploads stay on the render thread.
- **Licences of new code:** xxHash (BSD-2, copy from `DECO/tools/ppsspp-src/ext/xxhash.{h,c}`) and
  stb_image (public domain / MIT, single header, pinned version) may be vendored under
  `runtime/third_party/`.

## Success criteria

1. Hash gate: on the language menu and in the hideout, most distinct texture keys the runtime
   computes (target: at least half; the pack claims 99%, video frames and render targets cannot
   match) are present in the pack. If not, stop and fix the hash before writing the loader.
2. With the pack enabled, the hideout, menus, world map and a mission show HD art where the pack has
   it, with no missing or garbled textures and no stutter the user notices.
3. Without the pack: 0 differing pixels against the baseline; all ctest and cargo tests pass.

## How PPSSPP keys a replacement (the part we must reproduce bit for bit)

Reference: `DECO/tools/ppsspp-src/GPU/Common/TextureReplacer.cpp` and `TextureCacheCommon.{h,cpp}`.

- **Texture parameters, level 0:** `addr` (TEXADDR0 | TEXBUFWIDTH0 bits 16-23 << 24),
  `dim = TEXSIZE0 & 0x0F0F`, `w = 1 << (dim & 0xF)`, `h = 1 << (dim >> 8)`, `fmt` (TEXFORMAT,
  values >= 11 become 5650). `bufw = TEXBUFWIDTH0 & alignMask16[fmt]`, and when that is 0 for
  fmt <= DXT5 it becomes `128 / bitsPerPixel[fmt]` (16 bytes). `bitsPerPixel` = 16,16,16,32,4,8,
  16,32,4,8,8 for formats 0..10.
- **Data hash** (`ComputeHash`, `hash = xxh64` in the pack): if a `[hashranges]` entry matches
  `(addr, w, h)` it replaces `w, h`; otherwise if `h == 512` and `0 < maxSeenV < 512`, `h = maxSeenV`
  (except swizzled CLUT4 512-tall atlases, which keep 512). `reduce = reduceHash ? (the
  [reducehashranges] value for (w,h), default 0.5) : 1`, clamped to [0,1].
  - `bufw <= w`: contiguous; `bytes = bpp * (bufw*h + (w - bufw)) / 8 * reduce` (integer maths in
    PPSSPP's order: the `/8` truncates before the float multiply, which truncates again to u32);
    `hash = (u32) XXH64(ram + addr, bytes, 0xBACD7814)`.
  - `bufw > w`: per row, `rowBytes = bpp*w/8 * reduce`, `stride = bpp*bufw/8`;
    `result = result*11 ^ (u32) XXH64(row, rowBytes, 0xBACD7814)` over `h` rows.
- **CLUT hash** (CLUT formats 4..7 only): at `LOADCLUT` the GE copies `loadBytes = blocks * 32`
  bytes (`blocks = LOADCLUT & 0x3F`, or 0x40 when `& 0x7F == 0x40`; 0 = no-op) from the CLUT
  address into a 2 KiB buffer, records `clutTotalBytes = loadBytes` and
  `clutMaxBytes = max(clutMaxBytes, loadBytes)`. The hash is `XXH32(buffer, min(clutTotalBytes +
  clutBaseBytes, clutMaxBytes), 0xC0108888)`, where `clutBaseBytes = startPos * (32-bit palette ?
  4 : 2)` and `startPos = ((CLUTFORMAT >> 16) & 0x1F) << 4`. It is recomputed after every
  `LOADCLUT` and whenever CLUTFORMAT changes. `cluthash = clutHash ^ CLUTFORMAT`.
- **Key:** `cachekey = ((addr & 0x3FFFFFFF) << 32) | dim`, then `^= cluthash` for CLUT formats.
  `ReplacementCacheKey = (cachekey, dataHash)`; its name is `%016llx%08x` (24 hex), plus `_<level>`
  for mip levels (unused: the pack sets `ignoreMipmap`).
- **Lookup** (`LookupWildcard`): with `ignoreAddress` the address half of `cachekey` is zeroed
  first, then tried in order: exact; `(cachekey & 0xFFFFFFFF, 0)`; [no-ignoreAddress only:
  `(cachekey, 0)`]; `(cachekey & 0xFFFFFFFF, hash)`; [no-ignoreAddress only: `(addr part, hash)`,
  `(addr part, 0)`]; `(0, hash)`. An empty filename means "explicitly ignored": keep the original.
- **Files:** `textures.ini` `[hashes]` maps `key = relative/path.png` (keys may omit trailing hex;
  sscanf `%16llx%8x_%d` semantics). Hash-named PNGs in the pack's root folder are also found without
  an ini entry. Paths with a `..` component are rejected. `[options]`: `hash` (only `xxh64` and
  `xxh32` supported here; `quick` is rejected with a log line), `ignoreAddress`, `reduceHash`,
  `ignoreMipmap`, `version`. `[filtering]`: `key = nearest|linear` forces the sampler for that
  texture. `[hashranges]`: `addr,w,h = newW,newH`. `[reducehashranges]`: `w,h = value`.

## Architecture

Three units with one job each, wired into the existing texture cache:

1. **`psp_texrep_hash` (pure, unit-tested):** `ge_texrep_bufw`, `ge_texrep_data_hash(ram, addr,
   bufw, w, h, fmt, swizzled, maxSeenV, opts)`, `ge_texrep_clut_hash(buf, totalBytes, maxBytes,
   clutformat)`, `ge_texrep_cachekey(addr, fmt, dim, cluthash)`, `ge_texrep_key_name(key)`.
   Inputs are plain values, so tests feed byte arrays and compare with hashes computed by a small
   standalone program built from PPSSPP's own code paths (same xxhash).
2. **`psp_texrep_pack` (pure parsing + lookup, unit-tested):** loads `textures.ini` text and a root
   file listing into: options, alias map, filtering map, hash ranges, reduce-hash ranges.
   `find(key) -> {found, ignored, path, forced_filter}` implements the wildcard order above. No GL,
   no file I/O inside the lookup (the loader passes file contents in), so it is testable with
   strings.
3. **`psp_texrep` (glue, render thread):** reads `PSPRECOMP_TEXTURES` at `ge_texture_init`, loads
   the pack, owns a map from replacement key to a GL texture (and a "no replacement" marker), and
   decodes PNGs with stb_image. Exposes `ge_texrep_lookup_and_upload(...)`, called by
   `ge_texture_bind` on a cache miss: if a replacement exists it uploads the PNG (RGBA8, its own
   size; UVs are normalised so no UV change) and the cache entry keeps that GL texture.

GE state additions (generic): `LOADCLUT` copies the CLUT bytes into a 2 KiB snapshot and tracks
total/max bytes; the CLUT hash is cached and invalidated on `LOADCLUT`/`CLUTFORMAT`. `maxSeenV`
(the largest V a draw used, in texels) is tracked per texture only for 512-tall textures, as PPSSPP
does, from the decoded through-mode UVs of the draw that binds it.

## Data flow

`ge_draw_prim` → `ge_texture_bind` (existing FNV content check) → on a miss:
compute the PPSSPP key → `pack.find(key)` → found: load PNG (cached by key) → `glTexImage2D` at
the PNG size → bind. Not found or ignored: decode from guest RAM as today. The replacement GL
texture lives as long as the cache entry; a decoded-PNG cache (CPU side) avoids re-decoding when
the same texture is evicted and comes back.

## Error handling

- Pack folder missing, no `textures.ini`, unsupported hash type: one `[TEXREP]` log line, the loader
  stays disabled, the game renders as without a pack.
- A PNG that fails to decode or whose path is unsafe: log once per key, use the original texture.
- Huge PNGs: no artificial limit; GL errors from `glTexImage2D` are checked once and logged.

## Diagnostics

- `PSPRECOMP_TEXTURES_DUMP=1`: one `[TEXREP] key=<24 hex> <w>x<h> fmt=<n> hit|miss|ignored` line per
  distinct key (the gate and later debugging use it).
- Debug socket: no new command needed.

## Performance

Synchronous PNG decode on first use, cached afterwards. If the user notices stutter, a conditional
task moves decoding to a worker thread (the render thread polls for ready images and keeps drawing
the original texture meanwhile, as PPSSPP does).

## Testing

- Unit: data hash (contiguous, strided, reduceHash, maxSeenV rule) and CLUT hash against reference
  values produced from PPSSPP's code on the same bytes; cache key and name formatting; ini parsing
  (options, hashes with partial keys, filtering, hashranges, ignored entries, `..` rejection);
  wildcard lookup order with and without `ignoreAddress`.
- Gate (before the loader): dump keys on the language menu and in the hideout; count how many are in
  the pack.
- Regression: scale-1 baseline without the pack, 0 differing pixels; all ctest and `cargo test`.
- User check at scale 4 with the pack: hideout, SQUARE menu, barracks, world map, a mission.

## Out of scope

Saving new textures (`SaveNewTextures`), zip packs, `.dds`/`.ktx2`/basis formats, mip levels,
replacing textures sampled from render targets (no RTT in the runtime), video frames.

## Credits

When the loader lands, README credits add the pack's authors as listed in its README (WallSoGB,
Shockturtle, owocek, efonte, Keamble, Hozzzd, KnotSora, Lin, Olimp666, wondaoxigen, Rin Casi), with
a link to the pack and a note that it is downloaded separately.
