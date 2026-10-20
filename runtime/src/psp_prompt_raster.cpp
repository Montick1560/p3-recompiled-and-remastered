#include "psp_prompt_raster.h"
#include "psp_texrep_png.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

struct EmbeddedFile {
    const char* name;
    const unsigned char* data;
    unsigned size;
};
extern const EmbeddedFile g_prompt_icons[];  // prompt_icons_data.cpp (generated)
extern const unsigned g_prompt_icons_count;

namespace {

uint8_t* pix(RgbaImage& img, int x, int y) { return &img.px[(static_cast<size_t>(y) * img.w + x) * 4]; }
const uint8_t* pix(const RgbaImage& img, int x, int y) { return &img.px[(static_cast<size_t>(y) * img.w + x) * 4]; }

bool decode_embedded(const std::string& name, RgbaImage* out) {
    for (unsigned i = 0; i < g_prompt_icons_count; i++) {
        if (name != g_prompt_icons[i].name) continue;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!ge_texrep_decode_png(g_prompt_icons[i].data, g_prompt_icons[i].size, &w, &h, &rgba)) return false;
        out->w = w;
        out->h = h;
        out->px = std::move(rgba);
        return true;
    }
    return false;
}

// Paint the triangle (ax,ay) (bx,by) (cx,cy) opaque `rgba` (0xAABBGGRR), sampled at pixel centres.
void fill_triangle(RgbaImage& img, float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgba) {
    auto edge = [](float x0, float y0, float x1, float y1, float px, float py) {
        return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    };
    const float area = edge(ax, ay, bx, by, cx, cy);
    for (int y = 0; y < img.h; y++) {
        for (int x = 0; x < img.w; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float e0 = edge(ax, ay, bx, by, px, py), e1 = edge(bx, by, cx, cy, px, py),
                        e2 = edge(cx, cy, ax, ay, px, py);
            const bool inside = area > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0);
            if (!inside) continue;
            uint8_t* p = pix(img, x, y);
            p[0] = rgba & 0xFF; p[1] = (rgba >> 8) & 0xFF; p[2] = (rgba >> 16) & 0xFF; p[3] = rgba >> 24;
        }
    }
}

// "@arrows_*": Kenney's arrow keycap with its arrow filled in (a blank key), plus red arrow marks for
// the highlighted directions, like the D-pad highlights of the PSP glyphs. One keycap (not Kenney's
// inverted T), so the cell stays right when the game draws it rotated by 90 degrees.
bool draw_arrows(const std::string& which, RgbaImage* out) {
    if (!decode_embedded("keyboard_arrow_up", out)) return false;
    rgba_fill_holes(*out, 255, 255, 255);
    const uint32_t red = 0xFF4D4CFFu;
    const float c = out->w / 2.0f, s = out->w / 64.0f;
    if (which == "up") {
        fill_triangle(*out, c, 17 * s, c - 11 * s, 40 * s, c + 11 * s, 40 * s, red);
    } else if (which == "vertical") {
        fill_triangle(*out, c, 14 * s, c - 9 * s, 28 * s, c + 9 * s, 28 * s, red);
        fill_triangle(*out, c, 50 * s, c - 9 * s, 36 * s, c + 9 * s, 36 * s, red);
    } else if (which == "all") {
        fill_triangle(*out, c, 13 * s, c - 7 * s, 24 * s, c + 7 * s, 24 * s, red);
        fill_triangle(*out, c, 51 * s, c - 7 * s, 40 * s, c + 7 * s, 40 * s, red);
        fill_triangle(*out, 13 * s, c, 24 * s, c - 7 * s, 24 * s, c + 7 * s, red);
        fill_triangle(*out, 51 * s, c, 40 * s, c - 7 * s, 40 * s, c + 7 * s, red);
    } else {
        return false;
    }
    return true;
}

// 1-D resampling weights: output i takes sum(w * in[first + k]).
struct Taps {
    int first;
    std::vector<float> w;
};

std::vector<Taps> taps(int in, int out) {
    std::vector<Taps> t(out);
    const float scale = static_cast<float>(in) / out;
    for (int i = 0; i < out; i++) {
        Taps& tp = t[i];
        if (scale >= 1.0f) {  // box: the exact source span [i*scale, (i+1)*scale)
            const float a = i * scale, b = (i + 1) * scale;
            tp.first = static_cast<int>(std::floor(a));
            for (int k = tp.first; k < static_cast<int>(std::ceil(b)) && k < in; k++) {
                tp.w.push_back(std::min(b, k + 1.0f) - std::max(a, static_cast<float>(k)));
            }
        } else {  // bilinear between the two nearest source centres
            const float f = (i + 0.5f) * scale - 0.5f;
            int k0 = static_cast<int>(std::floor(f));
            float frac = f - k0;
            if (k0 < 0) { k0 = 0; frac = 0; }
            if (k0 >= in - 1) { k0 = in - 1; frac = 0; }
            tp.first = k0;
            tp.w = {1.0f - frac};
            if (frac > 0) tp.w.push_back(frac);
        }
        float sum = 0;
        for (float v : tp.w) sum += v;
        for (float& v : tp.w) v /= sum;
    }
    return t;
}

}  // namespace

