// ge_stencil_state: GE stencil registers -> GL stencil state, a port of PPSSPP's
// ConvertStencilFuncState / ConvertStencilFunc5551 (tools/ppsspp-src
// GPU/Common/GPUStateUtils.cpp). Header-only, no GL.
#include "psp_ge_stencil.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

// GE register words: STENCILTEST = func | ref << 8 | mask << 16,
// STENCILOP = sfail | zfail << 8 | zpass << 16.
static uint32_t test_word(int func, int ref, int mask) {
    return static_cast<uint32_t>(func | (ref << 8) | (mask << 16));
}
static uint32_t op_word(int sfail, int zfail, int zpass) {
    return static_cast<uint32_t>(sfail | (zfail << 8) | (zpass << 16));
}

static GeStencilInput input(int fmt) {
    GeStencilInput in;
    in.test_enable = true;
    in.stencil_test = test_word(GE_COMP_ALWAYS, 0, 0xFF);
    in.stencil_op = op_word(GE_STENCILOP_KEEP, GE_STENCILOP_KEEP, GE_STENCILOP_KEEP);
    in.mask_rgb = 0;
    in.mask_alpha = 0;
    in.depth_test = false;
    in.depth_write = false;
    in.fb_format = fmt;
    return in;
}

// ---- Register decode (GPUState.h:274-279) and the write mask (:1406) --------

static void decodes_8888_as_is() {
    GeStencilInput in = input(GE_FORMAT_8888);
    in.stencil_test = test_word(GE_COMP_GEQUAL, 0x42, 0x7F);
    in.stencil_op = op_word(GE_STENCILOP_ZERO, GE_STENCILOP_INVERT, GE_STENCILOP_INCR);
    in.mask_alpha = 0x0F;
    in.depth_test = true;
    const GeStencilState s = ge_stencil_state(in);
    CHECK(s.enabled, "8888: enabled");
    CHECK(s.func == GE_COMP_GEQUAL, "8888: func kept");
    CHECK(s.ref == 0x42 && s.test_mask == 0x7F, "8888: ref/mask decoded");
    CHECK(s.sfail == GE_STENCILOP_ZERO && s.zfail == GE_STENCILOP_INVERT
          && s.zpass == GE_STENCILOP_INCR, "8888: ops decoded");
    CHECK(s.write_mask == 0xF0, "8888: write mask = ~MASKALPHA");
}

static void disabled_keeps_write_mask_for_clears() {
    // :1409 "Ignore enabled, used for clears too."
    GeStencilInput in = input(GE_FORMAT_8888);
    in.test_enable = false;
    in.mask_alpha = 0x80;
    const GeStencilState s = ge_stencil_state(in);
    CHECK(!s.enabled, "disabled");
    CHECK(s.write_mask == 0x7F, "disabled 8888: write mask still ~MASKALPHA");

    in.fb_format = GE_FORMAT_5551;
    CHECK(ge_stencil_state(in).write_mask == 0x00, "disabled 5551: mask 0x7F -> 0x00 (:1402)");
    in.mask_alpha = 0x7F;
    CHECK(ge_stencil_state(in).write_mask == 0xFF, "disabled 5551: mask 0x80 -> 0xFF (:1402)");
}

// ---- Write-only-stencil case (:1424-1431) ------------------------------------

static void write_only_stencil_forces_always() {
    GeStencilInput in = input(GE_FORMAT_8888);
    in.stencil_test = test_word(GE_COMP_EQUAL, 3, 0xFF);
    in.stencil_op = op_word(GE_STENCILOP_REPLACE, GE_STENCILOP_REPLACE, GE_STENCILOP_REPLACE);
    in.mask_rgb = 0xFFFFFF;
    CHECK(ge_stencil_state(in).func == GE_COMP_ALWAYS, "RGB masked, all REPLACE -> ALWAYS");

    in.mask_rgb = 0xFFFF00;
    CHECK(ge_stencil_state(in).func == GE_COMP_EQUAL, "RGB partly written: test kept");

    in.mask_rgb = 0xFFFFFF;
    in.depth_test = true;
    in.depth_write = true;
    CHECK(ge_stencil_state(in).func == GE_COMP_EQUAL, "depth test + write: test kept");

    in.depth_write = false;
    CHECK(ge_stencil_state(in).func == GE_COMP_ALWAYS, "depth test without write -> ALWAYS");

    in.stencil_op = op_word(GE_STENCILOP_REPLACE, GE_STENCILOP_KEEP, GE_STENCILOP_REPLACE);
    CHECK(ge_stencil_state(in).func == GE_COMP_EQUAL, "depth test on, zfail differs: kept");
    in.depth_test = false;
    CHECK(ge_stencil_state(in).func == GE_COMP_ALWAYS, "no depth test: zfail ignored -> ALWAYS");
}

