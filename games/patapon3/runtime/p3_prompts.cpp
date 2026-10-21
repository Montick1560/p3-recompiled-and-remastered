// p3_prompts.cpp — Patapon 3's prompt atlases (help bar, mission command list), for the PC button
// prompts (psp_prompt_atlas.h).
//
// Every help bar (hideout, barracks, forge, pause menus...) is a run of through-mode sprites cropping
// one pre-rendered atlas: 256x128 CLUT4, swizzled (DxD 1.1.4 Spanish: 0x092E5300, CLUT 0x092E9340;
// index 0 = 0x171717 opaque, 2..13 = gray 0x7F with an alpha ramp, 14/15 = red). It holds every
// action label of the current language, the START / SELECT pills and one row of glyphs. Cells below
// are inclusive texel ranges measured from a dump (build/tools/texdump.py, recon 2026-10-19). The
// game draws some cells rotated by 90 degrees through the UVs (the hideout's "L/R/+" uses the
// up/down D-pad turned sideways), which the drawn icons tolerate. The pill sprites start at row 101,
// so the pill cells are the pills' own extent (y 102..113). The atlas is recognised by the
// coverage of the pills (x 0..78, y 99..115) and the glyph row (x 0..124, y 116..127): the same in
// most languages (only the labels and the palette's alpha ramp differ, so the signature ignores
// alpha levels); a few lack a faint halo or edge texels (one hash each, below). The labels are
// redrawn sharp from the word tables of p3_prompt_labels.cpp: their coverage picks the language's
// table (none for Polish: the bar font lacks its diacritics, so its labels stay the game's).
//
// The mission command list ("Avanzar: □·□·□·○  Atacar: ○·○·□·○ ...", bottom of the screen) is one
// through-mode sprite PRIM cropping another pre-rendered atlas: 128x64 CLUT4, swizzled (DxD 1.1.4 Spanish
// and EU English: 0x092E4100, CLUT 0x092E5140; 16 x 8888, colour 0x7F7F7F with an alpha ramp, index 0
// transparent). It holds the command names of the language in rows of 11 texels (y 11k..11k+10, cap 8,
// baseline 11k+9; up to y 52) and one glyph row (y 53..63): ':' '·' and the four drums. The cells are the
// UV cells the game crops (10x10, recon 2026-10-20 with build/tools/texture_recon/recon_mission.py); the
// atlas is recognised by the coverage of the glyph row's x 0..57 (the same in most languages; Japanese and
// Russian differ in a few faint texels: one hash each);
// its names are redrawn from the word tables of p3_command_labels.cpp. Registered after the help bar, so
// the help bar keeps atlas index 0 (its tags and cache keys do not move).
//
// The drum-hit words (PATA / PON / CHAKA / DON flying out next to the hero on every drum hit) are one
// texture each: 128x128 CLUT4, swizzled, in VRAM (DxD 1.1.4 es and EU en, recon 2026-10-20 with
// build/tools/texture_recon/to_march.py + hitshots.py: PATA 0x041302C0, PON 0x041324C0, DON 0x041346C0,
// CHAKA 0x041368C0, 0x2200 apart, CLUT = texture + 0x2040). Each holds the word art, three faint rings and
// the drum's white PSP symbol in an opaque black disc (radius ~12 texels around (63, 93)). They are
// recognised by their whole coverage (the same in es and en). A cell covers both the game's symbol and the
// HD pack's (ui/quest/, 1024x1024, a symbol ~1.5 texels larger) and is filled with the disc's black before
// the icon is drawn (cell_bg), so the redrawn symbol leaves no hole. Registered after the command list.
#include "p3_prompts.h"
#include "p3_command_labels.h"
#include "p3_prompt_labels.h"

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
        0xBDBB50705ED7F138ull,  // es, en (EU and US), it, de; DxD 1.1.4 / 1.1.5b and vanilla EU; pt (vanilla)
        0xC81C007599244E54ull,  // fr; ru (vanilla): 98 alpha-4 halo texels of the red D-pads are clear
        0x910C01FEB9FAECBDull,  // pl (vanilla): the same halo + 5 faint texels
        0x0A7562B382841968ull,  // nl (vanilla): 8 faint texels
        0x3036744C716933C2ull,  // ja (DxD, at 0x09341480): 8 faint edge texels
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
    s.label_sig_rects = p3_label_sig_rects();
    s.labels = p3_bar_labels();
    s.separators = p3_bar_separators();
    s.separator_cap = 7;
    psp_prompts_register_atlas(std::move(s));

    PromptAtlasSpec c;
    c.name = "command-list";
    c.width = 128;
    c.height = 64;
    c.format = GE_TFMT_CLUT4;
    c.sig_rects = {{0, 53, 58, 11}};
    c.sig_hashes = {
        0xEE3E34178623FCD2ull,  // es, en (EU and US), fr, it, de; DxD 1.1.4 / 1.1.5b and vanilla EU; pt, pl, nl (vanilla)
        0x0CEDF975E89D9C77ull,  // ja (DxD, at 0x09340280): 3 faint texels of the triangle and cross
        0xF6F6ED03BD6F714Dull,  // ru (vanilla): 5 faint texels of the cross are clear
    };
    c.gray = 0x7F;
    c.hole = 0x17;
    c.cells = {
        {PromptGlyph::Square, 14, 53, 10, 10},
        {PromptGlyph::Circle, 25, 53, 10, 10},
        {PromptGlyph::Triangle, 36, 53, 10, 10},
        {PromptGlyph::Cross, 47, 53, 10, 10},
    };
    c.label_sig_rects = p3_command_label_sig_rects();
    c.labels = p3_command_labels();
    c.separators = p3_command_separators();
    c.separator_cap = 7;
    psp_prompts_register_atlas(std::move(c));

    struct Drum {
        const char* name;
        uint64_t sig;
        PromptCell cell;
    };
    static const Drum kDrums[] = {
        {"drum-pata", 0x80A7B80AFBB1DF46ull, {PromptGlyph::Square, 56, 85, 15, 15}},
        {"drum-pon", 0xFB077A3947637FF9ull, {PromptGlyph::Circle, 55, 86, 15, 15}},
        {"drum-chaka", 0x1447FFDE5936A2ABull, {PromptGlyph::Triangle, 55, 85, 17, 15}},
        {"drum-don", 0x05B24690279EFF6Full, {PromptGlyph::Cross, 56, 86, 15, 15}},
    };
    for (const Drum& d : kDrums) {
        PromptAtlasSpec w;
        w.name = d.name;
        w.width = 128;
        w.height = 128;
        w.format = GE_TFMT_CLUT4;
        w.sig_rects = {{0, 0, 128, 128}};
        w.sig_hashes = {d.sig};  // es, en (DxD 1.1.4)
        w.cells = {d.cell};
        w.gray = 0xFE;  // the symbol is white
        w.hole = 0x00;  // an icon's cut-out shows the disc's black
        w.cell_bg = 0xFF000000u;
        psp_prompts_register_atlas(std::move(w));
    }
}
