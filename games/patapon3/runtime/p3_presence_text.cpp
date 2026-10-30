#include "p3_presence_text.h"

#include <cstdio>
#include <cstring>

namespace {

// Class names 1..34 (the order of the class byte in the save), index 0 unused.
const char* const kClassNames[35] = {
    "", "Hatapon", "Yarida", "Taterazay", "Yumiyacha", "Kibadda", "Dekapon", "Megapon", "Mahopon", "Destrobo",
    "Charipon", "Chakapon", "Piekron", "Wooyari", "Pyokorider", "Cannassault", "Charibasa", "Guardira", "Tondenga",
    "Myamsar", "Bowmunk", "Grenburr", "Alosson", "Wondabarappa", "Jamsch", "Oohoroc", "Pingrek", "Cannogabang",
    "Ravenous", "Sonarchy", "Ragewolf", "Naughtyfins", "Slogturtle", "Covet Hiss", "Buzzcrave"};

std::string utf8(const std::u16string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) { o += static_cast<char>(0xC0 | c >> 6); o += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += static_cast<char>(0xE0 | c >> 12); o += static_cast<char>(0x80 | (c >> 6 & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F)); }
        else { o += static_cast<char>(0xF0 | c >> 18); o += static_cast<char>(0x80 | (c >> 12 & 0x3F)); o += static_cast<char>(0x80 | (c >> 6 & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return o;
}

}  // namespace

nlohmann::json p3_presence_activity(const P3Presence& p, const char* lang, int64_t start_unix) {
    if (p.place == P3Place::Unknown) return nullptr;
    const bool en = lang && !std::strcmp(lang, "en");
    nlohmann::json a;
    a["timestamps"] = {{"start", start_unix}};
    a["assets"] = {{"large_image", "logo"}, {"large_text", "Patapon 3"}};
    if (p.versus) {
        a["details"] = en ? "In a versus match" : "En enfrentamiento";
        a["assets"]["small_image"] = "vs";
        return a;
    }
    if (p.place == P3Place::Title) {
        a["details"] = en ? "On the title screen" : "En la pantalla de t\xC3\xADtulo";
        return a;
    }
    const bool known = p.class_id >= 1 && p.class_id <= 34 && p.level >= 1;
    if (known) {
        char b[64], img[16];
        std::snprintf(b, sizeof b, en ? " \xC2\xB7 Level %d" : " \xC2\xB7 Nivel %d", p.level);
        std::snprintf(img, sizeof img, "class_%02d", p.class_id);
        a["details"] = std::string(kClassNames[p.class_id]) + b;
        a["assets"]["small_image"] = img;
        a["assets"]["small_text"] = kClassNames[p.class_id];
    } else {
        a["details"] = "Patapon 3";
    }
    if (p.place == P3Place::Hideout) a["state"] = en ? "In the hideout" : "En el escondite";
    else if (p.mission.empty()) a["state"] = en ? "On a mission" : "En misi\xC3\xB3n";
    else a["state"] = std::string(en ? "Mission: " : "Misi\xC3\xB3n: ") + utf8(p.mission);
    return a;
}

nlohmann::json p3_presence_activity_from_ram(const uint8_t* rdram, const char* lang, int64_t start_unix) {
    return p3_presence_activity(p3_presence_read(rdram), lang, start_unix);
}
