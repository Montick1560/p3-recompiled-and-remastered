# M3c — HD texture replacement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Task 4 is a gate: if it fails, stop and fix the hash before Task 5.

**Goal:** Load a PPSSPP-format texture pack from `PSPRECOMP_TEXTURES=<dir>` and draw its PNGs in place of the textures the game decodes from guest RAM, keyed exactly like PPSSPP.

**Architecture:** Three new units. `psp_texrep_hash` (pure) reproduces PPSSPP's replacement key: XXH64 data hash, XXH32 CLUT hash of a `LOADCLUT` snapshot, `cachekey`, `%016llx%08x` name. `psp_texrep_pack` (pure) parses `textures.ini` plus the pack root listing and implements PPSSPP's wildcard lookup. `psp_texrep` (render-thread glue) reads the env, loads the pack, keeps the CLUT snapshot, logs keys, decodes PNGs with stb_image and hands them to `ge_texture_bind`, which uploads them on a texture-cache miss instead of decoding guest RAM.

**Tech Stack:** C++17, OpenGL 3.3 core, xxHash (BSD-2, from `DECO/tools/ppsspp-src/ext`), stb_image (public domain/MIT), ctest standalone test executables, debug socket TCP 9999.

**Spec:** `docs/superpowers/specs/2026-10-10-m3c-hd-textures-design.md` (read it: the "How PPSSPP keys a replacement" section is the contract). PPSSPP reference code: `DECO/tools/ppsspp-src/GPU/Common/TextureReplacer.cpp` (`ComputeHash`, `LookupWildcard`, `LoadIniValues`, `ScanForHashNamedFiles`, `ComputeAliasMap`) and `TextureCacheCommon.{h,cpp}` (`CacheKey`, `UpdateCurrentClut`, `LoadClut`, `UpdateMaxSeenV`). `DECO` = `C:/Users/torso/Documents/deco`.

## Where things stand (read first)

- Repo `DECO/psprecomp`, branch `patapon3-windows`, HEAD after the spec commit `a30c5ba` (M3b render scale done, pushed `c655352`). Read `DECO/build/autopilot/HANDOFF.md` first.
- The pack is installed (3116 files, ~825 MB) at `DECO/PPSSPP/memstick/PSP/TEXTURES/INFN00001/`. `textures.ini` has `hash = xxh64`, `ignoreAddress = True`, `ignoreMipmap = True`, `reduceHash = True`, about 3060 `[hashes]` lines like `00000000098b1cca0ddf97bd = effects/00000000098b1cca0ddf97bd.png`, an empty `[hashranges]`, and a `[games]` section to ignore.
- Texture cache today: `runtime/src/psp_ge_texture.cpp` `ge_texture_bind(rdram, frame_num)`: FNV content hash per bind, 256-entry LRU, decode via `ge_decode_texture`, `glTexImage2D` at the PSP size, `apply_sampler(state)`. It is called from `ge_draw_prim` (`runtime/src/psp_ge_draw.cpp`, the `if (state.texture_enable)` block after the VBO upload). `GE_CMD_LOADCLUT` is a no-op in `runtime/src/psp_ge.cpp` (`case GE_CMD_LOADCLUT:`).
- Existing navigation tool (no new driver scripts — user feedback 2026-10-09): `python -I DECO/build/tools/to_square.py <tag> [ENV=VAL ...]` boots with a copy of the user's save, Continues into the hideout, presses SQUARE, leaves the game running; log `DECO/build/logs/<tag>.log`.

## Global Constraints

- Never vendor the pack: it is read from `PSPRECOMP_TEXTURES=<dir>`; nothing from it enters the repo.
- Generic runtime code only: no Patapon addresses or names in `runtime/src` / `runtime/include` (DEBUGGING.md §2).
- Off = identical: with `PSPRECOMP_TEXTURES` unset, the scale-1 language-menu screenshot has 0 differing pixels against `DECO/build/shots/m3b_base_a.tga` (`tools/tgadiff.py`).
- All GL calls stay on the render (main) thread; the GE list interpreter and `ge_draw_prim` already run there.
- Vendored code goes under `runtime/third_party/<name>/` with a `README.txt` stating origin, version and licence; compiled with `-w`.
- Commits authored `Patapon Community` (repo-local config; check `git config user.name`), **no `Co-Authored-By` trailer**. Push only with `git push p3 patapon3-windows:main`; never force.
- Gates after every runtime change: `cd DECO/build/rtw && ninja && ctest` 0 failures; `cd DECO/psprecomp && cargo test -q` all ok. If the game is running (user playing), build test targets only: `ninja $(ninja -t targets all | grep -oE "^test_[A-Za-z0-9_]+\.exe" | sort -u)`.
- Talk to the user in Spanish; code, comments, commits in English. The user has given full autonomy (2026-10-10) except destroying their C: drive.

## Review Focus

1. **Off path changes pixels or speed.** With no `PSPRECOMP_TEXTURES`, nothing new runs per bind except the cheap CLUT snapshot memcpy at `LOADCLUT`. Expected: scale-1 baseline 0 diff (Task 5 Step 7) and no `[TEXREP]` lines.
2. **Strided textures (`bufw > w`) and `reduceHash` integer truncation.** PPSSPP truncates `bpp*w/8` before the float multiply and again on the u32 store; an off-by-one byte changes every key. Expected: Task 1 tests `strided_reduce` and `odd_reduce`.
3. **CLUT keys.** Palette textures (fonts, UI) are keyed with the CLUT hash of the bytes loaded by the last `LOADCLUT`, not the current RAM; a palette fade changes the key. Expected: Task 2 tests plus the gate counts hits on CLUT formats separately.
4. **Bad pack content:** missing folder, no `textures.ini`, `hash = quick`, a `..` path, an unreadable or corrupt PNG. Expected: one `[TEXREP]` log line, the original texture is drawn, no crash. Task 3 tests (`..`, quick) and Task 5 test (corrupt PNG bytes).
5. **Memory and stutter.** 4x replacements are 16x the pixels; a 512x512 source becomes 2048x2048 (16 MiB). Expected: CPU image cache capped (256 MiB, LRU), decode time logged under `PSPRECOMP_TEXTURES_DUMP`; the user reports whether entering screens stutters (Task 6), conditional Task 6b moves decoding off the render thread.

---

### Task 1: xxHash vendoring and the PPSSPP data hash / key (pure)

**Files:**
- Create: `runtime/third_party/xxhash/xxhash.h`, `runtime/third_party/xxhash/xxhash.c` (copied from `DECO/tools/ppsspp-src/ext/`), `runtime/third_party/xxhash/README.txt`
- Create: `runtime/include/psp_texrep_hash.h`, `runtime/src/psp_texrep_hash.cpp`
- Create: `runtime/tests/test_texrep_hash.cpp`
- Modify: `runtime/CMakeLists.txt` (xxhash static lib, source, test)

**Interfaces:**
- Produces (in `psp_texrep_hash.h`):
  - `int ge_texrep_bpp(int fmt);` bits per pixel (16,16,16,32,4,8,16,32,4,8,8 for 0..10; 0 otherwise)
  - `int ge_texrep_bufw(uint32_t texbufwidth0, int fmt);`
  - `uint16_t ge_texrep_max_seen_v(bool through, uint16_t draw_max_v);`
  - `int ge_texrep_hash_height(int h, int fmt, bool swizzled, uint16_t max_seen_v);`
  - `uint32_t ge_texrep_data_hash(const uint8_t* tex, uint64_t avail, int bufw, int w, int h, int fmt, float reduce, bool xxh32);`
  - `uint64_t ge_texrep_cachekey(uint32_t addr, int fmt, uint16_t dim, uint32_t cluthash);`
  - `struct GeTexrepKey { uint64_t cachekey; uint32_t hash; };` with `operator==`, and `std::string ge_texrep_key_name(const GeTexrepKey& k);`
  - CMake target `xxhash` (static lib, PUBLIC include dir `third_party/xxhash`).

- [ ] **Step 1: Vendor xxHash.**

```bash
cd /c/Users/torso/Documents/deco/psprecomp
mkdir -p runtime/third_party/xxhash
cp ../tools/ppsspp-src/ext/xxhash.h ../tools/ppsspp-src/ext/xxhash.c runtime/third_party/xxhash/
grep -m1 -n "XXH_VERSION_MAJOR\s" runtime/third_party/xxhash/xxhash.h; grep -m3 -n "XXH_VERSION_MINOR\s\|XXH_VERSION_RELEASE\s" runtime/third_party/xxhash/xxhash.h
```
Write `runtime/third_party/xxhash/README.txt`:

```text
xxHash - Extremely Fast Hash algorithm (Yann Collet), BSD 2-Clause licence
(see the header comment in xxhash.h). Copied unmodified from PPSSPP's
ext/xxhash.{h,c} (DECO/tools/ppsspp-src), the same version PPSSPP uses for
texture-replacement hashing, so our keys match PPSSPP texture packs.
Version: <the XXH_VERSION_MAJOR.MINOR.RELEASE printed by the grep above>.
```

- [ ] **Step 2: Write the failing tests** `runtime/tests/test_texrep_hash.cpp`:

