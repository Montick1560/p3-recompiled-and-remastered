#pragma once
// The button-prompt patcher (psp_ge_texpatch.h): a game module describes its help-bar atlas — size,
// format, a region whose alpha mask identifies it, and the cell of each PSP glyph — and the runtime
// redraws those cells for the player's input device and the prompt style. It may also describe the
// atlas's labels per language (word tables, psp_bar_text.h): those are redrawn sharp with the bar font.
// Specs: docs/superpowers/specs/2026-10-19-pc-button-prompts-part1-design.md, part 1.1
// docs/superpowers/specs/2026-10-20-pc-button-prompts-part1-1-bar-text-design.md.
#include "psp_bar_text.h"
#include "psp_controls.h"
#include "psp_input_device.h"
#include "psp_prompts.h"
#include "psp_rgba_image.h"

#include <cstdint>
#include <vector>

struct PromptCell {
    PromptGlyph glyph;
    int x, y, w, h;  // texels of the original atlas
};

struct PromptRect {
    int x, y, w, h;
};

struct PromptAtlasSpec {
    int width = 0, height = 0, format = 0;    // GE texture size and GE_TFMT_*
    std::vector<PromptRect> sig_rects;        // recognition regions (texels)
    std::vector<uint64_t> sig_hashes;         // accepted prompt_atlas_signature() values
    std::vector<PromptCell> cells;
    uint8_t gray = 0x7F;  // the atlas's glyph colour: icon tint and the L/R slash
    uint8_t hole = 0x17;  // the dark interior of the atlas's glyphs: fills an icon's cut-out symbol
    // Labels: recognition regions of the language (none of sig_rects), one word table per known
    // label signature, and the language-independent separators (': / &') of the glyph row.
    std::vector<PromptRect> label_sig_rects;
    std::vector<BarLabels> labels;
    BarRow separators{0, -1, 0, {}};
    int separator_cap = 8;  // cap height of the separators (texels)
};

// FNV-1a 64 of the regions' coverage mask (alpha != 0 -> 1), row by row; `w` = image width. Only
// coverage counts: a palette's alpha ramp may differ per language while every glyph sits in place.
uint64_t prompt_atlas_signature(const uint8_t* rgba, int w, const std::vector<PromptRect>& rects);

// Registers the patcher for `spec` (one atlas; a later call replaces it). PSPRECOMP_BUTTON_ICONS:
// gray (default) / color / "off" (the game's PSP icons); PSPRECOMP_BAR_TEXT=off keeps the game's
// labels; both off registers nothing. PSPRECOMP_PROMPTS_SIG_LOG=1 logs the signatures of every
// candidate texture (to collect the hashes of a new language or release).
void psp_prompts_register_atlas(PromptAtlasSpec spec);

// The recognition tag of an atlas (psp_ge_texpatch.h): 0 = not spec's atlas (sig_rects), else 2 + i for
// the label table i whose signature matches (label_sig_rects), else 1 (labels unknown: icons only).
uint32_t prompt_atlas_recognise(const PromptAtlasSpec& spec, const uint8_t* rgba, int w);

// Composes the seam's value v = tag << 20 | prompt_atlas_variant(): the icons when its icons flag is on,
// then the labels (tag >= 2) and separators when its text flag is on. Icon cells are never touched by
// the text.
void prompt_atlas_apply(const PromptAtlasSpec& spec, uint32_t v, const ControlMap& map, RgbaImage& img);

// Redraw every cell whose glyph has an icon for (d, s, map), scaled by img.w / spec.width; cells
// without an icon keep their pixels, and nothing outside the cells changes.
void prompt_atlas_compose(const PromptAtlasSpec& spec, InputDevice d, PromptStyle s, const ControlMap& map,
                          RgbaImage& img);

// Texture-patch variant: (icons ? 1 + device * 2 + style : 0) | (text ? 0x10 : 0); never 0 when a flag
// is on. Decode is false for 0 or an out-of-range value; d / s are only meaningful with icons.
uint32_t prompt_atlas_variant(InputDevice d, PromptStyle s, bool icons, bool text);
bool prompt_atlas_variant_decode(uint32_t v, InputDevice* d, PromptStyle* s, bool* icons, bool* text);

// Redraws the labels (when `labels` is not null) and the separators with the bar font, scaled by
// (sx, sy): each word box and its half-texel ring is cleared, then every word is drawn at its box
// (left-aligned on its baseline, cap height labels->cap, alpha labels->alpha; separators at `cap`, `alpha`).
// A word with a character the font lacks keeps its pixels and joins `keep`; nothing in a `keep` rect
// (texels) or its ring is touched. All clears happen before all draws (words may touch).
void bar_text_compose(const BarLabels* labels, const BarRow& separators, int cap, uint8_t gray, uint8_t alpha,
                      std::vector<PromptRect> keep, double sx, double sy, RgbaImage& img);
