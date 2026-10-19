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
