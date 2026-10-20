// Unit tests for psp_prompt_atlas: recognising a help-bar atlas by a region of its alpha mask, and
// composing a device / style variant (only the cells change, at any scale).
#include "psp_prompt_atlas.h"
#include "psp_prompt_raster.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static const std::map<std::string, int32_t> kKeys = {
    {"up", 1}, {"down", 2}, {"left", 3}, {"right", 4}, {"x", 5}, {"return", 6}, {"z", 7}, {"a", 8},
    {"s", 9}, {"q", 10}, {"w", 11}, {"space", 12}, {"v", 13}, {"tab", 14}};
static const std::map<std::string, int> kPads = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"start", 6}, {"leftstick", 7}, {"rightstick", 8},
    {"leftshoulder", 9}, {"rightshoulder", 10}, {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14}};
static std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
static ControlMap defaults() {
    std::vector<std::string> log;
    return ControlMap::build(
        "", [](const std::string& n) { auto it = kKeys.find(lower(n)); return it == kKeys.end() ? 0 : it->second; },
        [](const std::string& n) { auto it = kPads.find(lower(n)); return it == kPads.end() ? -1 : it->second; }, &log);
}

// A 256x128 atlas: gray noise everywhere (labels), a "glyph row" pattern in the recognition region.
static RgbaImage synthetic_atlas() {
    RgbaImage img;
    img.w = 256;
    img.h = 128;
    img.px.resize(256 * 128 * 4);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 256; x++) {
            uint8_t* p = &img.px[(y * 256 + x) * 4];
            p[0] = p[1] = p[2] = 0x7F;
            p[3] = static_cast<uint8_t>(((x * 7 + y * 13) % 5) == 0 ? 0xFF : 0x00);
        }
    return img;
}

static PromptAtlasSpec spec_for(const RgbaImage& img) {
    PromptAtlasSpec s;
    s.width = 256;
    s.height = 128;
    s.format = 4;
    s.sig_rects = {{0, 99, 79, 17}, {0, 116, 125, 12}};
    s.sig_hashes = {prompt_atlas_signature(img.px.data(), img.w, s.sig_rects)};
    s.cells = {{PromptGlyph::Cross, 41, 117, 9, 11},
               {PromptGlyph::LR, 96, 117, 16, 11},
               {PromptGlyph::StickVertical, 115, 117, 9, 11},
               {PromptGlyph::Start, 1, 100, 37, 14}};
    return s;
}

// Inside a cell or its half-texel ring (the bilinear bleed of an enlarged image, cleared too).
static bool in_cells(const PromptAtlasSpec& s, int x, int y, int k) {
    const int m = k / 2;
    for (const PromptCell& c : s.cells)
        if (x >= c.x * k - m && x < (c.x + c.w) * k + m && y >= c.y * k - m && y < (c.y + c.h) * k + m) return true;
    return false;
}

static void signature_sees_only_its_region() {
    RgbaImage img = synthetic_atlas();
    const std::vector<PromptRect> R = {{0, 99, 79, 17}, {0, 116, 125, 12}};
    const uint64_t a = prompt_atlas_signature(img.px.data(), img.w, R);
    CHECK(a == prompt_atlas_signature(img.px.data(), img.w, R), "stable");
    img.px[(10 * 256 + 200) * 4 + 3] ^= 0xFF;  // outside the region
    CHECK(a == prompt_atlas_signature(img.px.data(), img.w, R), "outside change ignored");
    img.px[(120 * 256 + 50) * 4 + 3] ^= 0xFF;  // inside
    CHECK(a != prompt_atlas_signature(img.px.data(), img.w, R), "inside change seen");
    RgbaImage faint = synthetic_atlas();
    for (size_t i = 3; i < faint.px.size(); i += 4) if (faint.px[i] == 0xFF) faint.px[i] = 0x11;
    CHECK(a == prompt_atlas_signature(faint.px.data(), faint.w, R),
          "any non-zero alpha counts: the palette's alpha ramp differs per language");
    RgbaImage second = synthetic_atlas();
    second.px[(126 * 256 + 110) * 4 + 3] ^= 0xFF;  // in the second rectangle only
    CHECK(a != prompt_atlas_signature(second.px.data(), second.w, R), "the second rectangle counts");
}

