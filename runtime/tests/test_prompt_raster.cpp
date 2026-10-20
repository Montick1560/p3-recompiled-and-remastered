// Unit tests for psp_prompt_raster: the RGBA helpers that draw button prompts into a texture, and the
// embedded icon set (every name psp_prompts can return must resolve).
#include "psp_prompt_raster.h"
#include "psp_prompts.h"

#include <cctype>
#include <cstdio>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static RgbaImage filled(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    RgbaImage img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < img.px.size(); i += 4) {
        img.px[i] = r; img.px[i + 1] = g; img.px[i + 2] = b; img.px[i + 3] = a;
    }
    return img;
}
static const uint8_t* at(const RgbaImage& img, int x, int y) { return &img.px[(static_cast<size_t>(y) * img.w + x) * 4]; }
static uint8_t* at(RgbaImage& img, int x, int y) { return &img.px[(static_cast<size_t>(y) * img.w + x) * 4]; }

static void resample_keeps_flat_colour() {
    const RgbaImage src = filled(64, 64, 10, 200, 30, 255);
    for (int size : {8, 37, 100}) {
        const RgbaImage d = rgba_resample(src, size, size + 3);
        CHECK(d.w == size && d.h == size + 3, "target size");
        bool same = true;
        for (int y = 0; y < d.h; y++)
            for (int x = 0; x < d.w; x++) {
                const uint8_t* p = at(d, x, y);
                if (p[0] != 10 || p[1] != 200 || p[2] != 30 || p[3] != 255) same = false;
            }
        CHECK(same, "a flat opaque image stays flat and opaque");
    }
}

static void fill_holes_fills_only_enclosed() {
    RgbaImage ring = filled(9, 9, 0, 0, 0, 0);
    for (int y = 1; y < 8; y++)
        for (int x = 1; x < 8; x++)
            if (x == 1 || x == 7 || y == 1 || y == 7) { uint8_t* p = at(ring, x, y); p[0] = p[1] = p[2] = p[3] = 255; }
    rgba_fill_holes(ring, 0x17, 0x17, 0x17);
    CHECK(at(ring, 4, 4)[3] == 255 && at(ring, 4, 4)[0] == 0x17, "the enclosed centre is filled");
    CHECK(at(ring, 0, 0)[3] == 0 && at(ring, 8, 4)[3] == 0, "the outside stays transparent");
    CHECK(at(ring, 1, 1)[0] == 255, "the ring itself is unchanged");
}

static void tint_neutral_skips_colour() {
    RgbaImage img = filled(2, 1, 255, 255, 255, 255);
    uint8_t* red = at(img, 1, 0);
    red[0] = 255; red[1] = 76; red[2] = 77;
    rgba_tint_neutral(img, 0x7F);
    CHECK(at(img, 0, 0)[0] == 0x7F && at(img, 0, 0)[1] == 0x7F && at(img, 0, 0)[3] == 255, "white -> 0x7F");
    CHECK(at(img, 1, 0)[0] == 255 && at(img, 1, 0)[1] == 76, "red highlight kept");
}

static void draw_cell_stays_inside() {
    RgbaImage dst = filled(40, 30, 0xAB, 0xAB, 0xAB, 0xAB);
    const RgbaImage icon = filled(64, 64, 255, 255, 255, 255);
    rgba_draw_cell(dst, 10, 5, 9, 11, {&icon}, 0xFF7F7F7Fu);
    bool outside_ok = true, inside_written = true;
    for (int y = 0; y < dst.h; y++)
        for (int x = 0; x < dst.w; x++) {
            const bool in = x >= 10 && x < 19 && y >= 5 && y < 16;
            const uint8_t* p = at(dst, x, y);
            if (!in && (p[0] != 0xAB || p[3] != 0xAB)) outside_ok = false;
            if (in && p[0] == 0xAB) inside_written = false;
        }
    CHECK(outside_ok, "nothing written outside the cell");
    CHECK(inside_written, "the whole cell is cleared or drawn");
    CHECK(at(dst, 14, 10)[3] == 255, "the icon covers the cell centre");
    CHECK(at(dst, 14, 5)[3] == 0, "a 9-wide square icon leaves the top row of an 11-tall cell clear");
}

static void draw_cell_two_icons_side_by_side() {
    RgbaImage dst = filled(16, 11, 0, 0, 0, 0);
    const RgbaImage icon = filled(64, 64, 255, 255, 255, 255);
    rgba_draw_cell(dst, 0, 0, 16, 11, {&icon, &icon}, 0xFF7F7F7Fu);
    CHECK(at(dst, 2, 5)[3] == 255, "left icon");
    CHECK(at(dst, 13, 5)[3] == 255, "right icon");
    CHECK(at(dst, 8, 5)[0] == 0x7F || at(dst, 7, 5)[0] == 0x7F, "a gray slash between them");
}

static const std::map<std::string, int32_t> kKeys = {
    {"up", 1}, {"down", 2}, {"left", 3}, {"right", 4}, {"x", 5}, {"return", 6}, {"z", 7}, {"a", 8},
    {"s", 9}, {"q", 10}, {"w", 11}, {"space", 12}, {"v", 13}, {"tab", 14}};
static const std::map<std::string, int> kPads = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"start", 6}, {"leftstick", 7}, {"rightstick", 8},
    {"leftshoulder", 9}, {"rightshoulder", 10}, {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14}};
static std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

static void embedded_icons_resolve() {
    const RgbaImage* a = prompt_icon_image("xbox_button_a");
    CHECK(a != nullptr && a->w == 64 && a->h == 64, "xbox_button_a is 64x64");
    CHECK(prompt_icon_image("@arrows_vertical") != nullptr, "drawn keycap");
    CHECK(prompt_icon_image("nope") == nullptr, "unknown name");
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build(
        "", [](const std::string& n) { auto it = kKeys.find(lower(n)); return it == kKeys.end() ? 0 : it->second; },
        [](const std::string& n) { auto it = kPads.find(lower(n)); return it == kPads.end() ? -1 : it->second; }, &log);
    for (int g = 0; g <= static_cast<int>(PromptGlyph::StickVertical); g++)
        for (int d = 0; d < 4; d++)
            for (int s = 0; s < 2; s++)
                for (const std::string& n : prompt_icons(static_cast<PromptGlyph>(g), static_cast<InputDevice>(d),
                                                         static_cast<PromptStyle>(s), m)) {
                    if (prompt_icon_image(n) == nullptr) {
                        std::printf("  missing icon %s\n", n.c_str());
                        failures++;
                    }
                }
    for (const char* k : {"Return", "Space", "Tab", "Backspace", "Left Shift", "Escape", "F12", "9", ",", "Up"})
        for (bool wide : {false, true})
            CHECK(prompt_icon_image(prompt_key_icon(k, wide)) != nullptr, "key icon embedded");
}

int main() {
    resample_keeps_flat_colour();
    fill_holes_fills_only_enclosed();
    tint_neutral_skips_colour();
    draw_cell_stays_inside();
    draw_cell_two_icons_side_by_side();
    embedded_icons_resolve();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
