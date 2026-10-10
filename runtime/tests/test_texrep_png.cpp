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
