// Unit tests for psp_ge_texpatch: the texture patch seam (registered patchers recognise a texture by
// content once per content hash; their variant is latched per display list).
#include "psp_ge_texpatch.h"
#include "psp_ge_constants.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static int g_recognise_calls = 0, g_variant_calls = 0, g_apply_calls = 0;
static uint32_t g_device = 1;          // what the fake's variant() reports right now
static uint8_t g_marker = 0x5A;        // the fake recognises textures whose first texel red == marker
static uint32_t g_applied_variant = 0;

static bool cand_8888(const GeTexDecodeParams& p) { return p.format == GE_TFMT_8888 && p.width == 16; }
static bool recog(const GeTexDecodeParams&, const uint8_t* rgba) {
    g_recognise_calls++;
    return rgba[0] == g_marker;
}
static uint32_t variant_now() {
    g_variant_calls++;
    return g_device;
}
static void apply_fake(uint32_t v, const GeTexDecodeParams&, RgbaImage& img) {
    g_apply_calls++;
    g_applied_variant = v;
    img.px[0] = 0xEE;
}
static bool cand_none(const GeTexDecodeParams&) { return false; }

static std::vector<uint8_t> g_ram(0x10000, 0);
static GeTexDecodeParams tex_at(uint32_t addr, uint8_t first_red) {
    g_ram[addr] = first_red;
    GeTexDecodeParams p{};
    p.addr = addr;
    p.width = 16;
    p.height = 16;
    p.bufw = 16;
    p.format = GE_TFMT_8888;
    return p;
}

int main() {
    ge_texpatch_register({cand_8888, recog, variant_now, apply_fake});
    ge_texpatch_register({cand_none, recog, variant_now, apply_fake});
    ge_texpatch_begin_list();
    CHECK(g_variant_calls == 2, "variant read once per patcher at list start");

    const GeTexDecodeParams p = tex_at(0x100, 0x5A);
    const uint32_t v = ge_texpatch_variant(g_ram.data(), p, 0x1234);
    CHECK(v != 0 && (v & 0x0FFFFFFFu) == 1, "recognised: latched variant 1");
    CHECK((v >> 28) == 1, "first patcher's top bits");
    CHECK(ge_texpatch_variant(g_ram.data(), p, 0x1234) == v, "same hash, same answer");
    CHECK(g_recognise_calls == 1, "recognise once per content hash");

    g_device = 2;  // the device changes in the middle of a list
    CHECK(ge_texpatch_variant(g_ram.data(), p, 0x1234) == v, "mid-list change is not seen");
    ge_texpatch_begin_list();
    CHECK((ge_texpatch_variant(g_ram.data(), p, 0x1234) & 0x0FFFFFFFu) == 2, "next list sees it");

    const GeTexDecodeParams q = tex_at(0x2000, 0x11);
    CHECK(ge_texpatch_variant(g_ram.data(), q, 0x9999) == 0, "not recognised -> 0");
    CHECK(g_recognise_calls == 2, "one more decode for the new hash");

    GeTexDecodeParams other = p;
    other.width = 32;
    const int before = g_recognise_calls;
    CHECK(ge_texpatch_variant(g_ram.data(), other, 0x7777) == 0, "not a candidate -> 0");
    CHECK(g_recognise_calls == before, "no decode for a non-candidate");

    g_device = 0;
    ge_texpatch_begin_list();
    CHECK(ge_texpatch_variant(g_ram.data(), p, 0x1234) == 0, "latched variant 0 = do not patch");

    g_device = 3;
    ge_texpatch_begin_list();
    const uint32_t v3 = ge_texpatch_variant(g_ram.data(), p, 0x1234);
    RgbaImage img;
    img.w = img.h = 1;
    img.px = {0, 0, 0, 0};
    ge_texpatch_apply(v3, p, img);
    CHECK(g_apply_calls == 1 && g_applied_variant == 3 && img.px[0] == 0xEE, "apply reaches its patcher");

    ge_texpatch_set_scale(8);
    CHECK(ge_texpatch_scale() == 4, "compose scale clamped to 4");
    ge_texpatch_set_scale(0);
    CHECK(ge_texpatch_scale() == 1, "at least 1");

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