```cpp
// Unit tests for psp_texrep_hash: PPSSPP texture-replacement keys
// (GPU/Common/TextureReplacer.cpp ComputeHash, TextureCacheCommon CacheKey).
// Expected values are computed with the same xxHash calls in the order PPSSPP
// makes them, so a wrong byte count, truncation or row order fails.
#include "psp_texrep_hash.h"
#include "xxhash.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static constexpr uint32_t SEED = 0xBACD7814u;

static std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i * 37u + 11u);
    return v;
}

static void xxhash_vectors() {
    CHECK(XXH64(nullptr, 0, 0) == 0xEF46DB3751D8E999ULL, "XXH64 empty vector");
    CHECK(XXH32(nullptr, 0, 0) == 0x02CC5D05u, "XXH32 empty vector");
}

static void bpp_and_bufw() {
    CHECK(ge_texrep_bpp(0) == 16 && ge_texrep_bpp(3) == 32 && ge_texrep_bpp(4) == 4, "bpp 5650/8888/CLUT4");
    CHECK(ge_texrep_bpp(5) == 8 && ge_texrep_bpp(8) == 4 && ge_texrep_bpp(10) == 8, "bpp CLUT8/DXT1/DXT5");
    CHECK(ge_texrep_bpp(11) == 0, "bpp invalid");
    CHECK(ge_texrep_bufw(64, 5) == 64, "CLUT8 bufw 64 aligned");
    CHECK(ge_texrep_bufw(70, 5) == 64, "CLUT8 bufw aligned down to 16 bytes");
    CHECK(ge_texrep_bufw(70, 3) == 68, "8888 bufw aligned to 4 px");
    CHECK(ge_texrep_bufw(0, 4) == 32, "CLUT4 bufw 0 -> 16 bytes = 32 px");
    CHECK(ge_texrep_bufw(0x090040, 5) == 64, "upper address bits ignored");
    CHECK(ge_texrep_bufw(100, 8) == 100, "DXT keeps the raw width");
}

static void max_seen_v_rules() {
    CHECK(ge_texrep_max_seen_v(true, 300) == 300, "through: draw max V");
    CHECK(ge_texrep_max_seen_v(true, 100) == 272, "through: at least 272");
    CHECK(ge_texrep_max_seen_v(true, 0) == 0, "through: no UVs -> 0");
    CHECK(ge_texrep_max_seen_v(false, 100) == 512, "transform: whole texture");
    CHECK(ge_texrep_hash_height(512, 5, false, 300) == 300, "512 tall clipped to max V");
    CHECK(ge_texrep_hash_height(512, 4, true, 300) == 512, "swizzled CLUT4 512 keeps 512");
    CHECK(ge_texrep_hash_height(256, 5, false, 100) == 256, "only 512-tall textures clip");
    CHECK(ge_texrep_hash_height(512, 5, false, 0) == 512, "max V 0 -> no clip");
}

static void contiguous() {
    const auto buf = pattern(4096);
    // CLUT8 64x32, bufw 64: 64*32 bytes; reduce 0.5 -> 1024.
    CHECK(ge_texrep_data_hash(buf.data(), buf.size(), 64, 64, 32, 5, 0.5f, false)
              == static_cast<uint32_t>(XXH64(buf.data(), 1024, SEED)), "contiguous reduce 0.5");
    CHECK(ge_texrep_data_hash(buf.data(), buf.size(), 64, 64, 32, 5, 1.0f, false)
              == static_cast<uint32_t>(XXH64(buf.data(), 2048, SEED)), "contiguous no reduce");
    CHECK(ge_texrep_data_hash(buf.data(), buf.size(), 64, 64, 32, 5, 1.0f, true)
              == XXH32(buf.data(), 2048, SEED), "contiguous xxh32");
    // bufw < w: totalPixels = bufw*h + (w - bufw) = 16*4 + 16 = 80 px of 5650 = 160 bytes.
    CHECK(ge_texrep_data_hash(buf.data(), buf.size(), 16, 32, 4, 0, 1.0f, false)
              == static_cast<uint32_t>(XXH64(buf.data(), 160, SEED)), "bufw < w total pixels");
    CHECK(ge_texrep_data_hash(buf.data(), 100, 64, 64, 32, 5, 1.0f, false) == 0, "past end of RAM -> 0");
}

static void strided_reduce() {
    const auto buf = pattern(64 * 1024);
    // 8888 w=64 h=8 bufw=128: row 256 bytes * 0.5 = 128, stride 512.
    uint32_t want = 0;
    for (int y = 0; y < 8; y++)
        want = (want * 11u) ^ static_cast<uint32_t>(XXH64(buf.data() + y * 512, 128, SEED));
    CHECK(ge_texrep_data_hash(buf.data(), buf.size(), 128, 64, 8, 3, 0.5f, false) == want, "strided reduce");
}

static void odd_reduce() {
    const auto buf = pattern(4096);
    // CLUT4 w=8 h=2 bufw=32: rowBytes = 4*8/8 = 4, * 0.75 = 3 (truncated), stride 16.
    uint32_t want = 0;
    for (int y = 0; y < 2; y++)
        want = (want * 11u) ^ static_cast<uint32_t>(XXH64(buf.data() + y * 16, 3, SEED));
    CHECK(ge_texrep_data_hash(buf.data(), buf.size(), 32, 8, 2, 4, 0.75f, false) == want, "odd reduce truncates");
    // 5650 w=4 h=1 bufw=4: 8 bytes * 0.3 = 2.4 -> 2.
    CHECK(ge_texrep_data_hash(buf.data(), buf.size(), 4, 4, 1, 0, 0.3f, false)
              == static_cast<uint32_t>(XXH64(buf.data(), 2, SEED)), "contiguous reduce truncates");
}

static void cachekey_and_name() {
    CHECK(ge_texrep_cachekey(0x09123450u, 5, 0x0506, 0xAABBCCDDu) == 0x09123450AABBC9DBULL, "CLUT key xors cluthash");
    CHECK(ge_texrep_cachekey(0x09123450u, 3, 0x0506, 0xAABBCCDDu) == 0x0912345000000506ULL, "non-CLUT ignores cluthash");
    CHECK(ge_texrep_cachekey(0x49123450u, 3, 0x0506, 0) == 0x0912345000000506ULL, "address masked to 30 bits");
    const GeTexrepKey k{0x00000000098B1CCAULL, 0x0DDF97BDu};
    CHECK(ge_texrep_key_name(k) == "00000000098b1cca0ddf97bd", "key name is %016llx%08x lower-case");
}

int main() {
    xxhash_vectors();
    bpp_and_bufw();
    max_seen_v_rules();
    contiguous();
    strided_reduce();
    odd_reduce();
    cachekey_and_name();
    if (failures == 0) std::printf("test_texrep_hash: all passed\n");
    return failures == 0 ? 0 : 1;
}
```

Register it in `runtime/CMakeLists.txt` next to the other standalone tests (after the `test_present` block), plus the xxhash library after the `at3_standalone` block:

```cmake
# Vendored xxHash (BSD-2; third_party/xxhash/README.txt): texture-replacement
# keys must match PPSSPP's, which hashes with this exact code.
add_library(xxhash STATIC third_party/xxhash/xxhash.c)
target_include_directories(xxhash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash)
target_compile_options(xxhash PRIVATE -w -O2)
```

```cmake
add_executable(test_texrep_hash tests/test_texrep_hash.cpp src/psp_texrep_hash.cpp)
target_include_directories(test_texrep_hash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_texrep_hash PRIVATE xxhash)
target_compile_options(test_texrep_hash PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME texrep_hash_tests COMMAND test_texrep_hash)
```
Also add `src/psp_texrep_hash.cpp` to `RUNTIME_SOURCES` (after `src/psp_ge_viewport.cpp`) and `xxhash` to `target_link_libraries(psprecomp_runtime PRIVATE ...)` (after `at3_standalone`).

- [ ] **Step 3: Run to verify it fails.**

Run: `cd /c/Users/torso/Documents/deco/build/rtw && cmake . > /dev/null && ninja test_texrep_hash`
Expected: error, `psp_texrep_hash.h` not found (or the source file missing).

- [ ] **Step 4: Implement.** `runtime/include/psp_texrep_hash.h`:

```cpp
#pragma once
// PPSSPP texture-replacement keys (GPU/Common/TextureReplacer.cpp ComputeHash,
// TextureCacheCommon.h CacheKey). Pure functions of plain values so packs made
// for PPSSPP match bit for bit. Spec: docs/superpowers/specs/
// 2026-10-10-m3c-hd-textures-design.md, "How PPSSPP keys a replacement".
#include <cstdint>
#include <string>

/// Bits per pixel of a GE texture format (0..10); 0 for invalid formats.
int ge_texrep_bpp(int fmt);

/// Level-0 buffer width as PPSSPP's GetTextureBufw: TEXBUFWIDTH0 aligned down
/// to 16 bytes (DXT: raw & 0x7FF); 0 becomes one 16-byte row.
int ge_texrep_bufw(uint32_t texbufwidth0, int fmt);

/// PPSSPP UpdateMaxSeenV for a new texture: through mode uses the draw's
/// largest V (texels), at least 272, or 0 when unknown; transform mode 512.
uint16_t ge_texrep_max_seen_v(bool through, uint16_t draw_max_v);

/// Rows ComputeHash hashes: a 512-tall texture is clipped to max_seen_v
/// (when 0 < max_seen_v < 512), except swizzled CLUT4 atlases.
int ge_texrep_hash_height(int h, int fmt, bool swizzled, uint16_t max_seen_v);

/// ComputeHash data hash (xxh64 truncated to 32 bits, or xxh32), seed
/// 0xBACD7814. `avail` = bytes readable from `tex`; a texture past it gives 0.
uint32_t ge_texrep_data_hash(const uint8_t* tex, uint64_t avail, int bufw, int w, int h,
                             int fmt, float reduce, bool xxh32);

/// TexCacheEntry::CacheKey: (addr & 0x3FFFFFFF) << 32 | dim, xor cluthash for
/// CLUT formats (4..7).
uint64_t ge_texrep_cachekey(uint32_t addr, int fmt, uint16_t dim, uint32_t cluthash);

struct GeTexrepKey {
    uint64_t cachekey;
    uint32_t hash;
    bool operator==(const GeTexrepKey& o) const { return cachekey == o.cachekey && hash == o.hash; }
};

/// "%016llx%08x": the file/ini name of a key (level 0).
std::string ge_texrep_key_name(const GeTexrepKey& k);
```

`runtime/src/psp_texrep_hash.cpp`:

