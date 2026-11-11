#include "p3_presence.h"

#include "p3_achievements.h"

#include <cstring>
#include <initializer_list>

namespace {

constexpr uint32_t kMask = 0x07FFFFFFu;
constexpr uint32_t kUserLo = 0x08800000u, kUserHi = 0x0A000000u;
constexpr uint32_t kGlobal = 0x08AABD94u;      // pointer to the game object (also the save chain's root)
constexpr uint32_t kOverlayName = 0x08ABB1A0u; // file name of the loaded overlay
constexpr uint32_t kHolderOffset = 0x78;       // game object -> mission holder
constexpr uint32_t kMissionName = 0x9FC + 0x100;
constexpr uint32_t kHeroLevel = 0xF294;        // fact "hero.level" in p3_achievements.cpp
constexpr int kMissionMaxUnits = 64;
constexpr uint32_t kRoster = 0x08AB9560u;      // pointer whose +0x44 -> +0x28 is our player slot (FUN_089D1744)

uint32_t rd32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(ram[(a + i) & kMask]) << (8 * i);
    return v;
}

bool inUserRam(uint32_t p) { return p >= kUserLo && p < kUserHi; }

// rd32 along a pointer chain; 0 when a link leaves user RAM.
uint32_t follow(const uint8_t* ram, uint32_t p, std::initializer_list<uint32_t> offs) {
    for (uint32_t o : offs) {
        if (!inUserRam(p)) return 0;
        p = rd32(ram, p + o);
    }
    return p;
}

// Own "tweet" (multi_setTweet 0x0894C9E8 -> FUN_089D1628): holder +100 -> +0x18C table of 8 slots of 0x14 bytes,
// code at +0x30; the game stores it only while online (holder +0x44 -> +0x24 byte, FUN_08986CB4).
uint8_t ownTweet(const uint8_t* ram) {
    const uint32_t holder = follow(ram, rd32(ram, kGlobal), {kHolderOffset});
    const uint32_t online = follow(ram, holder, {0x44});
    if (!inUserRam(online) || ram[(online + 0x24) & kMask] == 0) return 0;
    const uint32_t table = follow(ram, holder, {100, 0x18C});
    const uint32_t slot = follow(ram, rd32(ram, kRoster), {0x44, 0x28});
    if (!inUserRam(table) || slot >= 8) return 0;
    const uint32_t code = rd32(ram, table + slot * 0x14 + 0x30);
    return code >= 1 && code <= 6 ? static_cast<uint8_t>(code) : 0;
}

P3Place placeOf(const uint8_t* ram) {
    char name[16] = {};
    for (int i = 0; i < 15; i++) name[i] = static_cast<char>(ram[(kOverlayName + i) & kMask]);
    if (!std::strcmp(name, "OL_Title.bin")) return P3Place::Title;
    if (!std::strcmp(name, "OL_Azito.bin")) return P3Place::Hideout;
    if (!std::strcmp(name, "OL_Mission.bin")) return P3Place::Mission;
    return P3Place::Unknown;
}

// Mission name chain; empty when a link leaves user RAM or the name is not 0-terminated within 64 units.
std::u16string missionName(const uint8_t* ram) {
    const uint32_t obj = rd32(ram, kGlobal);
    if (!inUserRam(obj)) return u"";
    const uint32_t holder = rd32(ram, obj + kHolderOffset);
    if (!inUserRam(holder)) return u"";
    std::u16string s;
    for (int i = 0; i < kMissionMaxUnits; i++) {
        const uint32_t a = holder + kMissionName + 2 * i;
        const char16_t u = static_cast<char16_t>(ram[a & kMask] | (ram[(a + 1) & kMask] << 8));
        if (!u) return s;
        // The game draws these enclosed symbols as its own icons (U+32A4 is the mission's heart): not readable text.
        if (u >= 0x3200 && u <= 0x33FF) continue;
        s += u;
    }
    return u"";  // no terminator within the cap: not a name
}

// Companion's is_pvp_mission keywords, matched on the lower-cased name (ASCII + Latin-1 letters).
bool looksVersus(const std::u16string& name) {
    std::u16string l = name;
    for (char16_t& c : l) {
        if (c >= u'A' && c <= u'Z') c += 32;
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c += 32;
    }
    for (const char16_t* k : {u"vs", u"pvp", u"enf. m\u00faltiple", u"enfrentamiento", u"["})
        if (l.find(k) != std::u16string::npos) return true;
    return false;
}

}  // namespace

bool operator==(const P3Presence& a, const P3Presence& b) {
    return a.place == b.place && a.class_id == b.class_id && a.level == b.level && a.versus == b.versus &&
           a.mission == b.mission && a.activity == b.activity;
}

P3Presence p3_presence_read(const uint8_t* rdram) {
    P3Presence p{placeOf(rdram), 0, 0, false, u""};
    const uint32_t save = p3_save_base(rdram);
    // Before the save loads the struct holds blank defaults (class 2, level 0): level 1..99 means loaded.
    const uint32_t lv = save ? rd32(rdram, save + kHeroLevel) : 0;
    if (lv >= 1 && lv <= 99 && p3_save_sane(rdram, save)) {
        p.class_id = static_cast<uint8_t>(rd32(rdram, save + 0x9520));  // sane() bounds it to 1..34
        p.level = static_cast<uint8_t>(lv);
    }
    if (p.place == P3Place::Mission || p.place == P3Place::Hideout) {
        const std::u16string name = missionName(rdram);
        p.versus = p.place == P3Place::Hideout && looksVersus(name);
        if (p.place == P3Place::Mission || p.versus) p.mission = name;
    }
    p.activity = ownTweet(rdram);
    return p;
}