const RgbaImage* prompt_icon_image(const std::string& name) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<RgbaImage>> cache;  // nullptr = known missing
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second.get();
    auto img = std::make_unique<RgbaImage>();
    const bool ok = name.rfind("@arrows_", 0) == 0 ? draw_arrows(name.substr(8), img.get())
                                                    : decode_embedded(name, img.get());
    if (!ok) img.reset();
    return (cache[name] = std::move(img)).get();
}

RgbaImage rgba_resample(const RgbaImage& src, int w, int h) {
    RgbaImage out;
    out.w = w;
    out.h = h;
    out.px.assign(static_cast<size_t>(w) * h * 4, 0);
    if (src.w <= 0 || src.h <= 0 || w <= 0 || h <= 0) return out;
    // Premultiplied float, horizontal pass then vertical pass.
    std::vector<float> pre(static_cast<size_t>(src.w) * src.h * 4);
    for (size_t i = 0; i < pre.size(); i += 4) {
        const float a = src.px[i + 3] / 255.0f;
        pre[i] = src.px[i] * a; pre[i + 1] = src.px[i + 1] * a; pre[i + 2] = src.px[i + 2] * a; pre[i + 3] = a;
    }
    const std::vector<Taps> tx = taps(src.w, w), ty = taps(src.h, h);
    std::vector<float> mid(static_cast<size_t>(w) * src.h * 4, 0.0f);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < w; x++)
            for (size_t k = 0; k < tx[x].w.size(); k++) {
                const float* s = &pre[(static_cast<size_t>(y) * src.w + tx[x].first + k) * 4];
                float* d = &mid[(static_cast<size_t>(y) * w + x) * 4];
                for (int c = 0; c < 4; c++) d[c] += s[c] * tx[x].w[k];
            }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < ty[y].w.size(); k++) {
                const float* s = &mid[(static_cast<size_t>(ty[y].first + k) * w + x) * 4];
                for (int c = 0; c < 4; c++) acc[c] += s[c] * ty[y].w[k];
            }
            uint8_t* d = pix(out, x, y);
            const float a = acc[3];
            d[3] = static_cast<uint8_t>(std::lround(std::clamp(a * 255.0f, 0.0f, 255.0f)));
            for (int c = 0; c < 3; c++) {
                d[c] = a > 0 ? static_cast<uint8_t>(std::lround(std::clamp(acc[c] / a, 0.0f, 255.0f))) : 0;
            }
        }
    return out;
}

void rgba_fill_holes(RgbaImage& img, uint8_t r, uint8_t g, uint8_t b) {
    const int w = img.w, h = img.h;
    std::vector<uint8_t> outside(static_cast<size_t>(w) * h, 0);
    std::vector<int> stack;
    auto clear = [&](int x, int y) { return pix(img, x, y)[3] < 32; };
    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const size_t i = static_cast<size_t>(y) * w + x;
        if (outside[i] || !clear(x, y)) return;
        outside[i] = 1;
        stack.push_back(static_cast<int>(i));
    };
    for (int x = 0; x < w; x++) { push(x, 0); push(x, h - 1); }
    for (int y = 0; y < h; y++) { push(0, y); push(w - 1, y); }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % w, y = i / w;
        push(x + 1, y); push(x - 1, y); push(x, y + 1); push(x, y - 1);
    }
    auto is_hole = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && !outside[static_cast<size_t>(y) * w + x] && clear(x, y);
    };
    auto near_outside = [&](int x, int y) {
        const int n[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : n) {
            const int nx = x + d[0], ny = y + d[1];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || outside[static_cast<size_t>(ny) * w + nx]) return true;
        }
        return false;
    };
    std::vector<uint8_t> hole(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) hole[static_cast<size_t>(y) * w + x] = is_hole(x, y);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = pix(img, x, y);
            const size_t i = static_cast<size_t>(y) * w + x;
            if (hole[i]) {
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                continue;
            }
            // The anti-aliased rim of a hole: blend over the fill colour so it has no light fringe.
            const bool rim = p[3] < 255 && !outside[i] && !near_outside(x, y) &&
                             ((x + 1 < w && hole[i + 1]) || (x > 0 && hole[i - 1]) ||
                              (y + 1 < h && hole[i + w]) || (y > 0 && hole[i - w]));
            if (rim) {
                const float a = p[3] / 255.0f;
                p[0] = static_cast<uint8_t>(std::lround(p[0] * a + r * (1 - a)));
                p[1] = static_cast<uint8_t>(std::lround(p[1] * a + g * (1 - a)));
                p[2] = static_cast<uint8_t>(std::lround(p[2] * a + b * (1 - a)));
                p[3] = 255;
            }
        }
}

