// p3_prompts.cpp — Patapon 3's help-bar atlas, for the PC button prompts (psp_prompt_atlas.h).
//
// Every help bar (hideout, barracks, forge, pause menus...) is a run of through-mode sprites cropping
// one pre-rendered atlas: 256x128 CLUT4, swizzled (DxD 1.1.4 Spanish: 0x092E5300, CLUT 0x092E9340;
// index 0 = 0x171717 opaque, 2..13 = gray 0x7F with an alpha ramp, 14/15 = red). It holds every
// action label of the current language, the START / SELECT pills and one row of glyphs. Cells below
// are inclusive texel ranges measured from a dump (build/tools/texdump.py, recon 2026-10-19). The
// game draws some cells rotated by 90 degrees through the UVs (the hideout's "L/R/+" uses the
// up/down D-pad turned sideways), which the drawn icons tolerate. The pill sprites start at row 101,
// so the pill cells are the pills' own extent (y 102..113). The atlas is recognised by the
// coverage of the pills (x 0..78, y 99..115) and the glyph row (x 0..124, y 116..127): identical in
// the Spanish, English, French, Italian and German atlases (only the labels and the palette's alpha
// ramp differ, so the signature ignores alpha levels).
#include "p3_prompts.h"

#include "psp_ge_constants.h"
#include "psp_prompt_atlas.h"

#include <utility>

void p3_prompts_register() {
    PromptAtlasSpec s;
    s.width = 256;
    s.height = 128;
    s.format = GE_TFMT_CLUT4;
    s.sig_rects = {{0, 99, 79, 17}, {0, 116, 125, 12}};
    s.sig_hashes = {
        0xBDBB50705ED7F138ull,  // DxD 1.1.4: Spanish, English, French, Italian, German
    };
    s.gray = 0x7F;
    s.hole = 0x17;
    s.cells = {
        {PromptGlyph::Square, 19, 117, 9, 11},
        {PromptGlyph::Circle, 30, 117, 9, 11},
        {PromptGlyph::Cross, 41, 117, 9, 11},
        {PromptGlyph::Triangle, 52, 117, 9, 11},
        {PromptGlyph::DpadAll, 63, 117, 9, 11},
        {PromptGlyph::DpadVertical, 74, 117, 9, 11},
        {PromptGlyph::DpadUp, 85, 117, 9, 11},
        {PromptGlyph::LR, 96, 117, 16, 11},
        {PromptGlyph::StickVertical, 115, 117, 9, 11},
        {PromptGlyph::Start, 1, 102, 37, 12},
        {PromptGlyph::Select, 41, 102, 37, 12},
    };
    psp_prompts_register_atlas(std::move(s));
}
