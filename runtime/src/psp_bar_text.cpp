#include "psp_bar_text.h"

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

struct EmbeddedFile {
    const char* name;
    const unsigned char* data;
    unsigned size;
};
extern const EmbeddedFile g_bar_fonts[];  // bar_fonts_data.cpp (generated)
extern const unsigned g_bar_fonts_count;

namespace {

constexpr const char* kBarFont = "ZenMaruGothic-Bold";

struct BarFont {
    bool ok = false;
    stbtt_fontinfo info{};
    float cap_units = 0;     // 'H' height in font units
    float height_units = 0;  // ascent - descent (what stbtt_ScaleForPixelHeight divides by)
};

// Parsed once (thread-safe static init); stbtt_fontinfo is read-only afterwards.
const BarFont& bar_font() {
    static const BarFont f = [] {
        BarFont b;
        for (unsigned i = 0; i < g_bar_fonts_count; i++) {
            if (std::string(g_bar_fonts[i].name) != kBarFont) continue;
            const unsigned char* data = g_bar_fonts[i].data;
            if (!stbtt_InitFont(&b.info, data, stbtt_GetFontOffsetForIndex(data, 0))) break;
            int x0, y0, x1, y1;
            if (!stbtt_GetCodepointBox(&b.info, 'H', &x0, &y0, &x1, &y1)) break;
            int ascent, descent, gap;
            stbtt_GetFontVMetrics(&b.info, &ascent, &descent, &gap);
            b.cap_units = static_cast<float>(y1 - y0);
            b.height_units = static_cast<float>(ascent - descent);
            b.ok = b.cap_units > 0 && b.height_units > 0;
            break;
        }
        return b;
    }();
    return f;
}

// UTF-8 -> code points; malformed bytes become U+FFFD.
std::vector<int> decode_utf8(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size()) {
            out.push_back(0xFFFD);
            i++;
            continue;
        }
        int cp = n == 0 ? c : c & (0x3F >> n);
        bool bad = false;
        for (int k = 1; k <= n; k++) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) bad = true;
            cp = (cp << 6) | (cc & 0x3F);
        }
        out.push_back(bad ? 0xFFFD : cp);
        i += bad ? 1 : n + 1;
    }
    return out;
}

bool has_all(const BarFont& f, const std::vector<int>& cps) {
    if (!f.ok) return false;
    for (int cp : cps) {
        if (stbtt_FindGlyphIndex(&f.info, cp) == 0) return false;
    }
    return true;
}

// Coverage of the word at (scale * sx, scale) into a buffer with the baseline at row `base`;
// returns its ink columns [*l, *r] (r < l when nothing is inked).
struct Coverage {
    int w = 0, h = 0, base = 0, origin = 0;  // pen starts at column `origin`
    std::vector<uint8_t> a;
};

Coverage render(const BarFont& f, const std::vector<int>& cps, float scale, float sx, int* l, int* r) {
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    Coverage c;
    float adv_total = 0;
    for (size_t i = 0; i < cps.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f.info, cps[i], &adv, &lsb);
        adv_total += adv * scale * sx;
        if (i + 1 < cps.size()) adv_total += stbtt_GetCodepointKernAdvance(&f.info, cps[i], cps[i + 1]) * scale * sx;
    }
    const int pad = static_cast<int>(std::ceil(scale * f.height_units)) + 4;
    c.origin = pad;
    c.w = static_cast<int>(std::ceil(adv_total)) + 2 * pad;
    c.base = static_cast<int>(std::ceil(ascent * scale)) + 2;
    c.h = c.base + static_cast<int>(std::ceil(-descent * scale)) + 4;
    c.a.assign(static_cast<size_t>(c.w) * c.h, 0);
    std::vector<uint8_t> glyph;
    float pen = static_cast<float>(c.origin);
    for (size_t i = 0; i < cps.size(); i++) {
        const int cp = cps[i];
        const float shift = pen - std::floor(pen);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&f.info, cp, scale * sx, scale, shift, 0, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            glyph.assign(static_cast<size_t>(gw) * gh, 0);
            stbtt_MakeCodepointBitmapSubpixel(&f.info, glyph.data(), gw, gh, gw, scale * sx, scale, shift, 0, cp);
            const int ox = static_cast<int>(std::floor(pen)) + x0, oy = c.base + y0;
            for (int y = 0; y < gh; y++) {
                const int dy = oy + y;
                if (dy < 0 || dy >= c.h) continue;
                for (int x = 0; x < gw; x++) {
                    const int dx = ox + x;
                    if (dx < 0 || dx >= c.w) continue;
                    uint8_t& d = c.a[static_cast<size_t>(dy) * c.w + dx];
                    d = static_cast<uint8_t>(std::min(255, d + glyph[static_cast<size_t>(y) * gw + x]));
                }
            }
        }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
        pen += adv * scale * sx;
        if (i + 1 < cps.size()) pen += stbtt_GetCodepointKernAdvance(&f.info, cp, cps[i + 1]) * scale * sx;
    }
    *l = c.w;
    *r = -1;
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++) {
            if (c.a[static_cast<size_t>(y) * c.w + x] == 0) continue;
            *l = std::min(*l, x);
            *r = std::max(*r, x);
        }
    return c;
}

}  // namespace

