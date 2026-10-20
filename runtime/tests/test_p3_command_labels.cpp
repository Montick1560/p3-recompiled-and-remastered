// Unit tests for Patapon 3's mission command-list word tables (games/patapon3/runtime/p3_command_labels.cpp):
// one table per distinct (release, language) atlas dump, every word drawable by the bar font and fitting
// its box at >= 75 % condensing, boxes inside the 128x64 atlas's label rows and never overlapping.
#include "p3_command_labels.h"
#include "psp_bar_text.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// One per (release, language) atlas whose label coverage differs (recon of part 2, 2026-10-20): DxD 1.1.4
// US English, DxD 1.1.5b es / en and vanilla EU en, de, fr, it, es share the DxD 1.1.4 atlases; vanilla adds
// pt, nl, ru (and pl, left to the game: the bar font lacks the ś ć ł of two of its command names).
static const std::vector<std::string> kExpected = {
    "dxd114-es", "dxd114-en", "dxd114-fr", "dxd114-it", "dxd114-de", "dxd114-jp", "van-pt", "van-nl", "van-ru",
};

// Rows of names end at y 52 (the glyph row is y 53..63); the separators live in the glyph row.
// `words`: a box much wider than its word means a misread or a wrong split (not for the separators).
static void check_row(const char* table, const BarRow& r, int cap, int ymin, int ymax, bool words = true) {
    CHECK(r.y0 >= ymin && r.y0 <= r.y1 && r.y1 <= ymax, "row inside its area");
    CHECK(r.baseline > r.y0 && r.baseline <= r.y1 + 1, "baseline inside the band");
    const float px = bar_font_px_for_cap(static_cast<float>(cap * 4));
    int prev = -1;
    for (size_t i = 0; i < r.words.size(); i++) {
        const BarWord& w = r.words[i];
        const std::string what = std::string(table) + " \"" + w.text + "\"";
        if (!bar_font_has(w.text)) std::printf("  missing glyph: %s\n", what.c_str());
        CHECK(bar_font_has(w.text), "every character in the bar font");
        CHECK(w.x0 <= w.x1 && w.x0 >= 0 && w.x1 < 128, "word box inside the atlas");
        CHECK(w.x0 > prev, "words of a row in order, never overlapping");
        prev = w.x1;
        const float natural = static_cast<float>(bar_text_ink_width(w.text, px)), box = (w.x1 - w.x0 + 1) * 4.0f;
        if (natural > box / 0.75f) std::printf("  too wide: %s %.0f px for a %.0f px box\n", what.c_str(), natural, box);
        CHECK(natural <= box / 0.75f, "fits its box at >= 75 % condensing");
        if (!words) continue;
        if (natural < box * 0.6f) std::printf("  box too wide: %s %.0f px in a %.0f px box\n", what.c_str(), natural, box);
        CHECK(natural >= box * 0.6f, "the box fits its word (no misread / mis-split)");
    }
}

int main() {
    CHECK(bar_font_ok(), "bar font");
    const std::vector<BarLabels>& t = p3_command_labels();
    std::vector<std::string> names;
    std::set<uint64_t> sigs;
    for (const BarLabels& l : t) {
        names.push_back(l.name);
        CHECK(sigs.insert(l.sig).second, "label signatures unique");
        CHECK(l.cap >= 6 && l.cap <= 10, "cap height of an 11-texel row");
        CHECK(l.alpha > 0, "peak alpha measured");
        size_t words = 0;
        for (const BarRow& r : l.rows) {
            check_row(l.name, r, l.cap, 0, 52);
            words += r.words.size();
        }
        if (words < 7) std::printf("  %s: %zu words\n", l.name, words);
        CHECK(words >= 7, "a whole atlas transcribed (>= 7 command names)");
    }
    CHECK(names == kExpected, "one table per distinct (release, language) dump");
    const BarRow sep = p3_command_separators();
    CHECK(sep.words.size() == 1 && sep.words[0].text == ":", "separator : (the dot keeps the game's pixels)");
    check_row("separators", sep, 5, 53, 62, false);
    // The label rects never overlap the atlas signature's rect (the glyph row's x 0..57, y 53..63), so
    // the atlas signature stays language independent; they stay inside the 128x64 atlas.
    const std::vector<PromptRect> rects = p3_command_label_sig_rects();
    CHECK(!rects.empty(), "label rects");
    for (const PromptRect& r : rects) {
        CHECK(!(r.x < 58 && r.x + r.w > 0 && r.y < 64 && r.y + r.h > 53), "label rects apart from the atlas rect");
        CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= 128 && r.y + r.h <= 64, "label rects inside the atlas");
    }
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
