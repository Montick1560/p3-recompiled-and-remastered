// Unit tests for psp_bar_text: the embedded bar font (Zen Maru Gothic Bold) and the word rasteriser
// that redraws help-bar labels (left-aligned in the word's box, condensed only when too wide).
#include "psp_bar_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static RgbaImage clear_image(int w, int h) {
    RgbaImage img;
    img.w = w;
    img.h = h;
    img.px.assign(static_cast<size_t>(w) * h * 4, 0);
    return img;
}

struct Ink {
    int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
    int peak = 0;
    bool gray_ok = true;  // every inked texel has rgb == 0x7F
    int width() const { return x1 < x0 ? 0 : x1 - x0 + 1; }
};

static Ink ink_of(const RgbaImage& img) {
    Ink k;
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            const uint8_t* p = &img.px[(static_cast<size_t>(y) * img.w + x) * 4];
            if (p[3] == 0) continue;
            k.x0 = std::min(k.x0, x);
            k.x1 = std::max(k.x1, x);
            k.y0 = std::min(k.y0, y);
            k.y1 = std::max(k.y1, y);
            k.peak = std::max<int>(k.peak, p[3]);
            if (p[0] != 0x7F || p[1] != 0x7F || p[2] != 0x7F) k.gray_ok = false;
        }
    return k;
}

static void font_parses_and_covers_latin() {
    CHECK(bar_font_ok(), "embedded bar font parses");
    CHECK(bar_font_has("Pantalla de título"), "Spanish label");
    CHECK(bar_font_has("Introducción ÀÉÎÕÜßçñ"), "Latin-1 accents");
    CHECK(!bar_font_has("a\xEE\x80\x80" "b"), "U+E000 (private use) is missing");
}

static void cap_height_matches() {
    const float px = bar_font_px_for_cap(32.0f);
    RgbaImage img = clear_image(200, 60);
    CHECK(rgba_draw_text(img, 10, 50, 180, px, "H", 0x7F, 255), "draws H");
    const Ink k = ink_of(img);
    CHECK(k.y1 - k.y0 + 1 >= 31 && k.y1 - k.y0 + 1 <= 33, "H ink height 31..33 px at cap 32");
    CHECK(k.y1 == 49, "H ink bottom on baseline - 1");
    CHECK(k.gray_ok, "ink colour is the gray");
}

static void ink_stays_in_the_box() {
    const float px = bar_font_px_for_cap(16.0f);
    RgbaImage img = clear_image(200, 60);
    CHECK(rgba_draw_text(img, 20, 40, 120, px, "Seleccionar", 0x7F, 255), "draws a word");
    const Ink k = ink_of(img);
    CHECK(k.x0 >= 20 && k.x1 < 140, "ink only inside [x, x + box_w)");
    CHECK(k.x0 == 20, "left-aligned on x");
}

static void too_wide_is_condensed() {
    const float px = bar_font_px_for_cap(16.0f);
    RgbaImage wide = clear_image(400, 60);
    rgba_draw_text(wide, 0, 40, 400, px, "Pantalla de título", 0x7F, 255);
    const int natural = ink_of(wide).width();
    const int box = natural * 8 / 10;
    RgbaImage img = clear_image(400, 60);
    CHECK(rgba_draw_text(img, 5, 40, box, px, "Pantalla de título", 0x7F, 255), "draws condensed");
    const Ink k = ink_of(img);
    CHECK(k.x0 >= 5 && k.x1 < 5 + box, "condensed ink inside the box");
    CHECK(k.width() >= box * 9 / 10, "condensed to fill the box (>= 90 %)");
}

static void narrow_is_not_stretched() {
    const float px = bar_font_px_for_cap(16.0f);
    RgbaImage a = clear_image(300, 60);
    rgba_draw_text(a, 0, 40, 300, px, "Atrás", 0x7F, 255);
    const int natural = ink_of(a).width();
    RgbaImage b = clear_image(300, 60);
    rgba_draw_text(b, 0, 40, natural * 3, px, "Atrás", 0x7F, 255);
    CHECK(std::abs(ink_of(b).width() - natural) <= 1, "a narrow word keeps its natural width");
}

static void alpha_scales_coverage() {
    const float px = bar_font_px_for_cap(16.0f);
    RgbaImage full = clear_image(200, 60), half = clear_image(200, 60);
    rgba_draw_text(full, 0, 40, 200, px, "Confirmar", 0x7F, 255);
    rgba_draw_text(half, 0, 40, 200, px, "Confirmar", 0x7F, 128);
    const int pf = ink_of(full).peak, ph = ink_of(half).peak;
    CHECK(pf >= 250, "bold text reaches full coverage");
    CHECK(std::abs(ph - pf * 128 / 255) <= 1, "alpha 128 halves the peak alpha");
}

static void missing_glyph_draws_nothing() {
    const float px = bar_font_px_for_cap(16.0f);
    RgbaImage img = clear_image(200, 60);
    img.px[5] = 0x33;
    const RgbaImage before = img;
    CHECK(!rgba_draw_text(img, 0, 40, 200, px, "ab\xEE\x80\x80" "cd", 0x7F, 255), "missing code point -> false");
    CHECK(img.px == before.px, "image unchanged");
}