static void compose_changes_only_cells(int k) {
    const RgbaImage base = synthetic_atlas();
    const PromptAtlasSpec spec = spec_for(base);
    RgbaImage img;
    img.w = 256 * k;
    img.h = 128 * k;
    img.px.resize(static_cast<size_t>(img.w) * img.h * 4);
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
            for (int c = 0; c < 4; c++)
                img.px[(static_cast<size_t>(y) * img.w + x) * 4 + c] = base.px[((y / k) * 256 + x / k) * 4 + c];
    const RgbaImage before = img;
    const ControlMap m = defaults();
    prompt_atlas_compose(spec, InputDevice::Xbox, PromptStyle::Color, m, img);
    bool outside_same = true, green = false, cross_opaque = false;
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            const size_t i = (static_cast<size_t>(y) * img.w + x) * 4;
            if (!in_cells(spec, x, y, k)) {
                for (int c = 0; c < 4; c++) if (img.px[i + c] != before.px[i + c]) outside_same = false;
            } else if (x >= 41 * k && x < 50 * k) {
                const uint8_t* p = &img.px[i];
                if (p[3] > 200) cross_opaque = true;
                if (p[3] > 200 && p[1] > p[0] + 40 && p[1] > p[2] + 40) green = true;
            }
        }
    CHECK(outside_same, "pixels outside the cells are untouched");
    bool clear_is_gray = true;  // transparent texels keep the atlas colour: no dark fringe when filtered
    for (int y = 100 * k; y < 114 * k; y++)
        for (int x = 1 * k; x < 38 * k; x++) {
            const uint8_t* p = &img.px[(static_cast<size_t>(y) * img.w + x) * 4];
            if (p[3] == 0 && (p[0] != 0x7F || p[1] != 0x7F || p[2] != 0x7F)) clear_is_gray = false;
        }
    CHECK(clear_is_gray, "cleared texels are (gray, 0) like the atlas's own transparent texels");
    CHECK(cross_opaque && green, "Xbox colour A is drawn in the cross cell");

    RgbaImage gray = before;
    prompt_atlas_compose(spec, InputDevice::Xbox, PromptStyle::Gray, m, gray);
    bool neutral = true;
    for (int y = 117 * k; y < 128 * k; y++)
        for (int x = 41 * k; x < 50 * k; x++) {
            const uint8_t* p = &gray.px[(static_cast<size_t>(y) * gray.w + x) * 4];
            if (p[3] > 0 && (std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]})) >= 24) neutral = false;
        }
    CHECK(neutral, "Gray: the A button has no colour");

    RgbaImage kb = before;
    prompt_atlas_compose(spec, InputDevice::Keyboard, PromptStyle::Gray, m, kb);
    bool stick_same = true;
    for (int y = 117 * k; y < 128 * k; y++)
        for (int x = 115 * k; x < 124 * k; x++)
            for (int c = 0; c < 4; c++) {
                const size_t i = (static_cast<size_t>(y) * kb.w + x) * 4 + c;
                if (kb.px[i] != before.px[i]) stick_same = false;
            }
    CHECK(stick_same, "a glyph without an icon (keyboard stick) keeps the game's pixels");
}

