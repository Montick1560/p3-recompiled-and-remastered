// Toast texts, batching and motion (psp_toast.h).
#include "psp_toast.h"

#include <cmath>
#include <cstdio>
#include <iterator>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// The five tiers in catalogue order (psp_ach_toasts ranks them by position in this array).
static const PspAchRarity kTiers[] = {
    {"comun", "Común", "Common", 0x231815u, 4000},     {"raro", "Raro", "Rare", 0x7E3FC7u, 4000},
    {"especial", "Especial", "Special", 0xE5601Eu, 4500}, {"epico", "Épico", "Epic", 0xB0177Eu, 5000},
    {"legendario", "Legendario", "Legendary", 0x4E9A2Bu, 6000}};
static const PspAchRarity* const kComun = &kTiers[0];
static const PspAchRarity* const kRaro = &kTiers[1];
static const PspAchRarity* const kEpico = &kTiers[3];
static const PspAchRarity* const kLegend = &kTiers[4];

static bool yes(const std::string&) { return true; }
static bool no(const std::string&) { return false; }

static void card_per_achievement() {
    const std::vector<ToastIn> in = {{"x", "Señor del dragón", "Derrota al dragón.", kEpico},
                                     {"y", "El despertar", "Completa el prólogo.", kComun}};
    const auto t = psp_ach_toasts(in, "D:/g/assets/achievements/", "es", yes);
    CHECK(t.size() == 2, "two cards");
    CHECK(t[0].title == "¡Logro Épico!" && t[0].rgb == 0xB0177Eu && t[0].ms == 5000, "epic ribbon, colour, time");
    CHECK(t[0].name == "Señor del dragón" && t[0].desc == "Derrota al dragón.", "texts");
    CHECK(t[0].image == "D:/g/assets/achievements/x.jpg", "own image");
    CHECK(t[1].title == "¡Logro Común!" && t[1].rgb == 0x231815u && t[1].ms == 4000, "common");
    const auto d = psp_ach_toasts({in[0]}, "D:/g/assets/achievements/", "es", no);
    CHECK(d[0].image == "D:/g/assets/achievements/_default.jpg", "default image when the file is missing");
    const auto e = psp_ach_toasts({in[0]}, "i/", "en", yes);
    CHECK(e[0].title == "Epic achievement!", "english ribbon");
    CHECK(psp_ach_toasts({}, "i/", "es", yes).empty(), "nothing");
    CHECK(psp_ach_toasts({in[0], in[1], in[1]}, "i/", nullptr, yes).size() == 3, "three stay separate");
}

static void summary_over_three() {
    const std::vector<ToastIn> in = {{"a", "A", "", kRaro},     {"b", "B", "", kComun}, {"l", "Leyenda", "", kLegend},
                                     {"c", "C", "", kRaro},     {"d", "D", "", kComun}};
    const auto t = psp_ach_toasts(in, "i/", "es", yes);
    CHECK(t.size() == 1, "one summary");
    CHECK(t[0].rgb == 0x4E9A2Bu && t[0].ms == 6000, "highest tier's colour and time");
    CHECK(t[0].title == "¡5 logros!" && t[0].name == "Leyenda" && t[0].desc == "y 4 más", "spanish summary");
    CHECK(t[0].image == "i/l.jpg", "highest tier's image");
    const auto e = psp_ach_toasts(in, "i/", "en", yes);
    CHECK(e[0].title == "5 achievements!" && e[0].desc == "and 4 more", "english summary");
}

