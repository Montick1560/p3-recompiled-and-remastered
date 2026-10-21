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

uint64_t prompt_atlas_signature(const uint8_t* rgba, int w, const std::vector<PromptRect>& rects) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const PromptRect& r : rects) {
        for (int yy = r.y; yy < r.y + r.h; yy++) {
            for (int xx = r.x; xx < r.x + r.w; xx++) {
                h ^= rgba[(static_cast<size_t>(yy) * w + xx) * 4 + 3] != 0 ? 1u : 0u;
                h *= 0x100000001B3ull;
            }
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
        // An enlarged image (bilinear x scale) spreads the old glyph half a source texel outside its
        // cell: clear that ring too, else a faint box stays around the new icon.
        const int mx = static_cast<int>(sx / 2), my = static_cast<int>(sy / 2);
        const int cx0 = std::max(0, x0 - mx), cy0 = std::max(0, y0 - my);
        const int cx1 = std::min(img.w, x1 + mx), cy1 = std::min(img.h, y1 + my);
        for (int y = cy0; y < cy1; y++)
            for (int x = cx0; x < cx1; x++) img.px[(static_cast<size_t>(y) * img.w + x) * 4 + 3] = 0;
        rgba_draw_cell(img, x0, y0, x1 - x0, y1 - y0, icons, slash);
        if (spec.cell_bg != 0) {
            // An opaque background (a drum's black disc): the icon goes over it, so the cell has no hole.
            const float ba = (spec.cell_bg >> 24) / 255.0f;
            const float bg[3] = {static_cast<float>((spec.cell_bg >> 16) & 0xFF),
                                 static_cast<float>((spec.cell_bg >> 8) & 0xFF), static_cast<float>(spec.cell_bg & 0xFF)};
            for (int y = cy0; y < cy1; y++)
                for (int x = cx0; x < cx1; x++) {
                    uint8_t* p = &img.px[(static_cast<size_t>(y) * img.w + x) * 4];
                    const float sa = p[3] / 255.0f, oa = sa + ba * (1 - sa);
                    for (int c = 0; c < 3; c++) p[c] = static_cast<uint8_t>(std::lround((p[c] * sa + bg[c] * ba * (1 - sa)) / oa));
                    p[3] = static_cast<uint8_t>(std::lround(oa * 255.0f));
                }
            continue;
        }
        // Fully transparent texels take the atlas gray (as the game's own do): the GE filters the
        // texture unpremultiplied, so black ones would leave a dark rim around the cell.
        for (int y = cy0; y < cy1; y++)
            for (int x = cx0; x < cx1; x++) {
                uint8_t* p = &img.px[(static_cast<size_t>(y) * img.w + x) * 4];
                if (p[3] == 0) p[0] = p[1] = p[2] = spec.gray;
            }
    }
}

uint32_t prompt_atlas_variant(InputDevice d, PromptStyle s, bool icons, bool text) {
    return (icons ? 1u + static_cast<uint32_t>(d) * 2u + static_cast<uint32_t>(s) : 0u) | (text ? 0x10u : 0u);
}

bool prompt_atlas_variant_decode(uint32_t v, InputDevice* d, PromptStyle* s, bool* icons, bool* text) {
    const uint32_t iv = v & 0xFu;
    if (v == 0 || v > 0x18u || iv > 8) return false;
    *icons = iv != 0;
    *text = (v & 0x10u) != 0;
    *d = static_cast<InputDevice>(iv != 0 ? (iv - 1) / 2 : 0);
    *s = static_cast<PromptStyle>(iv != 0 ? (iv - 1) % 2 : 0);
    return true;
}

uint32_t prompt_atlas_recognise(const PromptAtlasSpec& spec, const uint8_t* rgba, int w) {
    const uint64_t sig = prompt_atlas_signature(rgba, w, spec.sig_rects);
    if (std::find(spec.sig_hashes.begin(), spec.sig_hashes.end(), sig) == spec.sig_hashes.end()) return 0;
    if (spec.label_sig_rects.empty()) return 1;
    const uint64_t labels = prompt_atlas_signature(rgba, w, spec.label_sig_rects);
    for (size_t i = 0; i < spec.labels.size() && i < 254; i++) {
        if (spec.labels[i].sig == labels) return 2 + static_cast<uint32_t>(i);
    }
    return 1;
}

void prompt_atlas_apply(const PromptAtlasSpec& spec, uint32_t v, const ControlMap& map, RgbaImage& img) {
    InputDevice d;
    PromptStyle s;
    bool icons, text;
    if (spec.width <= 0 || spec.height <= 0 || !prompt_atlas_variant_decode(v & 0xFFFFFu, &d, &s, &icons, &text)) return;
    if (icons) prompt_atlas_compose(spec, d, s, map, img);
    if (!text) return;
    const uint32_t tag = (v >> 20) & 0xFFu;
    const BarLabels* t = tag >= 2 && tag - 2 < spec.labels.size() ? &spec.labels[tag - 2] : nullptr;
    std::vector<PromptRect> keep;
    for (const PromptCell& c : spec.cells) keep.push_back({c.x, c.y, c.w, c.h});
    const double sx = static_cast<double>(img.w) / spec.width, sy = static_cast<double>(img.h) / spec.height;
    // The separators share the labels' palette: their alpha is the table's when the language is known.
    bar_text_compose(t, spec.separators, spec.separator_cap, spec.gray, t != nullptr ? t->alpha : 0xFF,
                     std::move(keep), sx, sy, img);
}

uint32_t prompt_atlases_recognise(const std::vector<PromptAtlasSpec>& specs, int width, int height, int format,
                                  const uint8_t* rgba) {
    for (size_t i = 0; i < specs.size() && i < static_cast<size_t>(kPromptAtlasMax); i++) {
        const PromptAtlasSpec& s = specs[i];
        if (s.width != width || s.height != height || s.format != format) continue;
        uint32_t local = prompt_atlas_recognise(s, rgba, width);
        if (local == 0) continue;
        if (local >= kPromptAtlasTagStride) local = 1;
        return static_cast<uint32_t>(i) * kPromptAtlasTagStride + local;
    }
    return 0;
}

void prompt_atlases_apply(const std::vector<PromptAtlasSpec>& specs, uint32_t v, const ControlMap& map,
                          RgbaImage& img) {
    const uint32_t tag = (v >> 20) & 0xFFu, index = tag / kPromptAtlasTagStride;
    if (index >= specs.size()) return;
    prompt_atlas_apply(specs[index], ((tag % kPromptAtlasTagStride) << 20) | (v & 0xFFFFFu), map, img);
}
