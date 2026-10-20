#pragma once
// The button-prompt patcher (psp_ge_texpatch.h): a game module describes its help-bar atlas — size,
// format, a region whose alpha mask identifies it, and the cell of each PSP glyph — and the runtime
// redraws those cells for the player's input device and the prompt style. Spec:
// docs/superpowers/specs/2026-10-19-pc-button-prompts-part1-design.md.
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

struct PromptAtlasSpec {
    int width = 0, height = 0, format = 0;    // GE texture size and GE_TFMT_*
    int sig_x = 0, sig_y = 0, sig_w = 0, sig_h = 0;  // recognition region
    std::vector<uint64_t> sig_hashes;         // accepted prompt_atlas_signature() values
    std::vector<PromptCell> cells;
    uint8_t gray = 0x7F;  // the atlas's glyph colour: icon tint and the L/R slash
    uint8_t hole = 0x17;  // the dark interior of the atlas's glyphs: fills an icon's cut-out symbol
};

// FNV-1a 64 of the region's alpha mask (a >= 0x40 -> 1), row by row; `w` = image width.
uint64_t prompt_atlas_signature(const uint8_t* rgba, int w, int x, int y, int rw, int rh);

// Registers the patcher for `spec` (one atlas; a later call replaces it). PSPRECOMP_BUTTON_ICONS:
// gray (default) / color; "off" registers nothing. PSPRECOMP_PROMPTS_SIG_LOG=1 logs the signature of
// every candidate texture (to collect the hashes of a new language or release).
void psp_prompts_register_atlas(PromptAtlasSpec spec);

// Redraw every cell whose glyph has an icon for (d, s, map), scaled by img.w / spec.width; cells
// without an icon keep their pixels, and nothing outside the cells changes.
void prompt_atlas_compose(const PromptAtlasSpec& spec, InputDevice d, PromptStyle s, const ControlMap& map,
                          RgbaImage& img);

// Texture-patch variant of (device, style): 1 + device * 2 + style, never 0.
uint32_t prompt_atlas_variant(InputDevice d, PromptStyle s);
bool prompt_atlas_variant_decode(uint32_t v, InputDevice* d, PromptStyle* s);