```cpp
#include "psp_texrep_hash.h"

#include "xxhash.h"

#include <algorithm>
#include <cstdio>

namespace {
constexpr uint32_t kSeed = 0xBACD7814u;
constexpr int kBpp[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};

uint32_t hash_bytes(const uint8_t* p, uint32_t n, bool xxh32) {
    return xxh32 ? XXH32(p, n, kSeed) : static_cast<uint32_t>(XXH64(p, n, kSeed));
}
}  // namespace

int ge_texrep_bpp(int fmt) {
    return (fmt >= 0 && fmt <= 10) ? kBpp[fmt] : 0;
}

int ge_texrep_bufw(uint32_t texbufwidth0, int fmt) {
    const int bpp = ge_texrep_bpp(fmt);
    if (bpp == 0) return 0;
    const uint32_t mask = fmt >= 8 ? 0x7FFu : (0x7FFu & ~static_cast<uint32_t>((8 * 16) / bpp - 1));
    const int bufw = static_cast<int>(texbufwidth0 & mask);
    return bufw == 0 ? (8 * 16) / bpp : bufw;
}

uint16_t ge_texrep_max_seen_v(bool through, uint16_t draw_max_v) {
    if (!through) return 512;
    if (draw_max_v == 0) return 0;
    return std::max<uint16_t>(272, draw_max_v);
}

int ge_texrep_hash_height(int h, int fmt, bool swizzled, uint16_t max_seen_v) {
    const uint16_t seen = (h == 512 && swizzled && fmt == 4) ? 512 : max_seen_v;
    return (h == 512 && seen < 512 && seen != 0) ? static_cast<int>(seen) : h;
}

uint32_t ge_texrep_data_hash(const uint8_t* tex, uint64_t avail, int bufw, int w, int h,
                             int fmt, float reduce, bool xxh32) {
    const uint32_t bpp = static_cast<uint32_t>(ge_texrep_bpp(fmt));
    reduce = std::clamp(reduce, 0.0f, 1.0f);
    if (bufw <= w) {
        // Contiguous. PPSSPP: u32 sizeInRAM = (bpp * totalPixels) / 8 * reduce.
        const uint32_t total = static_cast<uint32_t>(bufw * h + (w - bufw));
        const uint32_t size = static_cast<uint32_t>((bpp * total) / 8 * reduce);
        if (avail < size) return 0;
        return hash_bytes(tex, size, xxh32);
    }
    // Strided: hash each row, combine as result * 11 ^ row.
    const uint32_t row = static_cast<uint32_t>((bpp * static_cast<uint32_t>(w)) / 8 * reduce);
    const uint32_t stride = (bpp * static_cast<uint32_t>(bufw)) / 8;
    const uint64_t span = h > 0 ? static_cast<uint64_t>(h - 1) * stride + row : 0;
    if (avail < span) return 0;
    uint32_t result = 0;
    for (int y = 0; y < h; y++) {
        result = (result * 11u) ^ hash_bytes(tex + static_cast<size_t>(y) * stride, row, xxh32);
    }
    return result;
}

uint64_t ge_texrep_cachekey(uint32_t addr, int fmt, uint16_t dim, uint32_t cluthash) {
    uint64_t key = (static_cast<uint64_t>(addr & 0x3FFFFFFFu) << 32) | dim;
    if (fmt & 4) key ^= cluthash;
    return key;
}

std::string ge_texrep_key_name(const GeTexrepKey& k) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx%08x",
                  static_cast<unsigned long long>(k.cachekey), k.hash);
    return buf;
}
```
Note on `fmt & 4`: PPSSPP's `hasClut = (format & 4) != 0`, which is also true for DXT3/DXT5 (9, 10) — keep it identical; their `cluthash` is 0 anyway (Task 2 only computes it for formats 4..7).

- [ ] **Step 5: Run the tests.**

Run: `cd /c/Users/torso/Documents/deco/build/rtw && ninja test_texrep_hash && ./test_texrep_hash.exe`
Expected: `test_texrep_hash: all passed`. If `XXH32(nullptr, 0, 0)` crashes on this xxhash version, pass a non-null empty buffer (`const uint8_t z = 0; XXH32(&z, 0, 0)`).

- [ ] **Step 6: Full gate and commit.**

```bash
cd /c/Users/torso/Documents/deco/build/rtw && ninja && ctest 2>&1 | tail -2
cd /c/Users/torso/Documents/deco/psprecomp
git add runtime/third_party/xxhash runtime/include/psp_texrep_hash.h runtime/src/psp_texrep_hash.cpp runtime/tests/test_texrep_hash.cpp runtime/CMakeLists.txt
git commit -m "feat(texrep): PPSSPP texture-replacement data hash and cache key"
```
Expected: 28/28 ctest.

### Task 2: CLUT snapshot at LOADCLUT and the key for the live GE state (pure + GE wiring)

**Files:**
- Modify: `runtime/include/psp_texrep_hash.h`, `runtime/src/psp_texrep_hash.cpp`
- Modify: `runtime/src/psp_ge.cpp` (`case GE_CMD_LOADCLUT:`)
- Create: `runtime/include/psp_texrep.h`, `runtime/src/psp_texrep.cpp` (module state; grows in Tasks 4-5)
- Test: `runtime/tests/test_texrep_hash.cpp`

**Interfaces:**
- Consumes: Task 1 functions.
- Produces (in `psp_texrep_hash.h`):
  - `struct GeClutSnapshot { uint8_t buf[2048]; uint32_t total_bytes; uint32_t max_bytes; };`
  - `uint32_t ge_clut_load_bytes(uint32_t loadclut_data);`
  - `void ge_clut_snapshot_load(GeClutSnapshot& s, const uint8_t* src, uint32_t load_bytes);` (`src == nullptr` = invalid address: zero-fill)
  - `uint32_t ge_texrep_cluthash(const GeClutSnapshot& s, uint32_t clutformat);` = `XXH32(buf, min(total + base, max), 0xC0108888) ^ clutformat`
  - `struct GeTexrepParams { uint32_t addr; uint16_t dim; int w, h, fmt, bufw; bool swizzled; };` and `GeTexrepParams ge_texrep_params(uint32_t texaddr0, uint32_t texbufwidth0, uint32_t texsize0, uint32_t texformat, uint32_t texmode);`
- Produces (in `psp_texrep.h`): `void ge_texrep_on_loadclut(const uint8_t* rdram, uint32_t clut_addr, uint32_t loadclut_data);` and `const GeClutSnapshot& ge_texrep_clut();`

- [ ] **Step 1: Failing tests.** Append to `runtime/tests/test_texrep_hash.cpp` above `int main()` and call both from `main()`:

```cpp
static void clut_snapshot() {
    CHECK(ge_clut_load_bytes(0x02) == 64, "2 blocks = 64 bytes");
    CHECK(ge_clut_load_bytes(0x43) == 96, "bit 6 ignored unless exactly 0x40");
    CHECK(ge_clut_load_bytes(0x40) == 2048, "0x40 blocks allowed");
    CHECK(ge_clut_load_bytes(0x00) == 0, "0 = no load");

    GeClutSnapshot s{};
    const auto src = pattern(2048);
    ge_clut_snapshot_load(s, src.data(), 64);
    CHECK(s.total_bytes == 64 && s.max_bytes == 64, "first load");
    CHECK(s.buf[63] == src[63], "bytes copied");
    ge_clut_snapshot_load(s, nullptr, 0);
    CHECK(s.total_bytes == 64, "0-byte load is a no-op");
    ge_clut_snapshot_load(s, src.data(), 32);
    CHECK(s.total_bytes == 32 && s.max_bytes == 64, "max keeps the largest load");
    ge_clut_snapshot_load(s, nullptr, 32);
    CHECK(s.buf[0] == 0 && s.buf[31] == 0 && s.buf[32] == src[32], "invalid source zero-fills only the load");

    // 16-bit palette (fmt 1), start pos 1 -> base 16 entries * 2 = 32 bytes.
    GeClutSnapshot t{};
    ge_clut_snapshot_load(t, src.data(), 64);
    const uint32_t fmt_start1 = 0x01 | (1u << 16);
    CHECK(ge_texrep_cluthash(t, fmt_start1) == (XXH32(t.buf, 64, 0xC0108888u) ^ fmt_start1),
          "min(total + base, max) = 64");
    const uint32_t fmt_plain = 0x03;  // 32-bit palette, start 0
    ge_clut_snapshot_load(t, src.data(), 32);
    CHECK(ge_texrep_cluthash(t, fmt_plain) == (XXH32(t.buf, 32, 0xC0108888u) ^ fmt_plain),
          "total = 32 after a smaller reload");
}

static void params_from_registers() {
    // TEXADDR0 0x123450, TEXBUFWIDTH0 bufw 64 | upper address 0x09 << 16,
    // TEXSIZE0 0x0506 (64x32), CLUT8, swizzled.
    const GeTexrepParams p = ge_texrep_params(0x123450u, (0x09u << 16) | 64u, 0x0506u, 5u, 1u);
    CHECK(p.addr == 0x09123450u, "address = TEXADDR0 | upper bits");
    CHECK(p.dim == 0x0506 && p.w == 64 && p.h == 32, "dim and size");
    CHECK(p.fmt == 5 && p.bufw == 64 && p.swizzled, "fmt, bufw, swizzle");
    const GeTexrepParams q = ge_texrep_params(0x12345Fu, 64u, 0x0F0F09u, 12u, 0u);
    CHECK(q.addr == 0x00123450u, "low 4 address bits dropped");
    CHECK(q.dim == 0x0F09, "dim keeps nibbles only");
    CHECK(q.fmt == 0, "invalid format -> 5650");
}
```

- [ ] **Step 2: Run** `ninja test_texrep_hash` → compile errors (`GeClutSnapshot`, `ge_texrep_params` undeclared).

- [ ] **Step 3: Implement.** Append to `psp_texrep_hash.h`:

