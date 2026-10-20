#include "psp_prompts.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {

// PSP sceCtrl masks (psp_controls.cpp control_actions()).
constexpr uint32_t kUp = 0x0010, kDown = 0x0040, kLeft = 0x0080, kRight = 0x0020;
constexpr uint32_t kCross = 0x4000, kCircle = 0x2000, kSquare = 0x8000, kTriangle = 0x1000;
constexpr uint32_t kL = 0x0100, kR = 0x0200, kStart = 0x0008, kSelect = 0x0001;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool playstation(InputDevice d) { return d == InputDevice::DualShock4 || d == InputDevice::DualSense; }

// The icon of a physical pad button (SDL GameController name) in the device's style.
std::string pad_icon(const std::string& button, InputDevice d, PromptStyle s) {
    const bool ps = playstation(d);
    const bool color = s == PromptStyle::Color;
    static const std::map<std::string, std::pair<const char*, const char*>> face = {
        {"a", {"a", "cross"}}, {"b", {"b", "circle"}}, {"x", {"x", "square"}}, {"y", {"y", "triangle"}}};
    if (auto it = face.find(button); it != face.end()) {
        if (ps) return std::string(color ? "playstation_button_color_" : "playstation_button_") + it->second.second;
        return std::string(color ? "xbox_button_color_" : "xbox_button_") + it->second.first;
    }
    if (button == "start") {
        if (!ps) return "xbox_button_menu";
        return d == InputDevice::DualSense ? "playstation5_button_options" : "playstation4_button_options";
    }
    if (button == "back") {
        if (!ps) return "xbox_button_view";
        return d == InputDevice::DualSense ? "playstation5_button_create" : "playstation4_button_share";
    }
    static const std::map<std::string, std::pair<const char*, const char*>> other = {
        {"leftshoulder", {"xbox_lb", "playstation_trigger_l1"}},
        {"rightshoulder", {"xbox_rb", "playstation_trigger_r1"}},
        {"leftstick", {"xbox_ls", "playstation_button_l3"}},
        {"rightstick", {"xbox_rs", "playstation_button_r3"}},
        {"dpup", {"xbox_dpad_up", "playstation_dpad_up"}},
        {"dpdown", {"xbox_dpad_down", "playstation_dpad_down"}},
        {"dpleft", {"xbox_dpad_left", "playstation_dpad_left"}},
        {"dpright", {"xbox_dpad_right", "playstation_dpad_right"}}};
    if (auto it = other.find(button); it != other.end()) return ps ? it->second.second : it->second.first;
    return "";
}

std::string button_icon(uint32_t mask, InputDevice d, PromptStyle s, const ControlMap& map, bool wide) {
    if (d == InputDevice::Keyboard) {
        const std::string key = map.key_name(mask);
        return key.empty() ? "" : prompt_key_icon(key, wide);
    }
    const std::string pad = map.pad_name(mask);
    return pad.empty() ? "" : pad_icon(pad, d, s);
}

// The D-pad glyphs only have a faithful icon while the four directions sit where the icon shows them:
// the arrow keys on the keyboard, the pad's own D-pad on a controller.
bool dpad_in_place(InputDevice d, const ControlMap& map) {
    const uint32_t masks[4] = {kUp, kDown, kLeft, kRight};
    const char* keys[4] = {"up", "down", "left", "right"};
    const char* pads[4] = {"dpup", "dpdown", "dpleft", "dpright"};
    for (int i = 0; i < 4; i++) {
        const std::string got = d == InputDevice::Keyboard ? lower(map.key_name(masks[i])) : map.pad_name(masks[i]);
        if (got != (d == InputDevice::Keyboard ? keys[i] : pads[i])) return false;
    }
    return true;
}

}  // namespace

PromptStyle prompt_style_from_env(const char* value) {
    return value != nullptr && std::strcmp(value, "color") == 0 ? PromptStyle::Color : PromptStyle::Gray;
}

