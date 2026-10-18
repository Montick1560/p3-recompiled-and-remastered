#pragma once
// On-screen keyboard skins (pure): colours, corner radius, key gap, opacity and an optional font, from
// a JSON file (settings.ini [display] osk_skin -> PSPRECOMP_OSK_SKIN). Missing or bad fields keep the
// built-in Deck skin's values, each problem adds a log line. Sizes are pixels at 1080p.
#include <string>
#include <vector>

struct OskColor { float r, g, b, a; };

struct OskSkin {
    std::string name = "Deck";
    OskColor background{0.0f, 0.0f, 0.0f, 0.45f};     // over the game image
    OskColor panel{0.106f, 0.157f, 0.220f, 0.96f};    // keyboard panel
    OskColor key{0.165f, 0.247f, 0.353f, 1.0f};       // key caps
    OskColor key_text{0.922f, 0.933f, 0.949f, 1.0f};
    OskColor focus{0.102f, 0.624f, 1.0f, 1.0f};       // focused key
    OskColor focus_text{1.0f, 1.0f, 1.0f, 1.0f};
    OskColor accent{0.102f, 0.624f, 1.0f, 1.0f};      // title, active shift / caps / layer
    float radius = 10.0f, gap = 8.0f, opacity = 1.0f;
    std::string font;                                  // absolute path, or empty for the system font
};

bool osk_parse_color(const std::string& s, OskColor* out);  // "#RRGGBB" or "#RRGGBBAA"
OskSkin osk_skin_parse(const std::string& json, const std::string& skin_dir, std::vector<std::string>* log);
