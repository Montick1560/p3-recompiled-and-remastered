// Unit tests for Patapon 3's help-bar word tables (games/patapon3/runtime/p3_prompt_labels.cpp): one
// table per distinct (release, language) atlas dump, every word drawable by the bar font and fitting
// its box at >= 75 % condensing, boxes inside their rows and never overlapping.
#include "p3_prompt_labels.h"
#include "psp_bar_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// One per (release, language) atlas whose label coverage differs (recon of part 1.1, 2026-10-20): DxD
// 1.1.5b and vanilla EU share the DxD 1.1.4 atlases of es, en, fr, it, de; vanilla adds pt, nl, ru (and pl,
// left to the game: the bar font lacks its diacritics).
static const std::vector<std::string> kExpected = {
    "dxd114-es", "dxd114-en", "dxd114-us", "dxd114-fr", "dxd114-it", "dxd114-de", "dxd114-jp",
    "van-pt", "van-nl", "van-ru",
};

static void check_row(const char* table, const BarRow& r, int cap) {
    CHECK(r.y0 >= 0 && r.y0 <= r.y1 && r.y1 < 128, "row inside the atlas");
    CHECK(r.baseline > r.y0 && r.baseline <= r.y1 + 1, "baseline inside the band");
    const float px = bar_font_px_for_cap(static_cast<float>(cap * 4));
    int prev = -1;
    for (size_t i = 0; i < r.words.size(); i++) {
        const BarWord& w = r.words[i];
        const std::string what = std::string(table) + " \"" + w.text + "\"";
        if (!bar_font_has(w.text)) std::printf("  missing glyph: %s\n", what.c_str());
        CHECK(bar_font_has(w.text), "every character in the bar font");
        CHECK(w.x0 <= w.x1 && w.x0 >= 0 && w.x1 < 256, "word box inside the atlas");
        CHECK(w.x0 > prev, "words of a row in order, never overlapping");
        prev = w.x1;
        const float natural = static_cast<float>(bar_text_ink_width(w.text, px)), box = (w.x1 - w.x0 + 1) * 4.0f;
        if (natural > box / 0.75f) std::printf("  too wide: %s %.0f px for a %.0f px box\n", what.c_str(), natural, box);
        CHECK(natural <= box / 0.75f, "fits its box at >= 75 % condensing");
        // A box much wider than its word means a misread word or a wrong split (the recon gave 0.71..1.33).
        if (natural < box * 0.6f) std::printf("  box too wide: %s %.0f px in a %.0f px box\n", what.c_str(), natural, box);
        CHECK(natural >= box * 0.6f, "the box fits its word (no misread / mis-split)");
    }
}

int main() {
    CHECK(bar_font_ok(), "bar font");
    const std::vector<BarLabels>& t = p3_bar_labels();
    std::vector<std::string> names;
    std::set<uint64_t> sigs;
    for (const BarLabels& l : t) {
        names.push_back(l.name);
        CHECK(sigs.insert(l.sig).second, "label signatures unique");
        CHECK(l.cap >= 6 && l.cap <= 10, "cap height of an 11-texel row");
        CHECK(l.alpha > 0, "peak alpha measured");
        size_t words = 0;
        for (const BarRow& r : l.rows) {
            check_row(l.name, r, l.cap);
            words += r.words.size();
        }
        if (words < 40) std::printf("  %s: %zu words\n", l.name, words);
        CHECK(words >= 40, "a whole atlas transcribed (>= 40 words)");
    }
    CHECK(names == kExpected, "one table per distinct (release, language) dump");
    const BarRow sep = p3_bar_separators();
    CHECK(sep.words.size() == 3, "separators : / &");
    check_row("separators", sep, 7);
    // The label rects never overlap part 1's recognition rects (pills x 0..78 y 99..115, glyphs x 0..124
    // y 116..127), so the atlas signature stays language independent.
    for (const PromptRect& r : p3_label_sig_rects()) {
        const bool pills = r.x < 79 && r.x + r.w > 0 && r.y < 116 && r.y + r.h > 99;
        const bool glyphs = r.x < 125 && r.y + r.h > 116;
        CHECK(!pills && !glyphs, "label rects apart from the atlas rects");
    }
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