```cpp
/// CLUT bytes copied at the last LOADCLUT (PPSSPP TextureCacheCommon::LoadClut).
/// The replacement key hashes these, not the palette's current RAM contents.
struct GeClutSnapshot {
    uint8_t buf[2048];
    uint32_t total_bytes;  // bytes of the last load
    uint32_t max_bytes;    // largest load so far
};

/// LOADCLUT data -> bytes: (data & 0x3F) blocks of 32, or 0x40 blocks when
/// (data & 0x7F) == 0x40.
uint32_t ge_clut_load_bytes(uint32_t loadclut_data);

/// Copy `load_bytes` from `src` (nullptr = invalid address: zeros). 0 = no-op.
void ge_clut_snapshot_load(GeClutSnapshot& s, const uint8_t* src, uint32_t load_bytes);

/// PPSSPP cluthash for the key: XXH32(buf, min(total + startPos*entryBytes,
/// max), 0xC0108888) ^ clutformat.
uint32_t ge_texrep_cluthash(const GeClutSnapshot& s, uint32_t clutformat);

/// Level-0 texture parameters from the raw GE registers, PPSSPP style.
struct GeTexrepParams {
    uint32_t addr;   // (TEXADDR0 & 0xFFFFF0) | ((TEXBUFWIDTH0 << 8) & 0x0F000000)
    uint16_t dim;    // TEXSIZE0 & 0x0F0F
    int w, h;        // 1 << nibbles of dim
    int fmt;         // TEXFORMAT, >= 11 -> 0 (5650)
    int bufw;        // ge_texrep_bufw
    bool swizzled;   // TEXMODE bit 0
};
GeTexrepParams ge_texrep_params(uint32_t texaddr0, uint32_t texbufwidth0, uint32_t texsize0,
                                uint32_t texformat, uint32_t texmode);
```

Append to `psp_texrep_hash.cpp`:

```cpp
uint32_t ge_clut_load_bytes(uint32_t loadclut_data) {
    const uint32_t blocks = (loadclut_data & 0x7F) == 0x40 ? 0x40 : (loadclut_data & 0x3F);
    return blocks * 32;
}

void ge_clut_snapshot_load(GeClutSnapshot& s, const uint8_t* src, uint32_t load_bytes) {
    if (load_bytes == 0) return;
    load_bytes = std::min<uint32_t>(load_bytes, sizeof(s.buf));
    if (src) {
        std::memcpy(s.buf, src, load_bytes);
    } else {
        std::memset(s.buf, 0, load_bytes);
    }
    s.total_bytes = load_bytes;
    s.max_bytes = std::max(s.max_bytes, load_bytes);
}

uint32_t ge_texrep_cluthash(const GeClutSnapshot& s, uint32_t clutformat) {
    const uint32_t entry_bytes = (clutformat & 3) == 3 ? 4 : 2;
    const uint32_t base = (((clutformat >> 16) & 0x1F) << 4) * entry_bytes;
    const uint32_t n = std::min(s.total_bytes + base, s.max_bytes);
    return XXH32(s.buf, n, 0xC0108888u) ^ clutformat;
}

GeTexrepParams ge_texrep_params(uint32_t texaddr0, uint32_t texbufwidth0, uint32_t texsize0,
                                uint32_t texformat, uint32_t texmode) {
    GeTexrepParams p{};
    p.addr = (texaddr0 & 0xFFFFF0u) | ((texbufwidth0 << 8) & 0x0F000000u);
    p.dim = static_cast<uint16_t>(texsize0 & 0x0F0Fu);
    p.w = 1 << (p.dim & 0xF);
    p.h = 1 << ((p.dim >> 8) & 0xF);
    p.fmt = texformat >= 11 ? 0 : static_cast<int>(texformat);
    p.bufw = ge_texrep_bufw(texbufwidth0, p.fmt);
    p.swizzled = (texmode & 1) != 0;
    return p;
}
```
Add `#include <cstring>` to the .cpp.

`runtime/include/psp_texrep.h`:

```cpp
#pragma once
// Texture replacement from a PPSSPP-format pack (PSPRECOMP_TEXTURES=<dir>).
// Render (GE) thread only. Spec: docs/superpowers/specs/2026-10-10-m3c-hd-textures-design.md
#include "psp_texrep_hash.h"

#include <cstdint>

/// GE_CMD_LOADCLUT: snapshot the palette bytes the replacement key hashes.
void ge_texrep_on_loadclut(const uint8_t* rdram, uint32_t clut_addr, uint32_t loadclut_data);

/// The current CLUT snapshot.
const GeClutSnapshot& ge_texrep_clut();
```

`runtime/src/psp_texrep.cpp`:

```cpp
#include "psp_texrep.h"

#include "psp_memory.h"

namespace {
GeClutSnapshot g_clut{};
}  // namespace

void ge_texrep_on_loadclut(const uint8_t* rdram, uint32_t clut_addr, uint32_t loadclut_data) {
    const uint32_t bytes = ge_clut_load_bytes(loadclut_data);
    const uint32_t off = clut_addr & PSP_ADDR_MASK;
    const bool valid = clut_addr != 0 && static_cast<uint64_t>(off) + bytes <= PSP_MEM_SIZE;
    ge_clut_snapshot_load(g_clut, valid ? rdram + off : nullptr, bytes);
}

const GeClutSnapshot& ge_texrep_clut() { return g_clut; }
```
(Check that `PSP_ADDR_MASK` / `PSP_MEM_SIZE` come from `psp_memory.h`, as `psp_ge_texture.cpp` uses them; include whatever header that file includes for them.)

In `runtime/src/psp_ge.cpp`, replace the `GE_CMD_LOADCLUT` case body (add `#include "psp_texrep.h"`):

```cpp
        case GE_CMD_LOADCLUT:
            // Snapshot the palette for texture-replacement keys (PPSSPP
            // LoadClut); decoding still reads the CLUT from RAM at bind time.
            ge_texrep_on_loadclut(rdram,
                                  (g_ge_state.clut_addr & 0xFFFFF0u) |
                                      ((g_ge_state.clut_addr_upper << 8) & 0x0F000000u),
                                  data);
            break;
```
Add `src/psp_texrep.cpp` to `RUNTIME_SOURCES`.

- [ ] **Step 4: Run** `ninja test_texrep_hash && ./test_texrep_hash.exe` → `all passed`; then `ninja && ctest` → 28/28.

- [ ] **Step 5: Commit** `feat(texrep): CLUT snapshot at LOADCLUT and texture params for the key` (files: the two hash files, the test, `psp_texrep.{h,cpp}`, `psp_ge.cpp`, `CMakeLists.txt`).

### Task 3: Pack parser and PPSSPP wildcard lookup (pure)

**Files:**
- Create: `runtime/include/psp_texrep_pack.h`, `runtime/src/psp_texrep_pack.cpp`, `runtime/tests/test_texrep_pack.cpp`
- Modify: `runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: `GeTexrepKey` (Task 1).
- Produces:

```cpp
enum class TexrepFilter { None = 0, Nearest = 1, Linear = 2 };
struct TexrepFind { bool found = false; bool ignored = false; std::string path; };
class TexrepPack {
public:
    // ini_text: textures.ini contents; root_files: file names in the pack root.
    bool load(const std::string& ini_text, const std::vector<std::string>& root_files, std::string* error);
    bool ignore_address() const;
    bool reduce_hash() const;
    bool xxh32() const;
    float reduce_for(int w, int h) const;                                  // [reducehashranges] or 0.5
    bool hash_range(uint32_t addr, int w, int h, int* nw, int* nh) const;  // [hashranges]
    TexrepFind find(GeTexrepKey key) const;                                // aliases, wildcard order
    TexrepFilter filter(GeTexrepKey key) const;                            // [filtering]
    size_t alias_count() const;
};
```

- [ ] **Step 1: Failing tests** `runtime/tests/test_texrep_pack.cpp`:

```cpp
// Unit tests for TexrepPack: textures.ini parsing and PPSSPP's LookupWildcard
// order (GPU/Common/TextureReplacer.cpp).
#include "psp_texrep_pack.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static const char* kIni =
    "# comment\n"
    "[options]\n"
    "version = 1\n"
    "hash = xxh64\n"
    "ignoreAddress = True\n"
    "ignoreMipmap = True\n"
    "reduceHash = True\n"
    "[games]\n"
    "UCES01421 = textures.ini\n"
    "[hashes]\n"
    "; TEXTURES: Start\n"
    "00000000098b1cca0ddf97bd = effects/00000000098b1cca0ddf97bd.png\n"
    "000000001111111122222222 = ui\\back.png\n"
    "0000000033333333 = clutonly.png\n"
    "000000004444444455555555 =\n"
    "000000006666666677777777 = ../escape.png\n"
    "000000008888888899999999_1 = level1.png\n"
    "[filtering]\n"
    "00000000098b1cca0ddf97bd = nearest\n"
    "[reducehashranges]\n"
    "512,512 = 0.25\n"
    "[hashranges]\n"
    "0x08800000,512,512 = 480,272\n";

static TexrepPack loaded() {
    TexrepPack p;
    std::string err;
    const std::vector<std::string> root = {"aaaaaaaabbbbbbbbcccccccc.png", "readme.md", "abc.png"};
    CHECK(p.load(kIni, root, &err), "pack loads");
    return p;
}

static void options() {
    const TexrepPack p = loaded();
    CHECK(p.ignore_address() && p.reduce_hash() && !p.xxh32(), "options parsed");
    CHECK(p.reduce_for(512, 512) == 0.25f, "reducehashranges entry");
    CHECK(p.reduce_for(256, 256) == 0.5f, "reduce default 0.5");
    int w = 0, h = 0;
    CHECK(p.hash_range(0x08800000u, 512, 512, &w, &h) && w == 480 && h == 272, "hashrange entry");
    CHECK(!p.hash_range(0x08800010u, 512, 512, &w, &h) && w == 512 && h == 512, "no hashrange keeps size");
}

