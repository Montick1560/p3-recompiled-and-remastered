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
    // PPSSPP xors gstate.clutformat, the whole command word: 0xC5 << 24 | data.
    CHECK(ge_texrep_cluthash(t, fmt_start1) == (XXH32(t.buf, 64, 0xC0108888u) ^ 0xC5000000u ^ fmt_start1),
          "min(total + base, max) = 64, command byte xored in");
    const uint32_t fmt_plain = 0x03;  // 32-bit palette, start 0
    ge_clut_snapshot_load(t, src.data(), 32);
    CHECK(ge_texrep_cluthash(t, fmt_plain) == (XXH32(t.buf, 32, 0xC0108888u) ^ 0xC5000000u ^ fmt_plain),
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

int main() {
    xxhash_vectors();
    bpp_and_bufw();
    max_seen_v_rules();
    contiguous();
    strided_reduce();
    odd_reduce();
    cachekey_and_name();
    clut_snapshot();
    params_from_registers();
    if (failures == 0) std::printf("test_texrep_hash: all passed\n");
    return failures == 0 ? 0 : 1;
}
