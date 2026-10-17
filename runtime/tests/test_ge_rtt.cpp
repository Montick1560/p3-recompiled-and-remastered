// Unit tests for the render-to-texture helpers (psp_ge_rtt.h): which GE render target a
// texture samples (PPSSPP TextureCacheCommon.cpp MatchFramebuffer, colour channel, no CLUT),
// which target a byte span overlaps, and the GL blit rect of a texel window.

#include "psp_ge_rtt.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static void find_rules() {
    const GeRttTarget t[3] = {
        {true, 0x000000, 512, 3},  // 0x04000000, 8888
        {true, 0x088000, 512, 3},  // 0x04088000, 8888
        {true, 0x110000, 256, 1},  // 0x04110000, 5551, stride 256
    };
    GeRttHit h = ge_rtt_find(t, 3, 0x04088000, 3, 512, 256);
    CHECK(h.index == 1 && h.x == 0 && h.y == 0, "texture at a buffer base samples that buffer");
    h = ge_rtt_find(t, 3, 0x04088000 + (10 * 512 + 20) * 4, 3, 512, 64);
    CHECK(h.index == 1 && h.x == 20 && h.y == 10, "texture inside a buffer gets its texel origin");
    CHECK(ge_rtt_find(t, 3, 0x44088000, 3, 512, 256).index == 1, "uncached VRAM alias matches");
    CHECK(ge_rtt_find(t, 3, 0x04288000, 3, 512, 256).index == 1, "VRAM mirror at +2 MB matches");
    h = ge_rtt_find(t, 3, 0x04110000 + (5 * 256 + 7) * 2, 1, 256, 64);
    CHECK(h.index == 2 && h.x == 7 && h.y == 5, "16-bit texture on a 16-bit buffer uses its stride");
    CHECK(ge_rtt_find(t, 3, 0x04110000 + 64, 0, 256, 64).index == 2,
          "5650 texture on a 5551 buffer matches (same texel size)");
    CHECK(ge_rtt_find(t, 3, 0x04110000, 3, 256, 64).index == -1, "8888 texture over a 16-bit buffer: no");
    CHECK(ge_rtt_find(t, 3, 0x04088000, 5, 512, 256).index == -1, "CLUT8 never matches (no depal)");
    CHECK(ge_rtt_find(t, 3, 0x04088000, 8, 512, 256).index == -1, "DXT1 never matches");
    CHECK(ge_rtt_find(t, 3, 0x08A00000, 3, 512, 256).index == -1, "main-RAM texture never matches");
    CHECK(ge_rtt_find(t, 3, 0x04088000, 3, 256, 256).index == -1,
          "stride mismatch: texture bufw 256 on a 512 buffer does not match (PPSSPP)");
    CHECK(ge_rtt_find(t, 3, 0x04088000 + 40 * 4, 3, 64, 1).index == 1,
          "one-row texture ignores the stride (PPSSPP texHeight > 1 rule)");
    CHECK(ge_rtt_find(t, 3, 0x04088000 + (200 * 512) * 4, 3, 512, 512).index == -1,
          "y offset + texHeight/4 past the buffer height does not match");
    CHECK(ge_rtt_find(t, 3, 0x04088000, 3, 512, 512).index == 1,
          "512-high texture at the base of a 272-line buffer matches (lenient, PPSSPP)");
    CHECK(ge_rtt_find(t, 3, 0x04000000 + 490 * 4, 3, 512, 1).index == -1,
          "texture right of the 480-pixel window does not match");
    h = ge_rtt_find(t, 3, 0x04000000 + (271 * 512 + 479) * 4, 3, 512, 1);
    CHECK(h.index == 0 && h.x == 479 && h.y == 271, "last visible texel matches");
    const GeRttTarget high[1] = {{true, 0x120000, 512, 3}};
    CHECK(ge_rtt_find(high, 1, 0x04120000 + (40 * 512) * 4, 3, 512, 16).index == -1,
          "y offset > 32 into a buffer above 0x04110000 is ignored (PPSSPP safety rule)");
    const GeRttTarget empty[1] = {{false, 0x088000, 512, 3}};
    CHECK(ge_rtt_find(empty, 1, 0x04088000, 3, 512, 256).index == -1, "slot without GL content: no");
    const GeRttTarget nostride[1] = {{true, 0x088000, 0, 3}};
    CHECK(ge_rtt_find(nostride, 1, 0x04088000, 3, 512, 256).index == -1, "stride 0: no (no divide)");
}

