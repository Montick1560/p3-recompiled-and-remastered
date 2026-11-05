// psp_tex_upscale.h: xBRZ upscaling of RGBA8 textures (PSPRECOMP_TEX_UPSCALE).
#include "psp_tex_upscale.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static std::vector<uint8_t> image(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a; }
    return px;
}

int main() {
    CHECK(tex_upscale_factor_parse("xbrz2") == 2, "xbrz2");
    CHECK(tex_upscale_factor_parse("xbrz3") == 3, "xbrz3");
    CHECK(tex_upscale_factor_parse("xbrz4") == 4, "xbrz4");
    CHECK(tex_upscale_factor_parse("") == 0 && tex_upscale_factor_parse(nullptr) == 0, "empty -> off");
    CHECK(tex_upscale_factor_parse("off") == 0 && tex_upscale_factor_parse("xbrz5") == 0, "off / unknown");

    {   // a flat colour stays that colour, at w*f x h*f
        const auto src = image(4, 4, 0x10, 0x20, 0x30, 0xFF);
        std::vector<uint8_t> out;
        CHECK(tex_upscale_xbrz(2, src.data(), 4, 4, &out), "2x scales");
        CHECK(out.size() == 8u * 8u * 4u, "2x size");
        bool flat = true;
        for (size_t i = 0; i < out.size(); i += 4)
            flat &= out[i] == 0x10 && out[i + 1] == 0x20 && out[i + 2] == 0x30 && out[i + 3] == 0xFF;
        CHECK(flat, "flat colour kept (RGBA order)");
        CHECK(tex_upscale_xbrz(3, src.data(), 4, 4, &out) && out.size() == 12u * 12u * 4u, "3x size");
        CHECK(tex_upscale_xbrz(4, src.data(), 4, 4, &out) && out.size() == 16u * 16u * 4u, "4x size");
    }
    {   // alpha survives: transparent left half, opaque right half
        auto src = image(4, 4, 0xFF, 0x80, 0x00, 0xFF);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 2; x++) src[(y * 4 + x) * 4 + 3] = 0;
        std::vector<uint8_t> out;
        CHECK(tex_upscale_xbrz(2, src.data(), 4, 4, &out), "alpha image scales");
        CHECK(out[(3 * 8 + 0) * 4 + 3] == 0, "left stays transparent");
        CHECK(out[(3 * 8 + 7) * 4 + 3] == 0xFF, "right stays opaque");
    }
    {   // refusals
        const auto src = image(1024, 8, 0, 0, 0, 0xFF);
        std::vector<uint8_t> out;
        CHECK(!tex_upscale_xbrz(5, src.data(), 4, 4, &out), "factor 5 refused");
        CHECK(!tex_upscale_xbrz(2, src.data(), 0, 4, &out), "zero width refused");
        CHECK(!tex_upscale_xbrz(3, src.data(), 1024, 8, &out), "3072 px side refused");
        CHECK(tex_upscale_xbrz(2, src.data(), 1024, 8, &out), "2048 px side allowed");
    }
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_tex_upscale: all passed\n");
    return 0;
}