// ---- Per-format tweaks (:1433-1446) -------------------------------------------

static void format_565_never_writes() {
    GeStencilInput in = input(GE_FORMAT_565);
    in.stencil_op = op_word(GE_STENCILOP_KEEP, GE_STENCILOP_KEEP, GE_STENCILOP_REPLACE);
    CHECK(ge_stencil_state(in).write_mask == 0, "565: write mask 0");
}

static void format_4444_as_8888() {
    GeStencilInput in = input(GE_FORMAT_4444);
    in.stencil_test = test_word(GE_COMP_LESS, 0x30, 0xF0);
    const GeStencilState s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_LESS && s.ref == 0x30 && s.test_mask == 0xF0, "4444 unchanged");
    CHECK(s.write_mask == 0xFF, "4444 write mask ~0");
}

// ---- 5551: one bit of stencil, any non-zero value reads as 0xFF (:1303) -------

static void s5551_fever_worm_pair() {
    // Patapon 3 FEVER worm: the silhouette zeroes the stencil, the fire draws
    // where it is zero (STENCILTEST 0xFF0001 / STENCILOP 0x010000, then 0xFF0002).
    GeStencilInput in = input(GE_FORMAT_5551);
    in.stencil_test = 0xFF0001;
    in.stencil_op = 0x010000;
    GeStencilState s = ge_stencil_state(in);
    CHECK(s.enabled && s.func == GE_COMP_ALWAYS, "worm: ALWAYS");
    CHECK(s.zpass == GE_STENCILOP_ZERO && s.sfail == GE_STENCILOP_KEEP
          && s.zfail == GE_STENCILOP_KEEP, "worm: zpass ZERO");
    CHECK(s.write_mask == 0xFF, "worm: writes");

    in.stencil_test = 0xFF0002;
    in.stencil_op = 0;
    s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_EQUAL && s.ref == 0 && s.test_mask == 0xFF, "fire: EQUAL 0");
    CHECK(s.zpass == GE_STENCILOP_KEEP, "fire: no writes");
}

static void s5551_equal_and_notequal() {
    GeStencilInput in = input(GE_FORMAT_5551);
    in.stencil_test = test_word(GE_COMP_EQUAL, 0x00, 0x0F);
    GeStencilState s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_EQUAL && s.ref == 0 && s.test_mask == 0xFF, "EQUAL 0: mask dropped");

    in.stencil_test = test_word(GE_COMP_EQUAL, 0xFF, 0xFF);
    s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_NOTEQUAL && s.ref == 0, "EQUAL 255 -> NOTEQUAL 0");

    in.stencil_test = test_word(GE_COMP_EQUAL, 0x40, 0xFF);
    s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_NEVER && s.ref == 0xFF, "EQUAL other -> NEVER, unused ref 0xFF");

    in.stencil_test = test_word(GE_COMP_NOTEQUAL, 0x00, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_NOTEQUAL, "NOTEQUAL 0 kept");
    in.stencil_test = test_word(GE_COMP_NOTEQUAL, 0xFF, 0xFF);
    s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_EQUAL && s.ref == 0, "NOTEQUAL 255 -> EQUAL 0");
    in.stencil_test = test_word(GE_COMP_NOTEQUAL, 0x40, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_ALWAYS, "NOTEQUAL other -> ALWAYS");
}

static void s5551_ordered_compares() {
    GeStencilInput in = input(GE_FORMAT_5551);
    in.stencil_test = test_word(GE_COMP_LESS, 0xFF, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_NEVER, "LESS 255 -> NEVER");
    in.stencil_test = test_word(GE_COMP_LESS, 0x10, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_NOTEQUAL, "LESS x -> NOTEQUAL 0");

    in.stencil_test = test_word(GE_COMP_LEQUAL, 0x00, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_ALWAYS, "LEQUAL 0 -> ALWAYS");
    in.stencil_test = test_word(GE_COMP_LEQUAL, 0x10, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_NOTEQUAL, "LEQUAL x -> NOTEQUAL 0");

    in.stencil_test = test_word(GE_COMP_GREATER, 0x10, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_EQUAL, "GREATER x -> EQUAL 0");
    in.stencil_test = test_word(GE_COMP_GREATER, 0x00, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_NEVER, "GREATER 0 -> NEVER");

    in.stencil_test = test_word(GE_COMP_GEQUAL, 0xFF, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_ALWAYS, "GEQUAL 255 -> ALWAYS");
    in.stencil_test = test_word(GE_COMP_GEQUAL, 0x10, 0xFF);
    CHECK(ge_stencil_state(in).func == GE_COMP_EQUAL, "GEQUAL x -> EQUAL 0");
}

