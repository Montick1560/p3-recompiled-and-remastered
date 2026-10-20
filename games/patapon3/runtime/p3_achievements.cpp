#include "p3_achievements.h"

namespace {

constexpr uint32_t kUserLo = 0x08800000u, kUserHi = 0x0A000000u;
constexpr uint32_t kStructSize = 178848;      // the decrypted SECURE.BIN size == the struct
constexpr uint32_t kHeroName = 0xF1C8;        // UTF-16, up to 16 units
constexpr uint32_t kHeroClass = 0x9520;       // 1..34 (PataponRichPressence class list)

uint32_t rd32(const uint8_t* rdram, uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(rdram[(a + i) & 0x07FFFFFFu]) << (8 * i);
    return v;
}

// base = rd32(... rd32(rd32(Global) + O1) ... + On); 0 when a link leaves user RAM.
template <uint32_t Global, uint32_t... Offs>
uint32_t follow(const uint8_t* rdram) {
    uint32_t p = rd32(rdram, Global);
    for (uint32_t o : {Offs...}) {
        if (p < kUserLo || p >= kUserHi) return 0;
        p = rd32(rdram, p + o);
    }
    return p;
}

// Hero name printable UTF-16 (1..16 units, then 0) and class 1..34.
bool sane(const uint8_t* rdram, uint32_t base) {
    const uint32_t cls = rd32(rdram, base + kHeroClass);
    if (cls < 1 || cls > 34) return false;
    int len = 0;
    for (; len < 16; len++) {
        const uint32_t a = base + kHeroName + 2 * len;
        const uint16_t u = static_cast<uint16_t>(rdram[a & 0x07FFFFFFu] | (rdram[(a + 1) & 0x07FFFFFFu] << 8));
        if (u == 0) break;
        if (u < 0x20) return false;
    }
    return len >= 1 && len < 16;
}

void hero(const uint8_t* rdram, uint32_t base, char16_t out[17]) {
    for (int i = 0; i < 16; i++) {
        const uint32_t a = base + kHeroName + 2 * i;
        out[i] = static_cast<char16_t>(rdram[a & 0x07FFFFFFu] | (rdram[(a + 1) & 0x07FFFFFFu] << 8));
        if (!out[i]) break;
    }
    out[16] = 0;
}

// Facts (docs/p3_achievement_facts.md): {name, offset, size, signed, bit, count, min, max}.
const PspFact kFacts[] = {
    {"missions", 0x10, 4, false, -1, 0, 0, 1000000},
    {"hero.level", 0xF294, 4, false, -1, 0, 1, 99},
    {"playtime.seconds", 0x14, 4, false, -1, 0, 0, 2147483647},
    {"kaching", 0x20, 4, false, -1, 0, 0, 999999999},
    {"archfiend.valor", 0xEDEA, 1, false, -1, 0, 0, 255},
    {"archfiend.purity", 0xEE12, 1, false, -1, 0, 0, 255},
    {"archfiend.justice", 0xEE3A, 1, false, -1, 0, 0, 255},
    {"archfiend.impetuosity", 0xEE62, 1, false, -1, 0, 0, 255},
    {"archfiend.restraint", 0xEE8A, 1, false, -1, 0, 0, 255},
    {"archfiend.inflexibility", 0xEEB2, 1, false, -1, 0, 0, 255},
    {"archfiend.tolerance", 0xEEDA, 1, false, -1, 0, 0, 255},
    {"team.coop_missions", 0xF4B4, 4, false, -1, 0, 0, 1000000},
    {"team.versus", 0xF4B8, 4, false, -1, 0, 0, 1000000},
    {"team.recruited", 0xF4C0, 4, false, -1, 0, 0, 1000000},
    {"team.points", 0xF4CC, 4, false, -1, 0, 0, 100000000},
    {"team.heroes_defeated", 0xF4D8, 4, false, -1, 0, 0, 100000000},
    {"team.multiplayer", 0xF4B0, 4, false, -1, 0, 0, 1000000},
    {"team.cards", 0xF4C4, 4, false, -1, 0, 0, 1000000},
    {"team.perfects", 0xF4D0, 4, false, -1, 0, 0, 1000000000},
    {"class.yarida.level", 0x9644, 4, false, -1, 0, 0, 99},
    {"class.taterazay.level", 0x96C8, 4, false, -1, 0, 0, 99},
    {"class.yumiyacha.level", 0x974C, 4, false, -1, 0, 0, 99},
    {"class.kibadda.level", 0x97D0, 4, false, -1, 0, 0, 99},
    {"class.destrobo.level", 0x99E0, 4, false, -1, 0, 0, 99},
    {"class.piekron.level", 0x9B6C, 4, false, -1, 0, 0, 99},
    {"class.wooyari.level", 0x9BF0, 4, false, -1, 0, 0, 99},
    {"class.pyokorider.level", 0x9C74, 4, false, -1, 0, 0, 99},
    {"class.cannassault.level", 0x9CF8, 4, false, -1, 0, 0, 99},
    {"class.charibasa.level", 0x9D7C, 4, false, -1, 0, 0, 99},
    {"class.guardira.level", 0x9E00, 4, false, -1, 0, 0, 99},
    {"class.tondenga.level", 0x9E84, 4, false, -1, 0, 0, 99},
    {"class.myamsar.level", 0x9F08, 4, false, -1, 0, 0, 99},
    {"class.bowmunk.level", 0x9F8C, 4, false, -1, 0, 0, 99},
    {"class.grenburr.level", 0xA010, 4, false, -1, 0, 0, 99},
    {"class.alosson.level", 0xA094, 4, false, -1, 0, 0, 99},
    {"class.wondabarappa.level", 0xA118, 4, false, -1, 0, 0, 99},
    {"class.jamsch.level", 0xA19C, 4, false, -1, 0, 0, 99},
    {"class.oohoroc.level", 0xA220, 4, false, -1, 0, 0, 99},
    {"class.pingrek.level", 0xA2A4, 4, false, -1, 0, 0, 99},
    {"class.cannogabang.level", 0xA328, 4, false, -1, 0, 0, 99},
    {"ton.yarida.level", 0xAC64, 4, false, -1, 0, 0, 99},
    {"ton.kibadda.level", 0xADF0, 4, false, -1, 0, 0, 99},
    {"ton.piekron.level", 0xB18C, 4, false, -1, 0, 0, 99},
    {"ton.wooyari.level", 0xB210, 4, false, -1, 0, 0, 99},
    {"ton.pyokorider.level", 0xB294, 4, false, -1, 0, 0, 99},
    {"ton.cannassault.level", 0xB318, 4, false, -1, 0, 0, 99},
    {"ton.charibasa.level", 0xB39C, 4, false, -1, 0, 0, 99},
    {"chin.taterazay.level", 0xC308, 4, false, -1, 0, 0, 99},
    {"chin.destrobo.level", 0xC620, 4, false, -1, 0, 0, 99},
    {"chin.guardira.level", 0xCA40, 4, false, -1, 0, 0, 99},
    {"chin.tondenga.level", 0xCAC4, 4, false, -1, 0, 0, 99},
    {"chin.myamsar.level", 0xCB48, 4, false, -1, 0, 0, 99},
    {"chin.bowmunk.level", 0xCBCC, 4, false, -1, 0, 0, 99},
    {"chin.grenburr.level", 0xCC50, 4, false, -1, 0, 0, 99},
    {"kan.yumiyacha.level", 0xD9AC, 4, false, -1, 0, 0, 99},
    {"kan.alosson.level", 0xE2F4, 4, false, -1, 0, 0, 99},
    {"kan.wondabarappa.level", 0xE378, 4, false, -1, 0, 0, 99},
    {"kan.jamsch.level", 0xE3FC, 4, false, -1, 0, 0, 99},
    {"kan.oohoroc.level", 0xE480, 4, false, -1, 0, 0, 99},
    {"kan.pingrek.level", 0xE504, 4, false, -1, 0, 0, 99},
    {"kan.cannogabang.level", 0xE588, 4, false, -1, 0, 0, 99},
    {"mission.22", 0xEDE0, 1, false, -1, 0, 0, 255},
    {"mission.23", 0xEDE1, 1, false, -1, 0, 0, 255},
    {"mission.42", 0xEDF4, 1, false, -1, 0, 0, 255},
    {"mission.53", 0xEDFF, 1, false, -1, 0, 0, 255},
    {"mission.54", 0xEE00, 1, false, -1, 0, 0, 255},
    {"mission.56", 0xEE02, 1, false, -1, 0, 0, 255},
    {"mission.82", 0xEE1C, 1, false, -1, 0, 0, 255},
    {"mission.93", 0xEE27, 1, false, -1, 0, 0, 255},
    {"mission.96", 0xEE2A, 1, false, -1, 0, 0, 255},
    {"mission.122", 0xEE44, 1, false, -1, 0, 0, 255},
    {"mission.133", 0xEE4F, 1, false, -1, 0, 0, 255},
    {"mission.136", 0xEE52, 1, false, -1, 0, 0, 255},
    {"mission.162", 0xEE6C, 1, false, -1, 0, 0, 255},
    {"mission.173", 0xEE77, 1, false, -1, 0, 0, 255},
    {"mission.176", 0xEE7A, 1, false, -1, 0, 0, 255},
    {"mission.202", 0xEE94, 1, false, -1, 0, 0, 255},
    {"mission.216", 0xEEA2, 1, false, -1, 0, 0, 255},
    {"mission.222", 0xEEA8, 1, false, -1, 0, 0, 255},
    {"mission.242", 0xEEBC, 1, false, -1, 0, 0, 255},
    {"mission.282", 0xEEE4, 1, false, -1, 0, 0, 255},
    {"mission.28", 0xEDE6, 1, false, -1, 0, 0, 255},
    {"mission.33", 0xEDEB, 1, false, -1, 0, 0, 255},
    {"mission.55", 0xEE01, 1, false, -1, 0, 0, 255},
    {"mission.57", 0xEE03, 1, false, -1, 0, 0, 255},
    {"mission.62", 0xEE08, 1, false, -1, 0, 0, 255},
    {"mission.64", 0xEE0A, 1, false, -1, 0, 0, 255},
    {"mission.73", 0xEE13, 1, false, -1, 0, 0, 255},
    {"mission.95", 0xEE29, 1, false, -1, 0, 0, 255},
    {"mission.113", 0xEE3B, 1, false, -1, 0, 0, 255},
    {"mission.135", 0xEE51, 1, false, -1, 0, 0, 255},
    {"mission.153", 0xEE63, 1, false, -1, 0, 0, 255},
    {"mission.175", 0xEE79, 1, false, -1, 0, 0, 255},
    {"mission.182", 0xEE80, 1, false, -1, 0, 0, 255},
    {"mission.183", 0xEE81, 1, false, -1, 0, 0, 255},
    {"mission.193", 0xEE8B, 1, false, -1, 0, 0, 255},
    {"mission.212", 0xEE9E, 1, false, -1, 0, 0, 255},
    {"mission.213", 0xEE9F, 1, false, -1, 0, 0, 255},
    {"mission.215", 0xEEA1, 1, false, -1, 0, 0, 255},
    {"mission.233", 0xEEB3, 1, false, -1, 0, 0, 255},
    {"mission.63", 0xEE09, 1, false, -1, 0, 0, 255},
    {"mission.103", 0xEE31, 1, false, -1, 0, 0, 255},
    {"mission.104", 0xEE32, 1, false, -1, 0, 0, 255},
    {"mission.143", 0xEE59, 1, false, -1, 0, 0, 255},
    {"mission.144", 0xEE5A, 1, false, -1, 0, 0, 255},
    {"mission.184", 0xEE82, 1, false, -1, 0, 0, 255},
    {"mission.223", 0xEEA9, 1, false, -1, 0, 0, 255},
    {"mission.224", 0xEEAA, 1, false, -1, 0, 0, 255},
    {"classes.unlocked", 0x9530, 4, false, -1, 35, 0, 35, 0x84},
    {"class.yarida.unlocked", 0x9638, 4, false, -1, 0, 0, 255},
    {"class.taterazay.unlocked", 0x96BC, 4, false, -1, 0, 0, 255},
    {"class.yumiyacha.unlocked", 0x9740, 4, false, -1, 0, 0, 255},
    {"class.kibadda.unlocked", 0x97C4, 4, false, -1, 0, 0, 255},
    {"class.destrobo.unlocked", 0x99D4, 4, false, -1, 0, 0, 255},
    {"class.piekron.unlocked", 0x9B60, 4, false, -1, 0, 0, 255},
    {"class.wooyari.unlocked", 0x9BE4, 4, false, -1, 0, 0, 255},
    {"class.pyokorider.unlocked", 0x9C68, 4, false, -1, 0, 0, 255},
    {"class.cannassault.unlocked", 0x9CEC, 4, false, -1, 0, 0, 255},
    {"class.charibasa.unlocked", 0x9D70, 4, false, -1, 0, 0, 255},
    {"class.guardira.unlocked", 0x9DF4, 4, false, -1, 0, 0, 255},
    {"class.tondenga.unlocked", 0x9E78, 4, false, -1, 0, 0, 255},
    {"class.myamsar.unlocked", 0x9EFC, 4, false, -1, 0, 0, 255},
    {"class.bowmunk.unlocked", 0x9F80, 4, false, -1, 0, 0, 255},
    {"class.grenburr.unlocked", 0xA004, 4, false, -1, 0, 0, 255},
    {"class.alosson.unlocked", 0xA088, 4, false, -1, 0, 0, 255},
    {"class.wondabarappa.unlocked", 0xA10C, 4, false, -1, 0, 0, 255},
    {"class.jamsch.unlocked", 0xA190, 4, false, -1, 0, 0, 255},
    {"class.oohoroc.unlocked", 0xA214, 4, false, -1, 0, 0, 255},
    {"class.pingrek.unlocked", 0xA298, 4, false, -1, 0, 0, 255},
    {"class.cannogabang.unlocked", 0xA31C, 4, false, -1, 0, 0, 255},
    {"class.ravenous.unlocked", 0xA3A0, 4, false, -1, 0, 0, 255},
    {"class.sonarchy.unlocked", 0xA424, 4, false, -1, 0, 0, 255},
    {"class.ragewolf.unlocked", 0xA4A8, 4, false, -1, 0, 0, 255},
    {"class.naughtyfins.unlocked", 0xA52C, 4, false, -1, 0, 0, 255},
    {"class.slogturtle.unlocked", 0xA5B0, 4, false, -1, 0, 0, 255},
    {"class.covet_hiss.unlocked", 0xA634, 4, false, -1, 0, 0, 255},
    {"class.buzzcrave.unlocked", 0xA6B8, 4, false, -1, 0, 0, 255},
};

const PspAchievementSource kSources[] = {
    {"dxd-1.1.4", follow<0x08AABD94u, 0x50u>, sane, hero, kStructSize, kFacts, sizeof kFacts / sizeof kFacts[0]},
    {"dxd-1.1.5b", follow<0x08AABD94u, 0x50u>, sane, hero, kStructSize, kFacts, sizeof kFacts / sizeof kFacts[0]},
    {"vanilla-eu", follow<0x08AABD94u, 0x50u>, sane, hero, kStructSize, kFacts, sizeof kFacts / sizeof kFacts[0]},
};

}  // namespace

const PspAchievementSource* p3_achievement_sources(size_t* n) {
    *n = sizeof kSources / sizeof kSources[0];
    return kSources;
}
