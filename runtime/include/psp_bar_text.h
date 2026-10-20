#pragma once
// Sharp help-bar text: the embedded bar font (Zen Maru Gothic Bold, SIL OFL 1.1, runtime/assets/fonts)
// and a word rasteriser that redraws a game's pre-rendered labels inside its texture, word by word, at
// the composition scale. The word tables are game data (games/<game>/runtime). Spec:
// docs/superpowers/specs/2026-10-20-pc-button-prompts-part1-1-bar-text-design.md.
#include "psp_rgba_image.h"

#include <cstdint>
#include <string>
#include <vector>

struct BarWord {
    int x0, x1;        // ink columns of the original word, inclusive (texels)
    std::string text;  // UTF-8, character for character as the game draws it
};

struct BarRow {
    int y0, y1, baseline;  // band (inclusive texels) and the first texel row below the letters
    std::vector<BarWord> words;
};

struct BarLabels {
    const char* name;  // e.g. "dxd114-es"
    uint64_t sig;      // prompt_atlas_signature() over the game's label rects
    int cap;           // cap height of the original letters (texels)
    uint8_t alpha;     // peak label alpha of that atlas
    std::vector<BarRow> rows;
};

// The embedded font parsed.
bool bar_font_ok();
// Every code point of `utf8` has a glyph in the bar font.
bool bar_font_has(const std::string& utf8);
// The pixel size (stbtt_ScaleForPixelHeight) whose 'H' is `cap_px` tall.
float bar_font_px_for_cap(float cap_px);
// Advance width of `utf8` at `px`, kerning included, not condensed.
float bar_text_width(const std::string& utf8, float px);

// Draws `utf8` with its ink's left edge at `x` and its baseline at `baseline` (the first pixel row below
// the letters), condensed horizontally by min(1, box_w / ink width) and never outside [x, x + box_w).
// Colour `gray`, alpha = coverage * alpha / 255, blended "over" what is there. False (nothing drawn)
// when a code point is missing from the font.
bool rgba_draw_text(RgbaImage& img, int x, int baseline, int box_w, float px, const std::string& utf8,
                    uint8_t gray, uint8_t alpha);
