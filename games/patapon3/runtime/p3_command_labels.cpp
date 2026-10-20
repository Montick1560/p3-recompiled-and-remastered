// p3_command_labels.cpp — the words of Patapon 3's mission command-list atlas, per language and release.
//
// Transcribed character for character from atlas dumps (build/tools/texture_recon/recon_mission.py, then
// build/tools/atlas_words.py --layout command: word runs split at gaps of >= 2 empty texel columns, read
// off x6 strips; touching words split at their faintest column). Rows: band y0..y1 (inclusive texels),
// baseline = the first texel row below the letters; words: ink columns x0..x1. The label signature (the
// coverage of p3_command_label_sig_rects(), atlas_words.py --layout command --sig) picks the table; cap
// height 8 texels; alpha = the dump's peak label alpha. Recon 2026-10-20.
#include "p3_command_labels.h"

// clang-format off
const std::vector<BarLabels>& p3_command_labels() {
    static const std::vector<BarLabels> t = {
        // DxD 1.1.4 Spanish
        {"dxd114-es", 0x25BCB24B42D5A964ull, 8, 255, {
            {0, 10, 9, {{40, 70, "Defen."}, {76, 109, "Cargar"}}},
            {11, 21, 20, {{1, 32, "Atacar"}, {38, 63, "Atrás"}, {66, 101, "Invocar"}}},
            {22, 32, 31, {{35, 62, "Saltar"}, {66, 105, "Avanzar"}}},
            {33, 43, 42, {{1, 40, "Purificar"}}},
        }},
        // DxD 1.1.4 EU English
        {"dxd114-en", 0x145412D96DAB49B3ull, 8, 255, {
            {0, 10, 9, {{1, 40, "Onward"}, {43, 77, "Defend"}, {80, 116, "Charge"}}},
            {11, 21, 20, {{1, 32, "Attack"}, {35, 70, "Retreat"}, {72, 113, "Summon"}}},
            {22, 32, 31, {{1, 25, "Party"}, {28, 54, "Jump"}}},
        }},
    };
    return t;
}
// clang-format on

// ':' x 0..1 (ink y 56..60) of the glyph row (y 53..62, the game's crop). The '·' (x 7..9, a round 3x3
// dot at y 57..59) keeps the game's pixels: the bar font's dots sit two texels higher than the colon's
// middle, where the game centres it, and are half its size.
BarRow p3_command_separators() { return {53, 62, 61, {{0, 1, ":"}}}; }

// Labels: the name rows y 0..52 and the glyph row right of the drum glyphs (x >= 58).
std::vector<PromptRect> p3_command_label_sig_rects() { return {{0, 0, 128, 53}, {58, 53, 70, 11}}; }