static void overlap_rules() {
    const GeRttTarget t[2] = {
        {true, 0x000000, 512, 1},  // 16-bit: 512*2*272 = 0x44000 bytes
        {true, 0x044000, 512, 1},
    };
    CHECK(ge_rtt_overlap(t, 2, 0x04000000, 64) == 0, "span at a target base overlaps it");
    CHECK(ge_rtt_overlap(t, 2, 0x04043FF0, 16) == 0, "last bytes of target 0 overlap it");
    CHECK(ge_rtt_overlap(t, 2, 0x04044000, 16) == 1, "first byte of target 1 overlaps target 1");
    CHECK(ge_rtt_overlap(t, 2, 0x04088000, 0x1000) == -1, "span past both targets: none");
    CHECK(ge_rtt_overlap(t, 2, 0x44044000, 16) == 1, "uncached alias overlaps");
    CHECK(ge_rtt_overlap(t, 2, 0x04244000, 16) == 1, "+2 MB mirror overlaps");
    CHECK(ge_rtt_overlap(t, 2, 0x08800000, 0x100000) == -1, "main RAM never overlaps");
    CHECK(ge_rtt_overlap(t, 2, 0x04000000, 0) == -1, "empty span never overlaps");
    const GeRttTarget off[1] = {{false, 0x000000, 512, 1}};
    CHECK(ge_rtt_overlap(off, 1, 0x04000000, 64) == -1, "invalid target never overlaps");
}

static void tex_bytes_rules() {
    CHECK(ge_rtt_tex_bytes(1, 512, 272) == 512u * 272 * 2, "5551: 2 bytes per texel");
    CHECK(ge_rtt_tex_bytes(3, 512, 272) == 512u * 272 * 4, "8888: 4 bytes per texel");
    CHECK(ge_rtt_tex_bytes(4, 128, 64) == 128u * 64 / 2, "CLUT4: half a byte per texel");
    CHECK(ge_rtt_tex_bytes(5, 128, 64) == 128u * 64, "CLUT8: one byte per texel");
    CHECK(ge_rtt_tex_bytes(8, 64, 64) == 64u * 64 / 2, "DXT1: 4 bits per texel");
    CHECK(ge_rtt_tex_bytes(11, 64, 64) == 0, "unknown format: no span");
    CHECK(ge_rtt_tex_bytes(3, 0, 1) == 4, "bufw 0 counts as one texel");
}

static void blit_rules() {
    GeRttBlit b = ge_rtt_blit_rect(0, 0, 64, 64, 1);
    CHECK(!b.empty && b.src_x0 == 0 && b.src_x1 == 64 && b.src_y0 == 272 && b.src_y1 == 208 &&
              b.dst_w == 64 && b.dst_h == 64,
          "top-left 64x64: GL rows 272..208 flipped so texture row 0 is PSP row 0");
    b = ge_rtt_blit_rect(460, 250, 64, 64, 1);
    CHECK(!b.empty && b.src_x0 == 460 && b.src_x1 == 480 && b.src_y0 == 22 && b.src_y1 == 0 &&
              b.dst_w == 20 && b.dst_h == 22,
          "window clipped to the 480x272 target (outside texels stay cleared)");
    b = ge_rtt_blit_rect(10, 20, 32, 16, 4);
    CHECK(!b.empty && b.src_x0 == 40 && b.src_x1 == 168 && b.src_y0 == 1008 && b.src_y1 == 944 &&
              b.dst_w == 128 && b.dst_h == 64,
          "render scale 4 multiplies every rect value");
    CHECK(ge_rtt_blit_rect(480, 0, 16, 16, 1).empty, "window right of the target: empty");
    CHECK(ge_rtt_blit_rect(0, 272, 16, 16, 1).empty, "window below the target: empty");
    CHECK(ge_rtt_blit_rect(0, 0, 16, 16, 0).empty, "scale 0: empty");
}

int main() {
    find_rules();
    overlap_rules();
    tex_bytes_rules();
    blit_rules();
    if (failures == 0) std::printf("test_ge_rtt: all passed\n");
    return failures == 0 ? 0 : 1;
}
