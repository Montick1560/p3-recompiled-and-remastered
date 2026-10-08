// Unit tests for the pure GE texture decoder (psp_ge_texdecode.cpp):
// buffer-width row stride, swizzled layout, CLUT index transform.
// Oracle: PPSSPP GPU/Common/TextureDecoder + GPUState::transformClutIndex.
// Standalone executable: no SDL/GL.

#include "psp_ge_texdecode.h"
#include "psp_ge_constants.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_U32_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        if (static_cast<uint32_t>(actual) != static_cast<uint32_t>(expected)) { \
            std::fprintf(stderr, "FAIL: %s: got 0x%X, expected 0x%X\n", msg, \
                static_cast<uint32_t>(actual), static_cast<uint32_t>(expected)); \
            failures++; \
        } \
    } while (0)

static constexpr uint32_t TEX = 0x04100000U;
static constexpr uint32_t CLUT = 0x04200000U;

// 32-bit palette whose entry i has red = i, alpha = 0xFF.
static void write_palette(std::vector<uint8_t>& ram) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t v = 0xFF000000U | i;
        std::memcpy(&ram[(CLUT & 0x07FFFFFFU) + i * 4], &v, 4);
    }
}

static GeTexDecodeParams params(int fmt, int w, int h, int bufw) {
    GeTexDecodeParams p{};
    p.addr = TEX; p.width = w; p.height = h; p.bufw = bufw; p.format = fmt;
    p.swizzle = false; p.clut_addr = CLUT; p.clut_format = GE_CMODE_32BIT_ABGR8888;
    p.clut_shift = 0; p.clut_mask = 0xFF; p.clut_start = 0;
    return p;
}

static uint8_t red_at(const std::vector<uint8_t>& rgba, int w, int x, int y) {
    return rgba[(y * w + x) * 4];
}

static void test_clut8_uses_buffer_width(std::vector<uint8_t>& ram) {
    uint8_t* t = &ram[TEX & 0x07FFFFFFU];
    for (int i = 0; i < 16; i++) t[i] = static_cast<uint8_t>(i);
    std::vector<uint8_t> rgba;
    ge_decode_texture(ram.data(), params(GE_TFMT_CLUT8, 4, 2, 8), rgba);
    ASSERT_U32_EQ(red_at(rgba, 4, 0, 1), 8, "CLUT8 row 1 starts at bufw (8), not width (4)");
    ASSERT_U32_EQ(red_at(rgba, 4, 3, 1), 11, "CLUT8 row 1 last pixel");
}

static void test_swizzle_with_wider_buffer(std::vector<uint8_t>& ram) {
    // bufw 32 px of CLUT8 = 32 bytes = two 16x8 tiles per tile row; texture
    // width 16 uses only the first tile, whose rows are 16 consecutive bytes.
    uint8_t* t = &ram[TEX & 0x07FFFFFFU];
    for (int i = 0; i < 256; i++) t[i] = static_cast<uint8_t>(i);
    auto p = params(GE_TFMT_CLUT8, 16, 8, 32);
    p.swizzle = true;
    std::vector<uint8_t> rgba;
    ge_decode_texture(ram.data(), p, rgba);
    ASSERT_U32_EQ(red_at(rgba, 16, 0, 1), 16, "swizzled row 1 = tile row 1");
    ASSERT_U32_EQ(red_at(rgba, 16, 5, 7), 7 * 16 + 5, "swizzled row 7");
}

static void test_clut_shift_mask_start(std::vector<uint8_t>& ram) {
    uint8_t* t = &ram[TEX & 0x07FFFFFFU];
    t[0] = 0x37;
    auto p = params(GE_TFMT_CLUT8, 1, 1, 1);
    p.clut_shift = 4; p.clut_mask = 0x0F; p.clut_start = 1;
    std::vector<uint8_t> rgba;
    ge_decode_texture(ram.data(), p, rgba);
    ASSERT_U32_EQ(red_at(rgba, 1, 0, 0), (0x3 | 0x10), "index = ((0x37>>4)&0xF) | (1<<4)");
}

static void test_clut4_uses_buffer_width(std::vector<uint8_t>& ram) {
    uint8_t* t = &ram[TEX & 0x07FFFFFFU];
    std::memset(t, 0, 64);
    t[0] = 0x21;   // row 0: pixels 1, 2
    t[16] = 0x43;  // row 1 at bufw 32 px = 16 bytes: pixels 3, 4
    std::vector<uint8_t> rgba;
    ge_decode_texture(ram.data(), params(GE_TFMT_CLUT4, 2, 2, 32), rgba);
    ASSERT_U32_EQ(red_at(rgba, 2, 0, 0), 1, "CLUT4 low nibble first");
    ASSERT_U32_EQ(red_at(rgba, 2, 1, 1), 4, "CLUT4 row 1 at bufw/2 bytes");
}

int main() {
    std::vector<uint8_t> ram(0x08000000, 0);
    write_palette(ram);
    test_clut8_uses_buffer_width(ram);
    test_swizzle_with_wider_buffer(ram);
    test_clut_shift_mask_start(ram);
    test_clut4_uses_buffer_width(ram);
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