static void blends_over_existing_pixels() {
    const float px = bar_font_px_for_cap(16.0f);
    RgbaImage img = clear_image(200, 60);
    for (size_t i = 0; i < img.px.size(); i += 4) {
        img.px[i] = img.px[i + 1] = img.px[i + 2] = 0x7F;
        img.px[i + 3] = 100;
    }
    rgba_draw_text(img, 0, 40, 200, px, "Menú", 0x7F, 255);
    int lo = 255;
    for (size_t i = 0; i < img.px.size(); i += 4) lo = std::min<int>(lo, img.px[i + 3]);
    CHECK(lo == 100, "\"over\" never lowers the alpha that is there");
}

// Fitted words are cached (a device switch re-composes the atlas): a word drawn again, after other
// words and other boxes, is byte-identical to its first drawing.
static void redraw_is_identical() {
    const float px = bar_font_px_for_cap(16.0f);
    RgbaImage a = clear_image(300, 60), b = clear_image(300, 60), junk = clear_image(300, 60);
    rgba_draw_text(a, 3, 40, 70, px, "Pantalla de título", 0x7F, 200);
    rgba_draw_text(junk, 3, 40, 300, px, "Pantalla de título", 0x7F, 200);
    rgba_draw_text(junk, 3, 40, 70, bar_font_px_for_cap(15.0f), "Pantalla de título", 0x7F, 200);
    rgba_draw_text(junk, 3, 40, 70, px, "Confirmar", 0x7F, 200);
    rgba_draw_text(b, 3, 40, 70, px, "Pantalla de título", 0x7F, 200);
    CHECK(a.px == b.px, "same word, box and size -> same pixels");
    CHECK(a.px != junk.px, "other boxes / sizes / words are other drawings");
}

static int ink_rows(const RgbaImage& img, int y0, int y1) {  // inked texels' alpha sum in rows [y0, y1]
    int s = 0;
    for (int y = std::max(0, y0); y <= std::min(img.h - 1, y1); y++)
        for (int x = 0; x < img.w; x++) s += img.px[(static_cast<size_t>(y) * img.w + x) * 4 + 3];
    return s;
}

// A band that leaves one texel above the caps (Patapon 3's rows): a capital accent that rises higher is
// folded into that room, not cut off; likewise a descender below the band's last row.
static void accents_and_descenders_fold_into_the_band() {
    const float cap = 32.0f, px = bar_font_px_for_cap(cap);
    const int baseline = 60, cap_top = baseline - 32;
    RgbaImage free_img = clear_image(200, 100), band = clear_image(200, 100);
    rgba_draw_text(free_img, 10, baseline, 180, px, "É", 0x7F, 255);
    rgba_draw_text(band, 10, baseline, 180, px, "É", 0x7F, 255, cap_top - 4, baseline + 7);
    const int accent = ink_rows(free_img, 0, cap_top - 1), folded = ink_rows(band, cap_top - 4, cap_top - 1);
    CHECK(ink_rows(free_img, 0, cap_top - 5) > 0, "the font's É accent rises more than 4 px above the caps");
    CHECK(folded * 2 >= accent, "the accent is folded into the 4 px above the caps (>= half its ink)");
    CHECK(ink_rows(band, 0, cap_top - 5) == 0, "nothing above the band");
    CHECK(ink_rows(band, cap_top, baseline - 1) == ink_rows(free_img, cap_top, baseline - 1), "the E itself is unchanged");
    RgbaImage free_g = clear_image(200, 100), band_g = clear_image(200, 100);
    rgba_draw_text(free_g, 10, baseline, 180, px, "g", 0x7F, 255);
    rgba_draw_text(band_g, 10, baseline, 180, px, "g", 0x7F, 255, cap_top - 4, baseline + 3);
    CHECK(ink_rows(free_g, baseline + 4, 99) > 0, "the descender of g goes deeper than 4 px");
    CHECK(ink_rows(band_g, baseline, baseline + 3) * 2 >= ink_rows(free_g, baseline, 99), "the descender is folded in");
    CHECK(ink_rows(band_g, baseline + 4, 99) == 0, "nothing below the band");
}

static void width_has_no_condensing() {
    const float px = bar_font_px_for_cap(16.0f);
    const float w = bar_text_width("Seleccionar", px);
    CHECK(w > 0.0f, "positive width");
    CHECK(std::fabs(bar_text_width("Seleccionar", px * 2) - 2 * w) < 1.0f, "width scales with px");
}

int main() {
    font_parses_and_covers_latin();
    cap_height_matches();
    ink_stays_in_the_box();
    too_wide_is_condensed();
    narrow_is_not_stretched();
    alpha_scales_coverage();
    missing_glyph_draws_nothing();
    blends_over_existing_pixels();
    redraw_is_identical();
    accents_and_descenders_fold_into_the_band();
    width_has_no_condensing();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