// A bilinear x4 enlargement spreads each texel half a texel outwards: the ring around a redrawn cell
// must not keep the old glyph's bleed (a faint box around the new icon).
static void compose_clears_the_bilinear_halo() {
    RgbaImage base;
    base.w = 256;
    base.h = 128;
    base.px.assign(256 * 128 * 4, 0);
    for (int y = 100; y < 114; y++)
        for (int x = 1; x < 38; x++) {
            uint8_t* p = &base.px[(y * 256 + x) * 4];
            p[0] = p[1] = p[2] = 0x7F;
            p[3] = 0xFF;
        }
    PromptAtlasSpec spec = spec_for(base);
    RgbaImage img = rgba_resample(base, 1024, 512);
    CHECK(img.px[(410 * 1024 + 2) * 4 + 3] > 0, "the enlargement bleeds left of the cell");
    prompt_atlas_compose(spec, InputDevice::Keyboard, PromptStyle::Gray, defaults(), img);
    bool halo = false;
    for (int y = 398; y < 458; y++)
        for (int x = 0; x < 160; x++) {
            const bool ring = x < 4 || x >= 152 || y < 400 || y >= 456;
            if (ring && img.px[(static_cast<size_t>(y) * 1024 + x) * 4 + 3] > 0) halo = true;
        }
    CHECK(!halo, "no bleed of the old pill around the cell");
}

static void variant_round_trip() {
    for (int icons = 0; icons < 2; icons++)
        for (int text = 0; text < 2; text++)
            for (int d = 0; d < 4; d++)
                for (int s = 0; s < 2; s++) {
                    const uint32_t v = prompt_atlas_variant(static_cast<InputDevice>(d), static_cast<PromptStyle>(s),
                                                            icons != 0, text != 0);
                    InputDevice dd;
                    PromptStyle ss;
                    bool ii = false, tt = false;
                    if (!icons && !text) {
                        CHECK(v == 0, "both off -> 0 (leave the texture alone)");
                        CHECK(!prompt_atlas_variant_decode(v, &dd, &ss, &ii, &tt), "0 does not decode");
                        continue;
                    }
                    CHECK(v != 0 && v <= 0x18, "never 0 when a flag is on; fits");
                    CHECK(prompt_atlas_variant_decode(v, &dd, &ss, &ii, &tt) && ii == (icons != 0) &&
                              tt == (text != 0), "flags decode back");
                    if (icons) {
                        CHECK(dd == static_cast<InputDevice>(d) && ss == static_cast<PromptStyle>(s),
                              "device and style decode back");
                    }
                }
    InputDevice dd;
    PromptStyle ss;
    bool ii, tt;
    CHECK(!prompt_atlas_variant_decode(0x19, &dd, &ss, &ii, &tt), "out of range");
    CHECK(!prompt_atlas_variant_decode(0x0C, &dd, &ss, &ii, &tt), "no such device");
}

// --- bar text ---------------------------------------------------------------------------------
// Label rows 0..10 and 11..21, a separator ':' in the glyph row, a word right of the cross cell.
static constexpr uint8_t kOld = 0x10;  // rgb of the "game's" label ink: anything redrawn is 0x7F

static RgbaImage label_atlas() {
    RgbaImage img;
    img.w = 256;
    img.h = 128;
    img.px.assign(256 * 128 * 4, 0);
    for (size_t i = 0; i < img.px.size(); i += 4) img.px[i] = img.px[i + 1] = img.px[i + 2] = 0x7F;
    auto ink = [&](int x0, int x1, int y0, int y1) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                uint8_t* p = &img.px[(y * 256 + x) * 4];
                p[0] = p[1] = p[2] = kOld;
                p[3] = ((x + y) % 3) ? 0xFF : 0x7C;
            }
    };
    ink(2, 40, 1, 8);       // "Menú"
    ink(41, 90, 1, 10);     // "Datos" touches it
    ink(100, 130, 1, 8);    // a word the font cannot draw
    ink(2, 60, 12, 19);     // "Confirmar"
    ink(1, 1, 120, 124);    // ':'
    ink(50, 70, 118, 126);  // "Hab." right of the cross cell (41..49)
    for (int y = 117; y < 128; y++)  // the cross cell: a glyph of the recognition region
        for (int x = 41; x < 50; x++) img.px[(y * 256 + x) * 4 + 3] = (x + y) % 2 ? 0xFF : 0;
    return img;
}

