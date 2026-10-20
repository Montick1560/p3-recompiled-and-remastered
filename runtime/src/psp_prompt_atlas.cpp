#include "psp_prompt_atlas.h"
#include "psp_prompt_raster.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

// An icon ready for the atlas: its cut-out symbol filled with the atlas's dark glyph interior and its
// neutral (white) pixels tinted to the atlas gray; Kenney's colours (Color style, red D-pad marks)
// are kept. Cached per (name, gray, hole).
const RgbaImage* prepared_icon(const std::string& name, uint8_t gray, uint8_t hole) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<RgbaImage>> cache;
    const std::string key = name + "/" + std::to_string(gray) + "/" + std::to_string(hole);
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<RgbaImage> img;
    if (const RgbaImage* src = prompt_icon_image(name)) {
        img = std::make_unique<RgbaImage>(*src);
        rgba_fill_holes(*img, hole, hole, hole);
        rgba_tint_neutral(*img, gray);
    }
    return (cache[key] = std::move(img)).get();
}

}  // namespace

uint64_t prompt_atlas_signature(const uint8_t* rgba, int w, int x, int y, int rw, int rh) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (int yy = y; yy < y + rh; yy++) {
        for (int xx = x; xx < x + rw; xx++) {
            h ^= rgba[(static_cast<size_t>(yy) * w + xx) * 4 + 3] >= 0x40 ? 1u : 0u;
            h *= 0x100000001B3ull;
        }
    }
    return h;
}

void prompt_atlas_compose(const PromptAtlasSpec& spec, InputDevice d, PromptStyle s, const ControlMap& map,
                          RgbaImage& img) {
    if (spec.width <= 0 || spec.height <= 0) return;
    const double sx = static_cast<double>(img.w) / spec.width, sy = static_cast<double>(img.h) / spec.height;
    const uint32_t slash = 0xFF000000u | (spec.gray << 16) | (spec.gray << 8) | spec.gray;
    for (const PromptCell& c : spec.cells) {
        const std::vector<std::string> names = prompt_icons(c.glyph, d, s, map);
        if (names.empty()) continue;
        std::vector<const RgbaImage*> icons;
        for (const std::string& n : names) icons.push_back(prepared_icon(n, spec.gray, spec.hole));
        if (std::find(icons.begin(), icons.end(), nullptr) != icons.end()) continue;
        const int x0 = static_cast<int>(std::lround(c.x * sx)), y0 = static_cast<int>(std::lround(c.y * sy));
        const int x1 = static_cast<int>(std::lround((c.x + c.w) * sx));
        const int y1 = static_cast<int>(std::lround((c.y + c.h) * sy));
        rgba_draw_cell(img, x0, y0, x1 - x0, y1 - y0, icons, slash);
    }
}

uint32_t prompt_atlas_variant(InputDevice d, PromptStyle s) {
    return 1u + static_cast<uint32_t>(d) * 2u + static_cast<uint32_t>(s);
}

bool prompt_atlas_variant_decode(uint32_t v, InputDevice* d, PromptStyle* s) {
    if (v < 1 || v > 8) return false;
    *d = static_cast<InputDevice>((v - 1) / 2);
    *s = static_cast<PromptStyle>((v - 1) % 2);
    return true;
}