std::vector<std::string> prompt_icons(PromptGlyph g, InputDevice d, PromptStyle s, const ControlMap& map) {
    auto one = [](std::string n) { return n.empty() ? std::vector<std::string>{} : std::vector<std::string>{n}; };
    const bool kb = d == InputDevice::Keyboard;
    const char* family = playstation(d) ? "playstation" : "xbox";
    switch (g) {
    case PromptGlyph::Cross: return one(button_icon(kCross, d, s, map, false));
    case PromptGlyph::Circle: return one(button_icon(kCircle, d, s, map, false));
    case PromptGlyph::Square: return one(button_icon(kSquare, d, s, map, false));
    case PromptGlyph::Triangle: return one(button_icon(kTriangle, d, s, map, false));
    case PromptGlyph::Start: return one(button_icon(kStart, d, s, map, true));
    case PromptGlyph::Select: return one(button_icon(kSelect, d, s, map, true));
    case PromptGlyph::LR: {
        const std::string l = button_icon(kL, d, s, map, false), r = button_icon(kR, d, s, map, false);
        if (l.empty() || r.empty()) return {};
        return {l, r};
    }
    case PromptGlyph::DpadAll:
    case PromptGlyph::DpadVertical:
    case PromptGlyph::DpadUp: {
        if (!dpad_in_place(d, map)) return {};
        const char* which = g == PromptGlyph::DpadAll ? "all" : g == PromptGlyph::DpadVertical ? "vertical" : "up";
        if (kb) return {std::string("@arrows_") + which};
        return {std::string(family) + "_dpad_" + which};
    }
    case PromptGlyph::StickVertical:
        if (kb) return {};
        return {std::string(family) + "_stick_l_vertical"};
    }
    return {};
}

std::string prompt_key_icon(const std::string& sdl_key_name, bool wide) {
    std::string n = lower(sdl_key_name);
    for (const char* prefix : {"left ", "right "}) {
        if (n.rfind(prefix, 0) == 0) n = n.substr(std::strlen(prefix));
    }
    if (n.empty()) return "";
    if (n.size() == 1 && std::isalnum(static_cast<unsigned char>(n[0]))) return "keyboard_" + n;
    if (n[0] == 'f' && n.size() <= 3 && std::all_of(n.begin() + 1, n.end(), ::isdigit)) {
        const int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return "keyboard_" + n;
    }
    // Keys whose wide (pill) and narrow (glyph cell) icons differ: a word key vs a symbol.
    static const std::map<std::string, std::pair<const char*, const char*>> two = {
        {"return", {"keyboard_enter", "keyboard_return"}},
        {"space", {"keyboard_space", "keyboard_space_icon"}},
        {"tab", {"keyboard_tab", "keyboard_tab_icon"}},
        {"backspace", {"keyboard_backspace", "keyboard_backspace_icon"}},
        {"shift", {"keyboard_shift", "keyboard_shift_icon"}},
        {"capslock", {"keyboard_capslock", "keyboard_capslock_icon"}}};
    if (auto it = two.find(n); it != two.end()) return wide ? it->second.first : it->second.second;
    static const std::map<std::string, const char*> one = {
        {"up", "keyboard_arrow_up"}, {"down", "keyboard_arrow_down"}, {"left", "keyboard_arrow_left"},
        {"right", "keyboard_arrow_right"}, {"escape", "keyboard_escape"}, {"ctrl", "keyboard_ctrl"},
        {"alt", "keyboard_alt"}, {"gui", "keyboard_win"}, {"delete", "keyboard_delete"},
        {"insert", "keyboard_insert"}, {"home", "keyboard_home"}, {"end", "keyboard_end"},
        {"pageup", "keyboard_page_up"}, {"pagedown", "keyboard_page_down"},
        {"keypad enter", "keyboard_numpad_enter"}, {"keypad +", "keyboard_numpad_plus"},
        {",", "keyboard_comma"}, {".", "keyboard_period"}, {"-", "keyboard_minus"}, {"=", "keyboard_equals"},
        {"/", "keyboard_slash_forward"}, {"\\", "keyboard_slash_back"}, {";", "keyboard_semicolon"},
        {"'", "keyboard_apostrophe"}, {"[", "keyboard_bracket_open"}, {"]", "keyboard_bracket_close"},
        {"`", "keyboard_tilde"}};
    if (auto it = one.find(n); it != one.end()) return it->second;
    return "";
}