bool bar_font_ok() { return bar_font().ok; }

bool bar_font_has(const std::string& utf8) { return has_all(bar_font(), decode_utf8(utf8)); }

float bar_font_px_for_cap(float cap_px) {
    const BarFont& f = bar_font();
    return f.ok ? cap_px * f.height_units / f.cap_units : 0.0f;
}

float bar_text_width(const std::string& utf8, float px) {
    const BarFont& f = bar_font();
    if (!f.ok) return 0.0f;
    const std::vector<int> cps = decode_utf8(utf8);
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    float w = 0;
    for (size_t i = 0; i < cps.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f.info, cps[i], &adv, &lsb);
        w += adv * scale;
        if (i + 1 < cps.size()) w += stbtt_GetCodepointKernAdvance(&f.info, cps[i], cps[i + 1]) * scale;
    }
    return w;
}

bool rgba_draw_text(RgbaImage& img, int x, int baseline, int box_w, float px, const std::string& utf8,
                    uint8_t gray, uint8_t alpha) {
    const BarFont& f = bar_font();
    const std::vector<int> cps = decode_utf8(utf8);
    if (!has_all(f, cps) || box_w <= 0 || px <= 0) return false;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    float sx = 1.0f;
    int l, r;
    Coverage c = render(f, cps, scale, sx, &l, &r);
    // Condense until the ink fits (the bitmap boxes round outwards, so one pass can be a texel over).
    for (int pass = 0; pass < 4 && r - l + 1 > box_w; pass++) {
        sx *= static_cast<float>(box_w) / static_cast<float>(r - l + 1) * (pass == 0 ? 1.0f : 0.995f);
        c = render(f, cps, scale, sx, &l, &r);
    }
    if (r < l) return true;  // only spaces
    for (int y = 0; y < c.h; y++) {
        const int dy = baseline - c.base + y;
        if (dy < 0 || dy >= img.h) continue;
        for (int cx = l; cx <= r; cx++) {
            const int dx = x + cx - l;
            if (dx < x || dx >= x + box_w || dx < 0 || dx >= img.w) continue;
            const int cov = c.a[static_cast<size_t>(y) * c.w + cx];
            if (cov == 0) continue;
            const int sa = (cov * alpha + 127) / 255;
            if (sa == 0) continue;
            uint8_t* p = &img.px[(static_cast<size_t>(dy) * img.w + dx) * 4];
            const int da = p[3];
            const int oa = sa + (da * (255 - sa) + 127) / 255;
            for (int k = 0; k < 3; k++) {
                const int num = gray * sa * 255 + p[k] * da * (255 - sa);
                p[k] = static_cast<uint8_t>(oa == 0 ? gray : (num + oa * 255 / 2) / (oa * 255));
            }
            p[3] = static_cast<uint8_t>(oa);
        }
    }
    return true;
}