static void pose_curve() {
    const int64_t ms = 4000;
    CHECK(psp_toast_pose(0, ms).drop == -1.2f, "starts above");
    CHECK(std::fabs(psp_toast_pose(600, ms).drop) < 0.01f, "settled at 600 ms");
    bool over = false;
    for (int64_t t = 300; t <= 600; t += 10) over = over || psp_toast_pose(t, ms).drop > 0.0f;
    CHECK(over, "one overshoot between 300 and 600 ms");
    const ToastPose p0 = psp_toast_pose(0, ms), p8 = psp_toast_pose(800, ms);
    CHECK(p0.medal == 0.0f && p0.ribbon == 0.0f && p0.rule == 0.0f, "reveals start hidden");
    CHECK(p8.medal == 1.0f && p8.ribbon == 1.0f && p8.rule == 1.0f, "revealed from 800 ms");
    CHECK(psp_toast_pose(2000, ms).alpha == 1.0f, "solid");
    CHECK(psp_toast_pose(3850, ms).alpha < 0.5f, "fading out");
    CHECK(psp_toast_pose(4000, ms).alpha == 0.0f && psp_toast_pose(9000, ms).alpha == 0.0f, "gone");
    bool smooth = true;
    for (int64_t t = 16; t < 600; t += 16) smooth = smooth && psp_toast_pose(t, ms).drop != psp_toast_pose(t - 16, ms).drop;
    for (int64_t t = 266; t < 650; t += 16) smooth = smooth && psp_toast_pose(t, ms).ribbon != psp_toast_pose(t - 16, ms).ribbon;
    CHECK(smooth, "moves every 16 ms frame");
}

static void fade() {
    CHECK(psp_toast_alpha(0, kToastMs) == 0.0f, "starts transparent");
    CHECK(psp_toast_alpha(125, kToastMs) > 0.45f && psp_toast_alpha(125, kToastMs) < 0.55f, "fading in");
    CHECK(psp_toast_alpha(250, kToastMs) == 1.0f && psp_toast_alpha(3700, kToastMs) == 1.0f, "solid");
    CHECK(psp_toast_alpha(3850, kToastMs) > 0.45f && psp_toast_alpha(3850, kToastMs) < 0.55f, "fading out");
    CHECK(psp_toast_alpha(kToastMs, kToastMs) == 0.0f && psp_toast_alpha(99999, kToastMs) == 0.0f, "gone");
    CHECK(psp_toast_alpha(5500, 6000) > 0.9f, "a longer toast is still up");
}

// F7 preview (PSPRECOMP_ACH_PREVIEW=1): one real entry of each tier in turn, then all tiers at once (summary).
static void preview_cycle() {
    PspAchCatalogue c;
    c.rarities.assign(std::begin(kTiers), std::end(kTiers));
    auto add = [&c](const char* id, const char* rarity) {
        PspAchEntry e;
        e.id = id;
        e.name_es = std::string("es_") + id;
        e.name_en = std::string("en_") + id;
        e.desc_es = "d";
        e.rarity = rarity;
        c.entries.push_back(e);
    };
    add("c1", "comun"), add("e1", "epico"), add("c2", "comun"), add("l1", "legendario"), add("r1", "raro");
    const auto s0 = psp_ach_preview(c, 0, "es");
    CHECK(s0.size() == 1 && s0[0].id == "c1" && s0[0].rarity == &c.rarities[0] && s0[0].name == "es_c1", "step 0: first comun");
    const auto s3 = psp_ach_preview(c, 3, "en");
    CHECK(s3.size() == 1 && s3[0].id == "e1" && s3[0].name == "en_e1", "step 3: first epico");
    const auto s2 = psp_ach_preview(c, 2, "es");
    CHECK(s2.size() == 1 && s2[0].id == "especial" && s2[0].rarity == &c.rarities[2], "a tier without entries: a sample card");
    const auto s5 = psp_ach_preview(c, 5, "es");
    CHECK(s5.size() == 5 && psp_ach_toasts(s5, "i/", "es", yes).size() == 1, "step 5: one per tier = summary");
    CHECK(psp_ach_preview(c, 6, "es")[0].id == "c1", "wraps");
}

int main() {
    card_per_achievement();
    summary_over_three();
    pose_curve();
    fade();
    preview_cycle();
    if (failures == 0) std::printf("test_toast: all passed\n");
    return failures == 0 ? 0 : 1;
}