static void lookups() {
    const TexrepPack p = loaded();
    // Exact key (address already zero in the pack).
    TexrepFind f = p.find({0x00000000098B1CCAULL, 0x0DDF97BDu});
    CHECK(f.found && !f.ignored && f.path == "effects/00000000098b1cca0ddf97bd.png", "exact");
    // ignoreAddress: the address half of a live key is zeroed first.
    f = p.find({0x09A0000000000000ULL | 0x098B1CCAULL, 0x0DDF97BDu});
    CHECK(f.found && f.path == "effects/00000000098b1cca0ddf97bd.png", "address ignored");
    CHECK(p.find({0x11111111ULL, 0x22222222u}).path == "ui/back.png", "backslash -> slash");
    // Partial key "0000000033333333" = (cachekey, hash 0): CLUT-only wildcard.
    f = p.find({0x33333333ULL, 0xDEADBEEFu});
    CHECK(f.found && f.path == "clutonly.png", "clut-only wildcard (hash 0)");
    f = p.find({0x44444444ULL, 0x55555555u});
    CHECK(f.found && f.ignored, "empty value = ignored");
    CHECK(!p.find({0x66666666ULL, 0x77777777u}).found, "'..' path rejected");
    f = p.find({0x88888888ULL, 0x99999999u});
    CHECK(f.found && f.ignored, "level-1-only entry = ignored (PPSSPP ComputeAliasMap breaks at mip 0)");
    CHECK(p.find({0xAAAAAAAABBBBBBBBULL & 0xFFFFFFFFULL, 0xCCCCCCCCu}).found == false,
          "root file key has address aaaaaaaa: not found once the address is zeroed");
    CHECK(p.alias_count() >= 5, "aliases counted");
    CHECK(p.filter({0x098B1CCAULL, 0x0DDF97BDu}) == TexrepFilter::Nearest, "filtering entry");
    CHECK(p.filter({0x1ULL, 0x2u}) == TexrepFilter::None, "no filtering entry");
}

static void root_files_without_ignore_address() {
    TexrepPack p;
    std::string err;
    CHECK(p.load("[options]\nhash = xxh64\n", {"aaaaaaaabbbbbbbbcccccccc.png", "x.png"}, &err), "root-only pack");
    TexrepFind f = p.find({0xAAAAAAAABBBBBBBBULL, 0xCCCCCCCCu});
    CHECK(f.found && f.path == "aaaaaaaabbbbbbbbcccccccc.png", "hash-named root file");
    // Wildcard (0, hash): any key with that data hash.
    TexrepPack q;
    CHECK(q.load("[options]\nhash = xxh64\n[hashes]\n0000000000000000abcdef01 = any.png\n", {}, &err), "data-hash-only pack");
    CHECK(q.find({0x0912345000000506ULL, 0xABCDEF01u}).path == "any.png", "data-hash-only wildcard");
}

static void bad_packs() {
    TexrepPack p;
    std::string err;
    CHECK(!p.load("[options]\nhash = quick\n", {}, &err), "quick hash rejected");
    CHECK(!err.empty(), "error text set");
    CHECK(!p.load("[hashes]\n000000001111111122222222 = a.png\n", {}, &err), "missing hash type rejected");
}

int main() {
    options();
    lookups();
    root_files_without_ignore_address();
    bad_packs();
    if (failures == 0) std::printf("test_texrep_pack: all passed\n");
    return failures == 0 ? 0 : 1;
}
```
CMake:

```cmake
add_executable(test_texrep_pack tests/test_texrep_pack.cpp src/psp_texrep_pack.cpp)
target_include_directories(test_texrep_pack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(test_texrep_pack PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
add_test(NAME texrep_pack_tests COMMAND test_texrep_pack)
```
and `src/psp_texrep_pack.cpp` in `RUNTIME_SOURCES`.

- [ ] **Step 2: Run** `cmake . && ninja test_texrep_pack` → error, header missing.

- [ ] **Step 3: Implement** `runtime/include/psp_texrep_pack.h` (the class above, with this header comment and private members):

```cpp
#pragma once
// A PPSSPP texture pack's textures.ini + root listing, and PPSSPP's lookup
// (TextureReplacer.cpp LoadIniValues / ScanForHashNamedFiles / ComputeAliasMap /
// LookupWildcard). Pure: no file I/O, no GL.
#include "psp_texrep_hash.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

enum class TexrepFilter { None = 0, Nearest = 1, Linear = 2 };  // TexCacheEntry::forced_filter

struct TexrepFind {
    bool found = false;    // the pack lists this texture
    bool ignored = false;  // listed with an empty file name: keep the original
    std::string path;      // relative to the pack folder, '/' separators
};

struct GeTexrepKeyHash {
    size_t operator()(const GeTexrepKey& k) const {
        return std::hash<uint64_t>()(k.cachekey ^ (static_cast<uint64_t>(k.hash) * 0x9E3779B97F4A7C15ULL));
    }
};

class TexrepPack {
public:
    bool load(const std::string& ini_text, const std::vector<std::string>& root_files, std::string* error);
    bool ignore_address() const { return ignore_address_; }
    bool reduce_hash() const { return reduce_hash_; }
    bool xxh32() const { return xxh32_; }
    float reduce_for(int w, int h) const;
    bool hash_range(uint32_t addr, int w, int h, int* nw, int* nh) const;
    TexrepFind find(GeTexrepKey key) const;
    TexrepFilter filter(GeTexrepKey key) const;
    size_t alias_count() const { return aliases_.size(); }

private:
    template <typename V>
    const V* wildcard(const std::unordered_map<GeTexrepKey, V, GeTexrepKeyHash>& m, GeTexrepKey key) const;

    bool ignore_address_ = false;
    bool reduce_hash_ = false;
    bool xxh32_ = false;
    std::unordered_map<GeTexrepKey, std::string, GeTexrepKeyHash> aliases_;   // "" = ignored
    std::unordered_map<GeTexrepKey, TexrepFilter, GeTexrepKeyHash> filtering_;
    std::map<uint64_t, std::pair<int, int>> hashranges_;  // addr<<32 | w<<16 | h
    std::map<uint64_t, float> reduceranges_;              // w<<16 | h
};
```

`runtime/src/psp_texrep_pack.cpp`:

```cpp
#include "psp_texrep_pack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_bool(const std::string& v) {
    const std::string l = lower(v);
    return l == "true" || l == "1" || l == "yes" || l == "on";
}

// sscanf("%16llx%8x_%d") semantics: >= 1 field parsed is a valid key.
bool parse_key(const std::string& k, GeTexrepKey* key, int* level) {
    unsigned long long ck = 0;
    unsigned int h = 0;
    int lv = 0;
    if (std::sscanf(k.c_str(), "%16llx%8x_%d", &ck, &h, &lv) < 1) return false;
    *key = {ck, h};
    *level = lv;
    return true;
}

bool parse_u32(const std::string& s, uint32_t* out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(t.c_str(), &end, (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) ? 16 : 10);
    if (end == t.c_str() || *end != '\0') return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, sep)) out.push_back(trim(part));
    return out;
}

bool has_parent_component(const std::string& p) {
    std::string norm = p;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    for (const std::string& c : split(norm, '/')) {
        if (c == "..") return true;
    }
    return false;
}

}  // namespace

bool TexrepPack::load(const std::string& ini_text, const std::vector<std::string>& root_files,
                      std::string* error) {
    *this = TexrepPack{};
    // filename map: key -> level -> file (PPSSPP builds this, then ComputeAliasMap).
    std::map<std::pair<uint64_t, uint32_t>, std::map<int, std::string>> files;
    for (const std::string& name : root_files) {
        const size_t dot = name.rfind('.');
        if (dot == std::string::npos || lower(name.substr(dot)) != ".png") continue;
        const std::string stem = name.substr(0, dot);
        if (!(stem.size() == 24 || (stem.size() >= 26 && stem.size() <= 27 && stem[24] == '_'))) continue;
        GeTexrepKey key{};
        int level = 0;
        if (parse_key(stem, &key, &level)) files[{key.cachekey, key.hash}][level] = name;
    }

    std::string section, hash_type;
    std::istringstream in(ini_text);
    std::string raw;
    while (std::getline(in, raw)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq));
        const std::string v = trim(line.substr(eq + 1));
        if (section == "options") {
            const std::string lk = lower(k);
            if (lk == "hash") hash_type = lower(v);
            else if (lk == "ignoreaddress") ignore_address_ = parse_bool(v);
            else if (lk == "reducehash") reduce_hash_ = parse_bool(v);
        } else if (section == "hashes") {
            GeTexrepKey key{};
            int level = 0;
            if (!parse_key(k, &key, &level)) continue;
            if (has_parent_component(v)) {
                std::fprintf(stderr, "[TEXREP] ignoring path with '..': %s\n", v.c_str());
                continue;
            }
            files[{key.cachekey, key.hash}][level] = v;
        } else if (section == "filtering") {
            GeTexrepKey key{};
            int level = 0;
            if (!parse_key(k, &key, &level)) continue;
            const std::string lv = lower(v);
            if (lv == "nearest") filtering_[key] = TexrepFilter::Nearest;
            else if (lv == "linear") filtering_[key] = TexrepFilter::Linear;
        } else if (section == "hashranges") {
            const auto kp = split(k, ',');
            const auto vp = split(v, ',');
            uint32_t addr = 0, fw = 0, fh = 0, tw = 0, th = 0;
            if (kp.size() != 3 || vp.size() != 2) continue;
            const std::string a = (kp[0].rfind("0x", 0) == 0 || kp[0].rfind("0X", 0) == 0) ? kp[0] : "0x" + kp[0];
            if (!parse_u32(a, &addr) || !parse_u32(kp[1], &fw) || !parse_u32(kp[2], &fh) ||
                !parse_u32(vp[0], &tw) || !parse_u32(vp[1], &th)) continue;
            if (tw > fw || th > fh || tw == 0 || th == 0) continue;
            hashranges_[(static_cast<uint64_t>(addr) << 32) | (static_cast<uint64_t>(fw) << 16) | fh] = {
                static_cast<int>(tw), static_cast<int>(th)};
        } else if (section == "reducehashranges") {
            const auto kp = split(k, ',');
            uint32_t fw = 0, fh = 0;
            if (kp.size() != 2 || !parse_u32(kp[0], &fw) || !parse_u32(kp[1], &fh)) continue;
            const float r = std::strtof(v.c_str(), nullptr);
            if (r == 0.0f) continue;
            reduceranges_[(static_cast<uint64_t>(fw) << 16) | fh] = r;
        }
    }

    if (hash_type.empty()) {
        *error = "textures.ini: hash type not specified";
        return false;
    }
    if (hash_type == "xxh32") {
        xxh32_ = true;
    } else if (hash_type != "xxh64") {
        *error = "textures.ini: unsupported hash type '" + hash_type + "' (xxh64/xxh32 only)";
        return false;
    }

    // ComputeAliasMap, level 0 only (packs use ignoreMipmap; mips are out of scope).
    // No level-0 file = PPSSPP's empty alias = "ignored" (keep the original).
    for (const auto& [k, levels] : files) {
        const auto it = levels.find(0);
        std::string path = it == levels.end() ? std::string() : it->second;
        std::replace(path.begin(), path.end(), '\\', '/');
        aliases_[{k.first, k.second}] = path;
    }
    return true;
}

