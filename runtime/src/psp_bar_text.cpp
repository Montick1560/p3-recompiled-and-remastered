#include "psp_bar_text.h"
#include "psp_prompt_atlas.h"

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>

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

// A word fitted to its box: its coverage and ink columns [l, r].
struct Fitted {
    Coverage c;
    int l = 0, r = -1;
};

// Fits a word (condensed until its ink fits the box; the bitmap boxes round outwards, so one pass can
// be a texel over). Cached per (text, px, box_w): a device switch re-composes the atlas with the same
// words, and rasterising them all again costs ~16 ms at render scale 4.
Fitted fit_word(const BarFont& f, const std::vector<int>& cps, float px, int box_w) {
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    float sx = 1.0f;
    Fitted fit;
    fit.c = render(f, cps, scale, sx, &fit.l, &fit.r);
    for (int pass = 0; pass < 4 && fit.r - fit.l + 1 > box_w; pass++) {
        sx *= static_cast<float>(box_w) / static_cast<float>(fit.r - fit.l + 1) * (pass == 0 ? 1.0f : 0.995f);
        fit.c = render(f, cps, scale, sx, &fit.l, &fit.r);
    }
    return fit;
}

std::shared_ptr<const Fitted> fitted(const BarFont& f, const std::vector<int>& cps, const std::string& utf8, float px,
                                     int box_w) {
    static std::mutex mu;
    static std::map<std::tuple<std::string, float, int>, std::shared_ptr<const Fitted>> cache;
    const auto key = std::make_tuple(utf8, px, box_w);
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    auto fit = std::make_shared<const Fitted>(fit_word(f, cps, px, box_w));
    std::lock_guard<std::mutex> lk(mu);
    if (cache.size() >= 4096) cache.clear();  // a few hundred words per atlas and scale in practice
    cache[key] = fit;
    return fit;
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

int bar_text_ink_width(const std::string& utf8, float px) {
    const BarFont& f = bar_font();
    const std::vector<int> cps = decode_utf8(utf8);
    if (!has_all(f, cps) || px <= 0) return 0;
    const std::shared_ptr<const Fitted> fit = fitted(f, cps, utf8, px, 1 << 20);
    return std::max(0, fit->r - fit->l + 1);
}

bool rgba_draw_text(RgbaImage& img, int x, int baseline, int box_w, float px, const std::string& utf8,
                    uint8_t gray, uint8_t alpha, int clip_y0, int clip_y1) {
    const BarFont& f = bar_font();
    const std::vector<int> cps = decode_utf8(utf8);
    if (!has_all(f, cps) || box_w <= 0 || px <= 0) return false;
    const std::shared_ptr<const Fitted> fit = fitted(f, cps, utf8, px, box_w);
    const Coverage& c = fit->c;
    const int l = fit->l, r = fit->r;
    if (r < l) return true;  // only spaces
    // Destination row -> source rows [s0, s1): identity inside the band; an accent above it (a capital's)
    // is folded into the rows between the band top and the caps, a descender below it into the rows
    // between the baseline and the band bottom (max of the folded rows), like the game's own squeezed
    // accents. Nothing is drawn outside [clip_y0, clip_y1].
    int ink_top = c.h, ink_bot = -1;
    for (int y = 0; y < c.h; y++)
        for (int cx = l; cx <= r; cx++)
            if (c.a[static_cast<size_t>(y) * c.w + cx] != 0) {
                ink_top = std::min(ink_top, y);
                ink_bot = std::max(ink_bot, y);
            }
    const int off = baseline - c.base;  // destination row of source row 0
    const int cap_src = c.base - static_cast<int>(std::lround(px * f.cap_units / f.height_units));
    auto fold = [](int d, int d0, int d1, int s0, int s1, int* a, int* b) {  // dst [d0, d1) <- src [s0, s1)
        const double k = static_cast<double>(s1 - s0) / (d1 - d0);
        *a = s0 + static_cast<int>(std::floor((d - d0) * k));
        *b = std::max(*a + 1, s0 + static_cast<int>(std::floor((d + 1 - d0) * k)));
    };
    const bool fold_top = ink_top + off < clip_y0 && cap_src + off > clip_y0;
    const bool fold_bot = ink_bot + off > clip_y1 && c.base + off <= clip_y1;
    for (int dy = std::max({0, clip_y0, ink_top + off}); dy <= std::min({img.h - 1, clip_y1, ink_bot + off}); dy++) {
        int s0 = dy - off, s1 = s0 + 1;
        if (fold_top && dy < cap_src + off) fold(dy, clip_y0, cap_src + off, ink_top, cap_src, &s0, &s1);
        if (fold_bot && dy >= c.base + off) fold(dy, c.base + off, clip_y1 + 1, c.base, ink_bot + 1, &s0, &s1);
        for (int cx = l; cx <= r; cx++) {
            const int dx = x + cx - l;
            if (dx < x || dx >= x + box_w || dx < 0 || dx >= img.w) continue;
            int cov = 0;
            for (int y = s0; y < s1; y++) cov = std::max<int>(cov, c.a[static_cast<size_t>(y) * c.w + cx]);
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

namespace {

struct Px {
    int x0, y0, x1, y1;  // half-open pixel rect
    bool has(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

Px scaled(int x0, int y0, int x1_incl, int y1_incl, double sx, double sy) {
    return {static_cast<int>(std::lround(x0 * sx)), static_cast<int>(std::lround(y0 * sy)),
            static_cast<int>(std::lround((x1_incl + 1) * sx)), static_cast<int>(std::lround((y1_incl + 1) * sy))};
}

struct Job {
    const BarRow* row;
    const BarWord* word;
    int cap;
    uint8_t alpha;
    bool label;  // a label word (false: a separator glyph)
};

void log_missing(const std::string& text) {
    static std::mutex mu;
    static std::set<std::string> logged;
    std::lock_guard<std::mutex> lk(mu);
    if (logged.insert(text).second) {
        std::fprintf(stderr, "[PROMPTS] bar font lacks a character of \"%s\": the game's label stays\n", text.c_str());
    }
}

}  // namespace

void bar_text_compose(const BarLabels* labels, const BarRow& separators, int cap, uint8_t gray, uint8_t alpha,
                      std::vector<PromptRect> keep, double sx, double sy, RgbaImage& img) {
    std::vector<Job> jobs;
    auto add_row = [&](const BarRow& row, int c, uint8_t a, bool label) {
        for (const BarWord& w : row.words) {
            if (bar_font_has(w.text)) {
                jobs.push_back({&row, &w, c, a, label});
            } else {
                log_missing(w.text);
                keep.push_back({w.x0, row.y0, w.x1 - w.x0 + 1, row.y1 - row.y0 + 1});
            }
        }
    };
    if (labels != nullptr) {
        for (const BarRow& row : labels->rows) add_row(row, labels->cap, labels->alpha, true);
    }
    add_row(separators, cap, alpha, false);
    if (jobs.empty()) return;
    // An enlarged image (bilinear x scale) spreads each old letter half a source texel outside its box:
    // clear that ring too, except over a kept rect or its own ring.
    const int mx = static_cast<int>(sx / 2), my = static_cast<int>(sy / 2);
    std::vector<Px> kept, kept_ring;
    for (const PromptRect& k : keep) {
        const Px p = scaled(k.x, k.y, k.x + k.w - 1, k.y + k.h - 1, sx, sy);
        kept.push_back(p);
        kept_ring.push_back({p.x0 - mx, p.y0 - my, p.x1 + mx, p.y1 + my});
    }
    auto inside = [](const std::vector<Px>& v, int x, int y) {
        for (const Px& p : v)
            if (p.has(x, y)) return true;
        return false;
    };
    std::vector<Px> near, near_ring;
    for (const Job& j : jobs) {
        const Px b = scaled(j.word->x0, j.row->y0, j.word->x1, j.row->y1, sx, sy);
        const int x0 = std::max(0, b.x0 - mx), y0 = std::max(0, b.y0 - my);
        const int x1 = std::min(img.w, b.x1 + mx), y1 = std::min(img.h, b.y1 + my);
        near.clear();
        near_ring.clear();
        for (size_t k = 0; k < kept.size(); k++) {
            const Px& r = kept_ring[k];
            if (r.x0 < x1 && r.x1 > x0 && r.y0 < y1 && r.y1 > y0) {
                near.push_back(kept[k]);
                near_ring.push_back(r);
            }
        }
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                if (!near_ring.empty() && inside(b.has(x, y) ? near : near_ring, x, y)) continue;
                uint8_t* p = &img.px[(static_cast<size_t>(y) * img.w + x) * 4];
                p[0] = p[1] = p[2] = gray;
                p[3] = 0;
            }
    }
    for (const Job& j : jobs) {
        const Px b = scaled(j.word->x0, j.row->y0, j.word->x1, j.row->y1, sx, sy);
        const int base = static_cast<int>(std::lround(j.row->baseline * sy));
        // The game's pixel font sets words 1-2 texels apart; the bar font's wider letter spacing would
        // make them read as one: a word that close to the next one stops short of its box end.
        const BarWord* next =
            j.label && j.word + 1 < j.row->words.data() + j.row->words.size() ? j.word + 1 : nullptr;
        // The margin never condenses the word below 75 % of its natural width.
        const float px = bar_font_px_for_cap(static_cast<float>(j.cap * sy));
        int margin = next != nullptr && next->x0 - j.word->x1 - 1 <= 2
                         ? std::max(1, static_cast<int>(std::lround(0.12 * j.cap * sy)))
                         : 0;
        if (margin > 0) {
            const int room = b.x1 - b.x0 - static_cast<int>(std::ceil(bar_text_ink_width(j.word->text, px) * 0.75));
            margin = std::clamp(room, 0, margin);
        }
        rgba_draw_text(img, b.x0, base, std::max(1, b.x1 - b.x0 - margin), px, j.word->text, gray, j.alpha, b.y0,
                       b.y1 - 1);
    }
}