void rgba_tint_neutral(RgbaImage& img, uint8_t gray) {
    for (size_t i = 0; i < img.px.size(); i += 4) {
        uint8_t* p = &img.px[i];
        const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
        if (mx - mn >= 24) continue;
        for (int c = 0; c < 3; c++) p[c] = static_cast<uint8_t>((p[c] * gray + 127) / 255);
    }
}

void rgba_draw_cell(RgbaImage& dst, int x, int y, int w, int h, const std::vector<const RgbaImage*>& icons,
                    uint32_t slash_rgba) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(dst.w, x + w), y1 = std::min(dst.h, y + h);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) std::memset(pix(dst, xx, yy), 0, 4);
    auto put = [&](int px, int py, const uint8_t* s) {  // "over" onto dst, clipped to the cell
        if (px < x0 || py < y0 || px >= x1 || py >= y1 || s[3] == 0) return;
        uint8_t* d = pix(dst, px, py);
        const float sa = s[3] / 255.0f, da = d[3] / 255.0f, oa = sa + da * (1 - sa);
        for (int c = 0; c < 3; c++) d[c] = static_cast<uint8_t>(std::lround((s[c] * sa + d[c] * da * (1 - sa)) / oa));
        d[3] = static_cast<uint8_t>(std::lround(oa * 255.0f));
    };
    auto draw_icon = [&](const RgbaImage* icon, int sx, int sw) {
        if (icon == nullptr || icon->w == 0 || sw <= 0) return;
        int bx0 = icon->w, by0 = icon->h, bx1 = -1, by1 = -1;  // alpha bounding box
        for (int yy = 0; yy < icon->h; yy++)
            for (int xx = 0; xx < icon->w; xx++)
                if (pix(*icon, xx, yy)[3] > 0) {
                    bx0 = std::min(bx0, xx); by0 = std::min(by0, yy); bx1 = std::max(bx1, xx); by1 = std::max(by1, yy);
                }
        if (bx1 < 0) return;
        RgbaImage crop;
        crop.w = bx1 - bx0 + 1;
        crop.h = by1 - by0 + 1;
        crop.px.resize(static_cast<size_t>(crop.w) * crop.h * 4);
        for (int yy = 0; yy < crop.h; yy++) std::memcpy(pix(crop, 0, yy), pix(*icon, bx0, by0 + yy), crop.w * 4);
        const float scale = std::min(static_cast<float>(sw) / crop.w, static_cast<float>(h) / crop.h);
        const int tw = std::max(1, static_cast<int>(std::lround(crop.w * scale)));
        const int th = std::max(1, static_cast<int>(std::lround(crop.h * scale)));
        const RgbaImage fit = rgba_resample(crop, std::min(tw, sw), std::min(th, h));
        const int ox = sx + (sw - fit.w) / 2, oy = y + (h - fit.h) / 2;
        for (int yy = 0; yy < fit.h; yy++)
            for (int xx = 0; xx < fit.w; xx++) put(ox + xx, oy + yy, pix(fit, xx, yy));
    };
    if (icons.size() == 1) {
        draw_icon(icons[0], x, w);
    } else if (icons.size() == 2) {
        const int gap = std::max(1, w / 5);
        const int left = (w - gap) / 2;
        draw_icon(icons[0], x, left);
        draw_icon(icons[1], x + left + gap, w - left - gap);
        // The slash: top right to bottom left of the gap, about one source texel thick.
        const uint8_t s[4] = {static_cast<uint8_t>(slash_rgba & 0xFF), static_cast<uint8_t>((slash_rgba >> 8) & 0xFF),
                              static_cast<uint8_t>((slash_rgba >> 16) & 0xFF), static_cast<uint8_t>(slash_rgba >> 24)};
        const float t = std::max(1.0f, h / 11.0f);
        const float gx = static_cast<float>(x + left);
        for (int row = 0; row < h; row++) {
            const float f = (row + 0.5f) / h;
            const float cx = gx + gap * 0.75f + (gap * 0.25f - gap * 0.75f) * f;
            for (int px = static_cast<int>(std::floor(cx - t / 2 + 0.5f)); px < static_cast<int>(std::floor(cx + t / 2 + 0.5f)); px++) {
                put(px, y + row, s);
            }
        }
    }
}