float TexrepPack::reduce_for(int w, int h) const {
    const auto it = reduceranges_.find((static_cast<uint64_t>(w) << 16) | static_cast<uint64_t>(h));
    return it != reduceranges_.end() ? it->second : 0.5f;
}

bool TexrepPack::hash_range(uint32_t addr, int w, int h, int* nw, int* nh) const {
    const auto it = hashranges_.find((static_cast<uint64_t>(addr) << 32) |
                                     (static_cast<uint64_t>(w) << 16) | static_cast<uint64_t>(h));
    if (it == hashranges_.end()) {
        *nw = w;
        *nh = h;
        return false;
    }
    *nw = it->second.first;
    *nh = it->second.second;
    return true;
}

template <typename V>
const V* TexrepPack::wildcard(const std::unordered_map<GeTexrepKey, V, GeTexrepKeyHash>& m,
                              GeTexrepKey key) const {
    // PPSSPP FindReplacement zeroes the address first when ignoreAddress is set,
    // then LookupWildcard tries these in order.
    if (ignore_address_) key.cachekey &= 0xFFFFFFFFULL;
    const uint64_t ck = key.cachekey;
    const uint32_t h = key.hash;
    auto try_key = [&](uint64_t c, uint32_t d) -> const V* {
        const auto it = m.find({c, d});
        return it != m.end() ? &it->second : nullptr;
    };
    if (const V* v = try_key(ck, h)) return v;
    if (const V* v = try_key(ck & 0xFFFFFFFFULL, 0)) return v;
    if (!ignore_address_) {
        if (const V* v = try_key(ck, 0)) return v;
    }
    if (const V* v = try_key(ck & 0xFFFFFFFFULL, h)) return v;
    if (!ignore_address_) {
        if (const V* v = try_key(ck & ~0xFFFFFFFFULL, h)) return v;
        if (const V* v = try_key(ck & ~0xFFFFFFFFULL, 0)) return v;
    }
    return try_key(0, h);
}

TexrepFind TexrepPack::find(GeTexrepKey key) const {
    TexrepFind f;
    if (const std::string* p = wildcard(aliases_, key)) {
        f.found = true;
        f.ignored = p->empty();
        f.path = *p;
    }
    return f;
}

TexrepFilter TexrepPack::filter(GeTexrepKey key) const {
    if (const TexrepFilter* f = wildcard(filtering_, key)) return *f;
    const auto it = filtering_.find({0, 0});  // global wildcard
    return it != filtering_.end() ? it->second : TexrepFilter::None;
}
```
Note: the `"... = ../escape.png"` line is dropped (not found), as PPSSPP does; an empty value and a level-1-only entry both become an ignored alias (`""`), as PPSSPP's `ComputeAliasMap` does.

- [ ] **Step 4: Run** `ninja test_texrep_pack && ./test_texrep_pack.exe` → `all passed`. If a check fails, compare against the PPSSPP function named in the test comment before changing the test.

- [ ] **Step 5: Full gate, commit** `feat(texrep): textures.ini parser and PPSSPP wildcard lookup` (29/29 ctest).

### Task 4: Load the pack at runtime, log keys, and the HASH GATE

**Files:**
- Modify: `runtime/include/psp_texrep.h`, `runtime/src/psp_texrep.cpp`
- Modify: `runtime/src/psp_ge_texture.cpp` (`ge_texture_init`, `ge_texture_bind`), `runtime/include/psp_ge_texture.h`
- Modify: `runtime/src/psp_ge_draw.cpp` (pass the draw's max V)

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces:
  - `void ge_texrep_init();` reads `PSPRECOMP_TEXTURES` and `PSPRECOMP_TEXTURES_DUMP`, loads `<dir>/textures.ini` and the root listing; logs `[TEXREP] pack <dir>: <n> textures, hash=<xxh64|xxh32> ignoreAddress=<0|1> reduceHash=<0|1>` or `[TEXREP] disabled: <reason>`; silent when the env is unset.
  - `bool ge_texrep_enabled();`
  - `GeTexrepKey ge_texrep_key(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v);`
  - `TexrepFind ge_texrep_lookup(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v, GeTexrepKey* key_out);` (computes the key, looks it up, logs it once under DUMP as `[TEXREP] key=<24 hex> <w>x<h> fmt=<n> hit|miss|ignored`)
  - `GLuint ge_texture_bind(uint8_t* rdram, uint32_t frame_num, uint16_t draw_max_v = 0);`

- [ ] **Step 1: Implement the key for the live state** in `psp_texrep.cpp` (no unit test: it only composes tested pure functions; the gate below is its test):

```cpp
GeTexrepKey ge_texrep_key(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v) {
    const GeTexrepParams p = ge_texrep_params(s.tex_addr[0], s.tex_bufw[0], s.tex_size[0],
                                              s.tex_format, s.tex_mode);
    int w = p.w, h = p.h;
    if (!g_pack.hash_range(p.addr, p.w, p.h, &w, &h)) {
        const bool through = (s.vertex_type & 0x00800000u) != 0;
        h = ge_texrep_hash_height(h, p.fmt, p.swizzled, ge_texrep_max_seen_v(through, draw_max_v));
    }
    const float reduce = g_pack.reduce_hash() ? g_pack.reduce_for(w, h) : 1.0f;
    const uint32_t off = p.addr & PSP_ADDR_MASK;
    const uint64_t avail = off < PSP_MEM_SIZE ? PSP_MEM_SIZE - off : 0;
    const uint32_t data = ge_texrep_data_hash(rdram + off, avail, p.bufw, w, h, p.fmt, reduce, g_pack.xxh32());
    const uint32_t cluthash = (p.fmt >= 4 && p.fmt <= 7) ? ge_texrep_cluthash(g_clut, s.clut_format) : 0;
    return {ge_texrep_cachekey(p.addr, p.fmt, p.dim, cluthash), data};
}
```
(`g_pack` is a `TexrepPack` in the module's anonymous namespace; `#include "psp_ge.h"` and `"psp_texrep_pack.h"` in `psp_texrep.h`.)

- [ ] **Step 2: Init and lookup with DUMP logging.**

```cpp
// in the anonymous namespace
TexrepPack g_pack;
bool g_enabled = false;
bool g_dump = false;
std::string g_dir;
std::unordered_set<std::string> g_logged;

void ge_texrep_init() {
    g_enabled = false;
    g_logged.clear();
    const char* dir = std::getenv("PSPRECOMP_TEXTURES");
    if (!dir || !dir[0]) return;
    const char* dump = std::getenv("PSPRECOMP_TEXTURES_DUMP");
    g_dump = dump && dump[0] && dump[0] != '0';
    g_dir = dir;
    std::replace(g_dir.begin(), g_dir.end(), '\\', '/');
    while (!g_dir.empty() && g_dir.back() == '/') g_dir.pop_back();
    std::ifstream f(g_dir + "/textures.ini", std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[TEXREP] disabled: no textures.ini in %s\n", g_dir.c_str());
        return;
    }
    std::stringstream text;
    text << f.rdbuf();
    std::vector<std::string> root;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(g_dir), ec)) {
        if (e.is_regular_file(ec)) root.push_back(e.path().filename().u8string());
    }
    std::string err;
    if (!g_pack.load(text.str(), root, &err)) {
        std::fprintf(stderr, "[TEXREP] disabled: %s\n", err.c_str());
        return;
    }
    g_enabled = true;
    std::fprintf(stderr, "[TEXREP] pack %s: %zu textures, hash=%s ignoreAddress=%d reduceHash=%d\n",
                 g_dir.c_str(), g_pack.alias_count(), g_pack.xxh32() ? "xxh32" : "xxh64",
                 g_pack.ignore_address() ? 1 : 0, g_pack.reduce_hash() ? 1 : 0);
}

bool ge_texrep_enabled() { return g_enabled; }

TexrepFind ge_texrep_lookup(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v,
                            GeTexrepKey* key_out) {
    const GeTexrepKey key = ge_texrep_key(rdram, s, draw_max_v);
    if (key_out) *key_out = key;
    const TexrepFind f = g_pack.find(key);
    if (g_dump) {
        const std::string name = ge_texrep_key_name(key);
        if (g_logged.insert(name).second) {
            const GeTexrepParams p = ge_texrep_params(s.tex_addr[0], s.tex_bufw[0], s.tex_size[0],
                                                      s.tex_format, s.tex_mode);
            std::fprintf(stderr, "[TEXREP] key=%s %dx%d fmt=%d %s\n", name.c_str(), p.w, p.h, p.fmt,
                         f.ignored ? "ignored" : (f.found ? "hit" : "miss"));
        }
    }
    return f;
}
```
Includes: `<algorithm> <cstdio> <cstdlib> <filesystem> <fstream> <sstream> <string> <unordered_set> <vector>`. If `std::filesystem` fails to link with this llvm-mingw toolchain, add `stdc++fs` only if the link error names it (clang/libc++ normally needs nothing).

Wire it:
- `ge_texture_init()`: call `ge_texrep_init();` after the `[TEX] Texture cache initialized` line.
- `ge_texture_bind` gets the third parameter `uint16_t draw_max_v` (header default `= 0`); on a cache miss, before `ge_decode_texture`, add:

```cpp
    if (ge_texrep_enabled()) {
        (void)ge_texrep_lookup(rdram, state, draw_max_v, nullptr);  // Task 5 uses the result
    }
```
- `ge_draw_prim` (`psp_ge_draw.cpp`): right before the `// Render scale > 1: snap 2D` block, compute the draw's max V in texels (raw through-mode UVs, before `ge_transform_vertices`):