static PromptAtlasSpec label_spec(const RgbaImage& img) {
    PromptAtlasSpec s = spec_for(img);
    s.label_sig_rects = {{0, 0, 256, 99}, {125, 116, 131, 12}};
    BarLabels t{"test-es", prompt_atlas_signature(img.px.data(), img.w, s.label_sig_rects), 8, 0xFF, {}};
    t.rows.push_back({0, 10, 9, {{2, 40, "Menú"}, {41, 90, "Datos"}, {100, 130, "a\xEE\x80\x80"}}});
    t.rows.push_back({11, 21, 20, {{2, 60, "Confirmar"}}});
    t.rows.push_back({117, 127, 126, {{50, 70, "Hab."}}});
    s.labels = {t};
    s.separators = {117, 127, 126, {{1, 1, ":"}}};
    s.separator_cap = 7;
    return s;
}

static RgbaImage upscale(const RgbaImage& base, int k) {
    RgbaImage img;
    img.w = base.w * k;
    img.h = base.h * k;
    img.px.resize(static_cast<size_t>(img.w) * img.h * 4);
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
            for (int c = 0; c < 4; c++)
                img.px[(static_cast<size_t>(y) * img.w + x) * 4 + c] = base.px[((y / k) * base.w + x / k) * 4 + c];
    return img;
}

struct Box {
    int x0, x1, y0, y1;  // inclusive texels
};
static bool in_box(const Box& b, int x, int y, int k, int ring) {
    return x >= b.x0 * k - ring && x < (b.x1 + 1) * k + ring && y >= b.y0 * k - ring && y < (b.y1 + 1) * k + ring;
}
static bool has_new_ink(const RgbaImage& img, const Box& b, int k) {
    for (int y = b.y0 * k; y < (b.y1 + 1) * k; y++)
        for (int x = b.x0 * k; x < (b.x1 + 1) * k; x++) {
            const uint8_t* p = &img.px[(static_cast<size_t>(y) * img.w + x) * 4];
            if (p[3] > 0 && p[0] == 0x7F) return true;
        }
    return false;
}
static bool same_px(const RgbaImage& a, const RgbaImage& b, size_t i) { return std::equal(&a.px[i], &a.px[i] + 4, &b.px[i]); }

static void bar_text_redraws_words(int k) {
    const RgbaImage base = label_atlas();
    const PromptAtlasSpec spec = label_spec(base);
    RgbaImage img = upscale(base, k);
    const RgbaImage before = img;
    const uint32_t v = (2u << 20) | prompt_atlas_variant(InputDevice::Keyboard, PromptStyle::Gray, false, true);
    prompt_atlas_apply(spec, v, defaults(), img);
    const std::vector<Box> redrawn = {{2, 40, 0, 10}, {41, 90, 0, 10}, {2, 60, 11, 21}, {1, 1, 117, 127},
                                      {50, 70, 117, 127}};
    const Box missing{100, 130, 0, 10}, cross{41, 49, 117, 127};
    const int ring = k / 2;
    bool old_gone = true, outside_same = true, missing_same = true, cell_same = true, ring_clear = true;
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            const size_t i = (static_cast<size_t>(y) * img.w + x) * 4;
            const bool same = same_px(img, before, i);
            if (in_box(missing, x, y, k, 0)) {
                missing_same &= same;
                continue;
            }
            if (in_box(cross, x, y, k, 0)) {
                cell_same &= same;
                continue;
            }
            bool in = false, near = false;
            for (const Box& b : redrawn) {
                in |= in_box(b, x, y, k, 0);
                near |= in_box(b, x, y, k, ring);
            }
            if (in || near) {
                if (img.px[i] == kOld && img.px[i + 3] != 0) old_gone = false;
                if (!in && img.px[i + 3] != 0 && !in_box(cross, x, y, k, ring)) ring_clear = false;
            } else if (!same) {
                outside_same = false;
            }
        }
    CHECK(old_gone, "no old ink left in a redrawn box or its ring");
    CHECK(outside_same, "nothing changes outside the redrawn boxes and rings");
    CHECK(ring_clear, "new ink only inside the boxes (the rings are clear)");
    CHECK(missing_same, "a word the font cannot draw keeps the game's pixels");
    CHECK(cell_same, "icons off: the icon cells are byte-identical");
    for (const Box& b : redrawn) CHECK(has_new_ink(img, b, k), "every redrawn word has new ink (the touching one too)");
}

