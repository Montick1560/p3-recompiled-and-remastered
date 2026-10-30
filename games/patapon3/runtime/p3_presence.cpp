#include "p3_presence.h"

#include "p3_achievements.h"

#include <cstring>

namespace {

constexpr uint32_t kMask = 0x07FFFFFFu;
constexpr uint32_t kUserLo = 0x08800000u, kUserHi = 0x0A000000u;
constexpr uint32_t kGlobal = 0x08AABD94u;      // pointer to the game object (also the save chain's root)
constexpr uint32_t kOverlayName = 0x08ABB1A0u; // file name of the loaded overlay
constexpr uint32_t kHolderOffset = 0x78;       // game object -> mission holder
constexpr uint32_t kMissionName = 0x9FC + 0x100;
constexpr uint32_t kHeroLevel = 0xF294;        // fact "hero.level" in p3_achievements.cpp
constexpr int kMissionMaxUnits = 64;

uint32_t rd32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(ram[(a + i) & kMask]) << (8 * i);
    return v;
}

bool inUserRam(uint32_t p) { return p >= kUserLo && p < kUserHi; }

P3Place placeOf(const uint8_t* ram) {
    char name[16] = {};
    for (int i = 0; i < 15; i++) name[i] = static_cast<char>(ram[(kOverlayName + i) & kMask]);
    if (!std::strcmp(name, "OL_Title.bin")) return P3Place::Title;
    if (!std::strcmp(name, "OL_Azito.bin")) return P3Place::Hideout;
    if (!std::strcmp(name, "OL_Mission.bin")) return P3Place::Mission;
    return P3Place::Unknown;
}

// Mission name chain; empty when a link leaves user RAM. Stops at the first 0 or 64 units.
std::u16string missionName(const uint8_t* ram) {
    const uint32_t obj = rd32(ram, kGlobal);
    if (!inUserRam(obj)) return u"";
    const uint32_t holder = rd32(ram, obj + kHolderOffset);
    if (!inUserRam(holder)) return u"";
    std::u16string s;
    for (int i = 0; i < kMissionMaxUnits; i++) {
        const uint32_t a = holder + kMissionName + 2 * i;
        const char16_t u = static_cast<char16_t>(ram[a & kMask] | (ram[(a + 1) & kMask] << 8));
        if (!u) break;
        s += u;
    }
    return s;
}

// Companion's is_pvp_mission keywords, matched on the lower-cased name (ASCII + Latin-1 letters).
bool looksVersus(const std::u16string& name) {
    std::u16string l = name;
    for (char16_t& c : l) {
        if (c >= u'A' && c <= u'Z') c += 32;
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c += 32;
    }
    for (const char16_t* k : {u"vs", u"contra", u"enfrentamiento", u"duelo", u"batalla", u"pvp", u"enf. m\u00faltiple", u"["})
        if (l.find(k) != std::u16string::npos) return true;
    return false;
}

}  // namespace

bool operator==(const P3Presence& a, const P3Presence& b) {
    return a.place == b.place && a.class_id == b.class_id && a.level == b.level && a.versus == b.versus &&
           a.mission == b.mission;
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
    return p;
}