```cpp
    // Largest V (texels) this draw samples: PPSSPP's vertBounds.maxV, used by
    // texture replacement to hash only the visible rows of 512-tall textures.
    uint16_t draw_max_v = 0;
    if (state.texture_enable && ge_vtype_through(state.vertex_type)) {
        for (const DecodedVertex& v : decoded) {
            if (v.has_uv && v.uv[1] > draw_max_v) {
                draw_max_v = static_cast<uint16_t>(std::min(v.uv[1], 65535.0f));
            }
        }
    }
```
and call `ge_texture_bind(rdram, g_frame_counter, draw_max_v);`.

- [ ] **Step 3: Build and check the off path.** `ninja && ctest` (29/29). Boot with no env to the language menu, screenshot, `tgadiff` against `DECO/build/shots/m3b_base_a.tga`:

```bash
cd /c/Users/torso/Documents/deco/build/rtw
taskkill //IM psprecomp_runtime.exe //F 2>/dev/null
python -I - <<'PY'
import os, socket, subprocess, time
env = dict(os.environ, PSPRECOMP_DISC0='C:/Users/torso/Documents/deco/disc0')
log = 'C:/Users/torso/Documents/deco/build/logs/m3c_off.log'
p = subprocess.Popen(['./psprecomp_runtime.exe'], env=env, stdout=open(log, 'wb'), stderr=subprocess.STDOUT)
t = time.time()
while b'DATAMS.BND' not in open(log, 'rb').read() and time.time() - t < 120: time.sleep(1)
time.sleep(8)
s = socket.create_connection(('127.0.0.1', 9999)); s.sendall(b'S C:/Users/torso/Documents/deco/build/shots/m3c_off.tga\n'); print(s.recv(64))
subprocess.run(['taskkill', '/IM', 'psprecomp_runtime.exe', '/F'], capture_output=True)
PY
grep -c "\[TEXREP\]" ../logs/m3c_off.log
python -I ../../psprecomp/tools/tgadiff.py ../shots/m3b_base_a.tga ../shots/m3c_off.tga
```
Expected: `b'OK 0\n'`, `0` TEXREP lines, `differing pixels: 0 of 130560`.

- [ ] **Step 4: THE GATE — language menu and hideout keys.** Same boot with `PSPRECOMP_TEXTURES=C:/Users/torso/Documents/deco/PPSSPP/memstick/PSP/TEXTURES/INFN00001` and `PSPRECOMP_TEXTURES_DUMP=1` (log `m3c_gate_menu.log`), then the hideout with the existing tool:

```bash
cd /c/Users/torso/Documents/deco/build
python -I tools/to_square.py m3c_gate PSPRECOMP_TEXTURES=C:/Users/torso/Documents/deco/PPSSPP/memstick/PSP/TEXTURES/INFN00001 PSPRECOMP_TEXTURES_DUMP=1
taskkill //IM psprecomp_runtime.exe //F
for f in logs/m3c_gate_menu.log logs/m3c_gate.log; do
  echo "$f: $(grep -a '\[TEXREP\] pack' $f)"
  grep -a "^\[TEXREP\] key=" $f | awk '{print $NF}' | sort | uniq -c
  grep -a "^\[TEXREP\] key=" $f | awk '{print $4, $NF}' | sort | uniq -c | sort -rn | head -20
done
```
Expected: the pack line shows ~3060 textures; in the hideout log, `hit` >= `miss` (target from the spec: at least half). Record the counts, per format, in "Task 4 result" at the end of this file.

**If the gate fails** (few or no hits): do not start Task 5. Debug in this order, one hypothesis at a time (superpowers:systematic-debugging): (a) a non-CLUT format (fmt 0-3) key missing → data-hash byte count/bufw; take one missing key's texture address from `PSPRECOMP_TEX_WATCH` and compare the hashed byte count with the spec formula by hand; (b) only CLUT formats missing → CLUT snapshot/clutformat; check `LOADCLUT` happens before the draw and the `clutformat` xor; (c) 512-tall textures missing → max V. As an independent oracle, PPSSPP itself can dump the keys it computes: in `DECO/PPSSPP/memstick/PSP/SYSTEM/ppsspp.ini` set `SaveNewTextures = True` temporarily (restore `False` afterwards), run the same screen, and compare the new PNG names in `TEXTURES/INFN00001/new/` with our `[TEXREP] key=` lines.

- [ ] **Step 5: Commit** `feat(texrep): load PPSSPP packs from PSPRECOMP_TEXTURES and log replacement keys` and fill "Task 4 result".

### Task 5: Draw the replacements (stb_image PNG decode, upload, filtering, CPU cache)

**Files:**
- Create: `runtime/third_party/stb/stb_image.h`, `runtime/third_party/stb/README.txt`
- Create: `runtime/include/psp_texrep_png.h`, `runtime/src/psp_texrep_png.cpp`, `runtime/tests/test_texrep_png.cpp`
- Modify: `runtime/src/psp_texrep.cpp`, `runtime/include/psp_texrep.h`, `runtime/src/psp_ge_texture.cpp`, `runtime/include/psp_ge_texture.h`, `runtime/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 4 (`ge_texrep_lookup`, `ge_texrep_enabled`), `TexrepFilter` (Task 3).
- Produces:
  - `bool ge_texrep_decode_png(const uint8_t* data, size_t size, int* w, int* h, std::vector<uint8_t>* rgba);`
  - `struct GeTexrepImage { int w = 0, h = 0; const uint8_t* rgba = nullptr; TexrepFilter filter = TexrepFilter::None; };`
  - `bool ge_texrep_replacement(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v, GeTexrepImage* out);` (pointer valid until the next call)
  - `TexCacheEntry` gains `int forced_filter = 0;` (0 none, 1 nearest, 2 linear).

- [ ] **Step 1: Vendor stb_image.** Download the current `stb_image.h` and record its version line and SHA-256:

```bash
cd /c/Users/torso/Documents/deco/psprecomp
mkdir -p runtime/third_party/stb
C=$(curl -s "https://api.github.com/repos/nothings/stb/commits?path=stb_image.h&per_page=1" | python -I -c "import json,sys;print(json.load(sys.stdin)[0]['sha'])")
curl -sfL "https://raw.githubusercontent.com/nothings/stb/$C/stb_image.h" -o runtime/third_party/stb/stb_image.h
head -1 runtime/third_party/stb/stb_image.h; sha256sum runtime/third_party/stb/stb_image.h; echo $C
```
`runtime/third_party/stb/README.txt`:

```text
stb_image.h - public domain / MIT image loader by Sean Barrett (nothings/stb).
Used only to decode PNG texture-replacement images (STBI_ONLY_PNG).
Commit: <C printed above>   Version line: <first line printed above>
SHA-256: <printed above>
```

- [ ] **Step 2: Failing PNG test** `runtime/tests/test_texrep_png.cpp`:

```cpp
// ge_texrep_decode_png: PNG bytes -> RGBA8 rows (stb_image).
#include "psp_texrep_png.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

// 2x1 RGBA PNG: pixel 0 = (255,0,0,255), pixel 1 = (0,255,0,128).
static const uint8_t kPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xF4, 0x22, 0x7F,
    0x8A, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x9F, 0xE1, 0x3F, 0x43, 0x03, 0x00, 0x10, 0x79, 0x03, 0x7E, 0x21, 0xC0, 0xFD, 0x8D, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

int main() {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    CHECK(ge_texrep_decode_png(kPng, sizeof(kPng), &w, &h, &rgba), "valid PNG decodes");
    CHECK(w == 2 && h == 1 && rgba.size() == 8, "size 2x1 RGBA");
    CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255, "pixel 0");
    CHECK(rgba[4] == 0 && rgba[5] == 255 && rgba[6] == 0 && rgba[7] == 128, "pixel 1 keeps alpha");
    const uint8_t garbage[] = {0x89, 0x50, 0x4E, 0x47, 1, 2, 3};
    CHECK(!ge_texrep_decode_png(garbage, sizeof(garbage), &w, &h, &rgba), "corrupt PNG fails cleanly");
    CHECK(!ge_texrep_decode_png(nullptr, 0, &w, &h, &rgba), "empty input fails");
    if (failures == 0) std::printf("test_texrep_png: all passed\n");
    return failures == 0 ? 0 : 1;
}
```
CMake:

```cmake
add_executable(test_texrep_png tests/test_texrep_png.cpp src/psp_texrep_png.cpp)
target_include_directories(test_texrep_png PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_compile_options(test_texrep_png PRIVATE -O2)
add_test(NAME texrep_png_tests COMMAND test_texrep_png)
```
plus `src/psp_texrep_png.cpp` in `RUNTIME_SOURCES` and `${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb` in the runtime's include directories.

- [ ] **Step 3: Run** `cmake . && ninja test_texrep_png` → header missing.

- [ ] **Step 4: Implement** `runtime/include/psp_texrep_png.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

/// Decode PNG bytes to tightly packed RGBA8 rows (top row first). False on any
/// error (nothing is thrown, `rgba` is left unspecified).
bool ge_texrep_decode_png(const uint8_t* data, size_t size, int* w, int* h, std::vector<uint8_t>* rgba);
```
`runtime/src/psp_texrep_png.cpp`:

```cpp
#include "psp_texrep_png.h"

#include <cstring>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

bool ge_texrep_decode_png(const uint8_t* data, size_t size, int* w, int* h, std::vector<uint8_t>* rgba) {
    if (data == nullptr || size == 0 || size > 0x7FFFFFFF) return false;
    int comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), w, h, &comp, 4);
    if (px == nullptr) return false;
    rgba->resize(static_cast<size_t>(*w) * static_cast<size_t>(*h) * 4);
    std::memcpy(rgba->data(), px, rgba->size());
    stbi_image_free(px);
    return true;
}
```

- [ ] **Step 5: Run** `ninja test_texrep_png && ./test_texrep_png.exe` → `all passed`.

- [ ] **Step 6: The replacement path.** In `psp_texrep.cpp` add a CPU image cache (path → decoded image, LRU by bytes, 256 MiB budget; a failed decode is cached as an empty image so it is not retried) and:

```cpp
struct CachedImage {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // empty = failed decode
    uint64_t last_use = 0;
};
std::unordered_map<std::string, CachedImage> g_images;
size_t g_image_bytes = 0;
uint64_t g_use_clock = 0;
constexpr size_t kImageBudget = 256u << 20;

