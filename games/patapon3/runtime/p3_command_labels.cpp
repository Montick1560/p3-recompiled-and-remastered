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
        // DxD 1.1.4 Spanish; DxD 1.1.5b and vanilla EU Spanish
        {"dxd114-es", 0x25BCB24B42D5A964ull, 8, 255, {
            {0, 10, 9, {{40, 70, "Defen."}, {76, 109, "Cargar"}}},
            {11, 21, 20, {{1, 32, "Atacar"}, {38, 63, "Atrás"}, {66, 101, "Invocar"}}},
            {22, 32, 31, {{35, 62, "Saltar"}, {66, 105, "Avanzar"}}},
            {33, 43, 42, {{1, 40, "Purificar"}}},
        }},
        // DxD 1.1.4 EU and US English; DxD 1.1.5b and vanilla EU English
        {"dxd114-en", 0x145412D96DAB49B3ull, 8, 255, {
            {0, 10, 9, {{1, 40, "Onward"}, {43, 77, "Defend"}, {80, 116, "Charge"}}},
            {11, 21, 20, {{1, 32, "Attack"}, {35, 70, "Retreat"}, {72, 113, "Summon"}}},
            {22, 32, 31, {{1, 25, "Party"}, {28, 54, "Jump"}}},
        }},
        // DxD 1.1.4 French
        {"dxd114-fr", 0xE3F955217049E438ull, 8, 255, {
            {0, 10, 9, {{1, 37, "Marche"}, {42, 60, "Déf."}, {64, 98, "Charge"}}},
            {11, 21, 20, {{1, 16, "Att."}, {20, 52, "Arrière"}, {56, 83, "Invoc."}, {85, 106, "Part."}}},
            {22, 32, 31, {{1, 31, "Sauter"}}},
        }},
        // DxD 1.1.4 Italian
        {"dxd114-it", 0xDDC6C980FA75957Full, 8, 255, {
            {0, 10, 9, {{1, 35, "Avanza"}, {38, 70, "Difendi"}, {73, 103, "Carica"}}},
            {11, 21, 20, {{1, 36, "Attacca"}, {39, 63, "Ritira"}, {82, 109, "Evoca"}}},
            {22, 32, 31, {{30, 53, "Salta"}, {57, 92, "Purifica"}}},
        }},
        // DxD 1.1.4 German
        {"dxd114-de", 0x9A62014C11BDC7AAull, 8, 255, {
            {0, 10, 9, {{1, 43, "Vorwärts"}, {46, 83, "Abwehr"}, {87, 115, "Laden"}}},
            {11, 21, 20, {{1, 33, "Angriff"}, {37, 76, "Rückzug"}, {82, 119, "Beschw."}}},
            {22, 32, 31, {{29, 69, "Springen"}, {73, 114, "Reinigen"}}},
        }},
        // DxD 1.1.4 Japanese (kana cap 7: the ink is 9 texels tall)
        {"dxd114-jp", 0x5D67FDB3EFB97F34ull, 7, 255, {
            {0, 10, 9, {{0, 26, "すすめ"}, {29, 55, "まもれ"}, {57, 83, "ためろ"}, {86, 123, "ショカーン"}}},
            {11, 21, 20, {{0, 26, "せめろ"}, {29, 53, "にげろ"}, {55, 72, "ジャ"}, {75, 92, "ンプ"}, {95, 120, "さわげ"}}},
        }},
        // vanilla EU Portuguese
        {"van-pt", 0x8A900217936B4FB8ull, 8, 255, {
            {0, 10, 9, {{1, 40, "Avançar"}, {43, 86, "Defender"}, {88, 123, "Invocar"}}},
            {11, 21, 20, {{1, 32, "Atacar"}, {35, 65, "Retirar"}, {67, 106, "Purificar"}}},
            {22, 32, 31, {{1, 28, "Saltar"}, {63, 105, "Carregar"}}},
        }},
        // vanilla EU Dutch
        {"van-nl", 0xD5D5AB51FEC2B2F3ull, 8, 255, {
            {0, 10, 9, {{1, 52, "Voorwaarts"}, {55, 106, "Verdedigen"}}},
            {11, 21, 20, {{1, 38, "Opladen"}, {41, 85, "Aanvallen"}}},
            {22, 32, 31, {{1, 58, "Terugtrekken"}, {61, 99, "Springen"}}},
            {33, 43, 42, {{1, 43, "Oproepen"}, {45, 89, "Herstellen"}}},
        }},
        // vanilla EU Russian
        {"van-ru", 0x488C7D5E3325B657ull, 8, 255, {
            {0, 10, 9, {{1, 58, "Наступление"}, {62, 100, "Оборона"}}},
            {11, 21, 20, {{1, 30, "Штурм"}, {32, 58, "Атака"}, {61, 118, "Отступление"}}},
            {22, 32, 31, {{1, 31, "Группа"}, {34, 68, "Прыжок"}, {73, 124, "Призывание"}}},
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
