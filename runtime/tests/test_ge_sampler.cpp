// Unit tests for the GE sampler choice (psp_ge_sampler.h): TEXFILTER /
// TEXWRAP register bits -> GL filter and wrap, plus the PSPRECOMP_TEX_FILTER
// override. PPSSPP default-settings semantics.

#include "psp_ge_sampler.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    // 1. All-off registers: nearest filters, repeat wrap.
    GeSampler a = ge_sampler_from_regs(0x000, 0x000, GeFilterOverride::Auto);
    CHECK(!a.min_linear, "0x000/0x000 auto: min nearest");
    CHECK(!a.mag_linear, "0x000/0x000 auto: mag nearest");
    CHECK(!a.clamp_s, "0x000/0x000 auto: U repeat");
    CHECK(!a.clamp_t, "0x000/0x000 auto: V repeat");

    // 2. Both filter bits and both clamp bits set.
    GeSampler b = ge_sampler_from_regs(0x101, 0x101, GeFilterOverride::Auto);
    CHECK(b.min_linear, "0x101/0x101 auto: min linear");
    CHECK(b.mag_linear, "0x101/0x101 auto: mag linear");
    CHECK(b.clamp_s, "0x101/0x101 auto: U clamp");
    CHECK(b.clamp_t, "0x101/0x101 auto: V clamp");

    // 3. Only the minify bit.
    GeSampler c = ge_sampler_from_regs(0x001, 0x000, GeFilterOverride::Auto);
    CHECK(c.min_linear, "0x001/0x000 auto: min linear");
    CHECK(!c.mag_linear, "0x001/0x000 auto: mag nearest");

    // 4. Only the magnify bit; U clamp only.
    GeSampler d = ge_sampler_from_regs(0x100, 0x001, GeFilterOverride::Auto);
    CHECK(!d.min_linear, "0x100/0x001 auto: min nearest");
    CHECK(d.mag_linear, "0x100/0x001 auto: mag linear");
    CHECK(d.clamp_s, "0x100/0x001 auto: U clamp");
    CHECK(!d.clamp_t, "0x100/0x001 auto: V repeat");

    // 5. Mipmap bits ignored; V clamp only.
    GeSampler e = ge_sampler_from_regs(0x007, 0x100, GeFilterOverride::Auto);
    CHECK(e.min_linear, "0x007/0x100 auto: min linear (mip bits ignored)");
    CHECK(!e.clamp_s, "0x007/0x100 auto: U repeat");
    CHECK(e.clamp_t, "0x007/0x100 auto: V clamp");

    // 6. Mipmap bits without the minify bit stay nearest.
    GeSampler f = ge_sampler_from_regs(0x006, 0, GeFilterOverride::Auto);
    CHECK(!f.min_linear, "0x006/0x000 auto: min nearest (bit 0 clear)");

    // 7. Nearest override forces both filters off, wrap untouched.
    GeSampler g = ge_sampler_from_regs(0x101, 0x101, GeFilterOverride::Nearest);
    CHECK(!g.min_linear, "0x101/0x101 nearest: min nearest");
    CHECK(!g.mag_linear, "0x101/0x101 nearest: mag nearest");
    CHECK(g.clamp_s, "0x101/0x101 nearest: U clamp preserved");
    CHECK(g.clamp_t, "0x101/0x101 nearest: V clamp preserved");

    // 8. Linear override forces both filters on, wrap untouched.
    GeSampler h = ge_sampler_from_regs(0x000, 0x000, GeFilterOverride::Linear);
    CHECK(h.min_linear, "0x000/0x000 linear: min linear");
    CHECK(h.mag_linear, "0x000/0x000 linear: mag linear");
    CHECK(!h.clamp_s, "0x000/0x000 linear: U repeat preserved");
    CHECK(!h.clamp_t, "0x000/0x000 linear: V repeat preserved");

    // 9. Override parsing.
    CHECK(ge_filter_override_parse(nullptr) == GeFilterOverride::Auto, "parse nullptr -> auto");
    CHECK(ge_filter_override_parse("") == GeFilterOverride::Auto, "parse empty -> auto");
    CHECK(ge_filter_override_parse("auto") == GeFilterOverride::Auto, "parse auto -> auto");
    CHECK(ge_filter_override_parse("AUTO?") == GeFilterOverride::Auto, "parse AUTO? -> auto");
    CHECK(ge_filter_override_parse("nearest") == GeFilterOverride::Nearest, "parse nearest");
    CHECK(ge_filter_override_parse("linear") == GeFilterOverride::Linear, "parse linear");

    if (failures) { std::fprintf(stderr, "SOME TESTS FAILED\n"); return 1; }
    std::printf("test_ge_sampler: all passed\n");
    return 0;
}