const CachedImage* load_image(const std::string& rel) {
    auto it = g_images.find(rel);
    if (it == g_images.end()) {
        CachedImage img;
        const auto t0 = std::chrono::steady_clock::now();
        std::ifstream f(std::filesystem::u8path(g_dir + "/" + rel), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!f.good() && !f.eof()) bytes.clear();
        if (bytes.empty() || !ge_texrep_decode_png(bytes.data(), bytes.size(), &img.w, &img.h, &img.rgba)) {
            img.rgba.clear();
            std::fprintf(stderr, "[TEXREP] cannot load %s (keeping the original texture)\n", rel.c_str());
        }
        if (g_dump) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "[TEXREP] loaded %s %dx%d in %.1f ms\n", rel.c_str(), img.w, img.h, ms);
        }
        g_image_bytes += img.rgba.size();
        it = g_images.emplace(rel, std::move(img)).first;
        while (g_image_bytes > kImageBudget && g_images.size() > 1) {  // evict LRU, never the new one
            auto victim = g_images.end();
            for (auto j = g_images.begin(); j != g_images.end(); ++j) {
                if (j->first != rel && (victim == g_images.end() || j->second.last_use < victim->second.last_use)) victim = j;
            }
            if (victim == g_images.end()) break;
            g_image_bytes -= victim->second.rgba.size();
            g_images.erase(victim);
        }
        it = g_images.find(rel);
    }
    it->second.last_use = ++g_use_clock;
    return it->second.rgba.empty() ? nullptr : &it->second;
}

bool ge_texrep_replacement(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v, GeTexrepImage* out) {
    if (!g_enabled) return false;
    GeTexrepKey key{};
    const TexrepFind f = ge_texrep_lookup(rdram, s, draw_max_v, &key);
    if (!f.found || f.ignored) return false;
    const CachedImage* img = load_image(f.path);
    if (!img) return false;
    out->w = img->w;
    out->h = img->h;
    out->rgba = img->rgba.data();
    out->filter = g_pack.filter(key);
    return true;
}
```
Declare `GeTexrepImage` and `ge_texrep_replacement` in `psp_texrep.h` (include `psp_texrep_pack.h`), add `<chrono> <iterator>` and `#include "psp_texrep_png.h"`.

In `psp_ge_texture.cpp`, replace the Task 4 lookup placeholder and the decode/upload with:

```cpp
    // Texture replacement (PSPRECOMP_TEXTURES): a pack PNG replaces the
    // decoded texture at its own size; UVs are normalised, so nothing else
    // changes. Otherwise decode from guest RAM as before.
    GeTexrepImage rep;
    const bool replaced = ge_texrep_enabled() && ge_texrep_replacement(rdram, state, draw_max_v, &rep);
    if (entry.gl_tex == 0) {
        glGenTextures(1, &entry.gl_tex);
    }
    glBindTexture(GL_TEXTURE_2D, entry.gl_tex);
    if (replaced) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rep.w, rep.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rep.rgba);
    } else {
        std::vector<uint8_t> rgba;
        if (!ge_decode_texture(rdram, params, rgba)) {
            /* existing magenta fill, unchanged */
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    entry.forced_filter = replaced ? static_cast<int>(rep.filter) : 0;
    apply_sampler(state, entry.forced_filter);
```
(keep the existing magenta loop body verbatim where the comment says so). `apply_sampler(const GeState&, int forced_filter)`: after the existing four `glTexParameteri`, add

```cpp
    if (forced_filter != 0) {  // pack [filtering]: 1 nearest, 2 linear
        const GLint f = forced_filter == 1 ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    }
```
and pass `entry.forced_filter` on the cache-hit path too. (`TexrepFilter` values are `None = 0, Nearest = 1, Linear = 2`, Task 3).

- [ ] **Step 7: Gates.** `ninja && ctest` (30/30). Off path again: Task 4 Step 3 commands with output `m3c_off2.tga` → `differing pixels: 0 of 130560`.

- [ ] **Step 8: See it.** Boot with the pack at scale 4 to the language menu and screenshot (`m3c_menu_s4.tga`); also run `to_square.py m3c_hideout PSPRECOMP_RENDER_SCALE=4 PSPRECOMP_TEXTURES=... PSPRECOMP_TEXTURES_DUMP=1` and take `S` of the hideout before SQUARE is processed (`to_square.py` saves shots under `shots/m3c_hideout/`). Convert with `shoot.tga2png` and open them with the Read tool next to the M3b scale-4 shot. Expected: sharper art where the log says `hit`, no magenta, no black or missing sprites, no `[TEXREP] cannot load` lines. Note the largest `loaded ... in N ms` values.

- [ ] **Step 9: Commit** `feat(texrep): draw PPSSPP pack replacements (stb_image PNG, [filtering])` with `runtime/third_party/stb` and all touched files.

### Task 6: User check with the pack (and conditional background decoding)

**Files:** `DECO/jugar.bat` (outside the repo). Conditional 6b: `runtime/src/psp_texrep.cpp`.

- [ ] **Step 1: Launcher.** In `DECO/jugar.bat`, after the `PSPRECOMP_RENDER_SCALE` line:

```bat
rem Texturas HD (pack de PPSSPP). Para desactivarlas, borra o comenta esta linea.
set PSPRECOMP_TEXTURES=%~dp0PPSSPP\memstick\PSP\TEXTURES\INFN00001
```

- [ ] **Step 2: Ask the user (Spanish)** to play: hideout (SQUARE menu, barracks), world map, one mission, window and F11. Ask about: textures that look wrong (wrong image, wrong colours, misaligned), anything missing, and stutter when entering a screen. After they close the game: `grep -a "\[TEXREP\]" DECO/build/logs/jugar.log | grep -v "key=" | head` and the fatal-error grep from M3b Task 1 Step 4.

- [ ] **Step 3: Record** "Task 6 result". If stutter is reported or any `loaded ... in` exceeds ~30 ms on screens the user plays, do 6b.

#### Task 6b (only if stutter): decode on a worker thread

- [ ] Move `load_image`'s file read + `ge_texrep_decode_png` into a single worker thread fed by a queue of paths (`std::thread` + `std::mutex` + `std::condition_variable`). `ge_texrep_replacement` returns false (draw the original) while the path is pending and enqueues it once; the next cache miss for that texture after the decode finishes uses it. Because the texture cache only re-asks on a miss, also bump a `g_ready_epoch` counter when an image finishes, and in `ge_texture_bind`'s cache-hit path re-run the replacement lookup for entries flagged `pending_replacement` when the epoch changed. Unit-test the queue logic separately if it is split into a pure class; verify with the user that the stutter is gone. Commit `perf(texrep): decode replacement PNGs on a worker thread`.

### Task 7: Docs, credits, handoff, push

**Files:** `README.md`, `docs/GRAPHICS.md`, `DEBUGGING.md`, `ARCHITECTURE.md` (only if it lists runtime subsystems that now miss texture replacement; check `grep -n "psp_ge_texture" ARCHITECTURE.md`), `DECO/build/autopilot/HANDOFF.md`.

- [ ] **Step 1: README.** Under the `PSPRECOMP_RENDER_SCALE` sentence: "`PSPRECOMP_TEXTURES=<dir>` loads a PPSSPP-format texture-replacement pack (`textures.ini` + PNGs, `xxh64`/`xxh32` hashes) from that folder; the pack is not part of this repository." In the credits section add: "Patapon 3 HD Texture Pack (<https://github.com/Lin-zl522/Patapon-3-HD-Texture-Pack>, downloaded separately): WallSoGB (Patapon3Textures lead), Shockturtle, owocek, efonte (AI upscaling), Keamble, Hozzzd, KnotSora, Lin, Olimp666, wondaoxigen, Rin Casi." and the vendored libraries "xxHash (Yann Collet, BSD-2), stb_image (Sean Barrett, public domain/MIT)".
- [ ] **Step 2: docs/GRAPHICS.md.** New subsection "Texture replacement" in the Texture Pipeline section: the three units, the key (link the spec), where it hooks (`ge_texture_bind` cache miss), the `LOADCLUT` snapshot, `draw_max_v`, the CPU image cache, `[filtering]`, what cannot be replaced (render targets, video), and the off-path guarantee. Update the `GE_CMD_LOADCLUT` mention if the doc calls it a no-op.
- [ ] **Step 3: DEBUGGING.md.** Env list: "`PSPRECOMP_TEXTURES=<dir>`: PPSSPP texture pack folder. `PSPRECOMP_TEXTURES_DUMP=1`: one `[TEXREP] key=<24 hex> WxH fmt=N hit|miss|ignored` line per distinct key, plus PNG load times. A miss on a texture the pack has means our key differs: compare with PPSSPP's own dump (`SaveNewTextures = True` in its ppsspp.ini)."
- [ ] **Step 4: Gates and push.**

```bash
cd /c/Users/torso/Documents/deco/build/rtw && ninja && ctest 2>&1 | tail -2
cd /c/Users/torso/Documents/deco/psprecomp && cargo test -q 2>&1 | grep -c "FAILED\|panicked"
git add README.md docs/GRAPHICS.md DEBUGGING.md docs/superpowers/plans/2026-10-10-m3c-hd-textures.md
git commit -m "docs: texture replacement (PSPRECOMP_TEXTURES), pack credits"
git push p3 patapon3-windows:main
git ls-remote p3 refs/heads/main   # must equal git rev-parse HEAD
```
Expected: all ctest pass, `0` cargo failures, remote = HEAD.
- [ ] **Step 5: HANDOFF.md:** mark M3c done (what the user verified, hit rate, jugar.bat settings), add a log line, and set the next item: PPSSPP-vs-runtime screenshot pairs (runtime at scale 4 with the pack) to order the remaining graphics bugs.

---

## Task 4 result

(fill in: date, pack line, hit/miss/ignored counts for the menu and the hideout, per-format split, anything investigated)

## Task 6 result

(fill in: user findings, load times, whether 6b was needed)