static void icons_on_text_off_keeps_labels() {
    const RgbaImage base = label_atlas();
    const PromptAtlasSpec spec = label_spec(base);
    RgbaImage img = base;
    prompt_atlas_apply(spec, (2u << 20) | prompt_atlas_variant(InputDevice::Xbox, PromptStyle::Color, true, false),
                       defaults(), img);
    bool labels_same = true, cell_changed = false;
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 256; x++) {
            const size_t i = (static_cast<size_t>(y) * 256 + x) * 4;
            const bool same = same_px(img, base, i);
            if (in_cells(spec, x, y, 1)) {
                cell_changed |= !same;
                continue;
            }
            labels_same &= same;
        }
    CHECK(labels_same, "text off: every label keeps the game's pixels");
    CHECK(cell_changed, "icons on: the cells are redrawn");
}

static void unknown_labels_redraw_separators_only() {
    const RgbaImage base = label_atlas();
    const PromptAtlasSpec spec = label_spec(base);
    RgbaImage img = base;
    prompt_atlas_apply(spec, (1u << 20) | prompt_atlas_variant(InputDevice::Keyboard, PromptStyle::Gray, false, true),
                       defaults(), img);
    bool words_same = true;
    for (int y = 0; y < 22; y++)
        for (int x = 0; x < 256; x++) words_same &= same_px(img, base, (static_cast<size_t>(y) * 256 + x) * 4);
    CHECK(words_same, "tag 1 (labels unknown): the labels stay the game's");
    CHECK(has_new_ink(img, {1, 1, 117, 127}, 1), "the separator is redrawn");
}

static void label_recognition() {
    const RgbaImage base = label_atlas();
    const PromptAtlasSpec spec = label_spec(base);
    CHECK(prompt_atlas_recognise(spec, base.px.data(), base.w) == 2, "the table's label signature -> tag 2");
    RgbaImage faint = base;
    for (size_t i = 3; i < faint.px.size(); i += 4) {
        if (faint.px[i] != 0) faint.px[i] = static_cast<uint8_t>(std::max(1, faint.px[i] / 2));
    }
    CHECK(prompt_atlas_signature(faint.px.data(), faint.w, spec.label_sig_rects) == spec.labels[0].sig,
          "a ramp at half the alpha has the same label signature");
    CHECK(prompt_atlas_recognise(spec, faint.px.data(), faint.w) == 2, "... and the same table");
    RgbaImage other = base;
    other.px[(5 * 256 + 200) * 4 + 3] = 0xFF;  // a label pixel no table knows
    CHECK(prompt_atlas_recognise(spec, other.px.data(), other.w) == 1, "unknown labels -> tag 1");
    RgbaImage alien = base;
    alien.px[(120 * 256 + 42) * 4 + 3] ^= 0xFF;  // part 1's region differs
    CHECK(prompt_atlas_recognise(spec, alien.px.data(), alien.w) == 0, "not the atlas -> 0");
}

int main() {
    signature_sees_only_its_region();
    compose_changes_only_cells(1);
    compose_changes_only_cells(4);
    compose_clears_the_bilinear_halo();
    variant_round_trip();
    for (int k : {1, 2, 3}) bar_text_redraws_words(k);
    icons_on_text_off_keeps_labels();
    unknown_labels_redraw_separators_only();
    label_recognition();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