static void s5551_rewrite_keeps_replace_ref() {
    // rewriteFunc only applies when the ref is unused or already the 1-bit value.
    GeStencilInput in = input(GE_FORMAT_5551);
    in.stencil_test = test_word(GE_COMP_GREATER, 0x10, 0xFF);
    in.stencil_op = op_word(GE_STENCILOP_KEEP, GE_STENCILOP_KEEP, GE_STENCILOP_REPLACE);
    in.depth_test = true;
    GeStencilState s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_EQUAL && s.ref == 0, "REPLACE ref 0x10 -> 1-bit 0x00 matches: rewritten");

    in.stencil_test = test_word(GE_COMP_GREATER, 0x90, 0xFF);
    s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_GREATER && s.ref == 0x90, "REPLACE ref 0x90 (1-bit 0xFF): not rewritten");

    // rewriteRef with REPLACE: the ref becomes the 1-bit value.
    in.stencil_test = test_word(GE_COMP_EQUAL, 0x90, 0xFF);
    s = ge_stencil_state(in);
    CHECK(s.func == GE_COMP_NEVER && s.ref == 0xFF, "NEVER keeps REPLACE ref as 0xFF");
}

static void s5551_op_rewrites() {
    GeStencilInput in = input(GE_FORMAT_5551);
    in.depth_test = true;
    in.stencil_op = op_word(GE_STENCILOP_DECR, GE_STENCILOP_KEEP, GE_STENCILOP_DECR);
    GeStencilState s = ge_stencil_state(in);
    CHECK(s.sfail == GE_STENCILOP_ZERO && s.zpass == GE_STENCILOP_ZERO, "DECR -> ZERO");

    in.stencil_test = test_word(GE_COMP_NOTEQUAL, 0, 0xFF);
    in.stencil_op = op_word(GE_STENCILOP_KEEP, GE_STENCILOP_KEEP, GE_STENCILOP_INVERT);
    CHECK(ge_stencil_state(in).zpass == GE_STENCILOP_ZERO, "NOTEQUAL 0: INVERT -> ZERO");

    in.stencil_test = test_word(GE_COMP_EQUAL, 0, 0xFF);
    in.stencil_op = op_word(GE_STENCILOP_KEEP, GE_STENCILOP_KEEP, GE_STENCILOP_INCR);
    CHECK(ge_stencil_state(in).zpass == GE_STENCILOP_INVERT, "EQUAL 0: INCR -> INVERT");

    // ALWAYS without REPLACE: rewriteRef sets ref 0xFF, then INCR -> REPLACE.
    in.stencil_test = test_word(GE_COMP_ALWAYS, 0, 0xFF);
    s = ge_stencil_state(in);
    CHECK(s.ref == 0xFF && s.zpass == GE_STENCILOP_REPLACE, "ALWAYS: INCR -> REPLACE 0xFF");
}

static void s5551_write_mask_one_bit() {
    GeStencilInput in = input(GE_FORMAT_5551);
    in.mask_alpha = 0x7F;  // write mask 0x80 -> top bit writable
    CHECK(ge_stencil_state(in).write_mask == 0xFF, "5551 enabled: 0x80 -> 0xFF");
    in.mask_alpha = 0x80;
    CHECK(ge_stencil_state(in).write_mask == 0x00, "5551 enabled: 0x7F -> 0x00");
}

// ---- Clear mode (StateMappingGLES.cpp:237, SoftwareTransformCommon.cpp:711) ----

static void clear_value_and_mask() {
    GeStencilInput in = input(GE_FORMAT_8888);
    in.test_enable = false;
    GeStencilClear c = ge_stencil_clear(in, 0x02, 0x5A);
    CHECK(c.clear && c.value == 0x5A && c.write_mask == 0xFF, "alpha flag: clear to vertex alpha");
    c = ge_stencil_clear(in, 0x05, 0x5A);
    CHECK(!c.clear, "no alpha flag: stencil kept");
    in.mask_alpha = 0x0F;
    c = ge_stencil_clear(in, 0x07, 0xFF);
    CHECK(c.clear && c.write_mask == 0xF0, "MASKALPHA applies to clears");
    in.fb_format = GE_FORMAT_5551;
    in.mask_alpha = 0x80;
    c = ge_stencil_clear(in, 0x07, 0xFF);
    CHECK(c.clear && c.write_mask == 0x00, "5551 clear: 1-bit write mask");
}

int main() {
    decodes_8888_as_is();
    disabled_keeps_write_mask_for_clears();
    write_only_stencil_forces_always();
    format_565_never_writes();
    format_4444_as_8888();
    s5551_fever_worm_pair();
    s5551_equal_and_notequal();
    s5551_ordered_compares();
    s5551_rewrite_keeps_replace_ref();
    s5551_op_rewrites();
    s5551_write_mask_one_bit();
    clear_value_and_mask();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ge_stencil: all tests passed\n");
    return 0;
}
