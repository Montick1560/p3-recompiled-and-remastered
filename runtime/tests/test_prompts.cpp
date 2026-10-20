// Unit tests for psp_prompts: which icon each help-bar glyph shows per device, style and control map
// (docs/superpowers/specs/2026-10-19-pc-button-prompts-part1-design.md §4.5). Empty = the game's glyph.
#include "psp_prompts.h"

#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// Stand-ins for SDL's name tables, as in test_controls.cpp.
static const std::map<std::string, int32_t> kKeys = {
    {"up", 1}, {"down", 2}, {"left", 3}, {"right", 4}, {"x", 5}, {"return", 6}, {"z", 7}, {"a", 8},
    {"s", 9}, {"q", 10}, {"w", 11}, {"space", 12}, {"v", 13}, {"tab", 14}, {"k", 15}};
static const std::map<std::string, int> kPads = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"guide", 5}, {"start", 6},
    {"leftstick", 7}, {"rightstick", 8}, {"leftshoulder", 9}, {"rightshoulder", 10},
    {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14}};
static std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
static ControlMap map_of(const char* spec) {
    std::vector<std::string> log;
    return ControlMap::build(
        spec, [](const std::string& n) { auto it = kKeys.find(lower(n)); return it == kKeys.end() ? 0 : it->second; },
        [](const std::string& n) { auto it = kPads.find(lower(n)); return it == kPads.end() ? -1 : it->second; }, &log);
}
using V = std::vector<std::string>;
static V icons(PromptGlyph g, InputDevice d, const ControlMap& m, PromptStyle s = PromptStyle::Gray) {
    return prompt_icons(g, d, s, m);
}

static void keyboard_defaults() {
    const ControlMap m = map_of("");
    const InputDevice k = InputDevice::Keyboard;
    CHECK(icons(PromptGlyph::Cross, k, m) == V{"keyboard_x"}, "cross shows the first key, X");
    CHECK(icons(PromptGlyph::Circle, k, m) == V{"keyboard_z"}, "circle Z");
    CHECK(icons(PromptGlyph::Square, k, m) == V{"keyboard_a"}, "square A");
    CHECK(icons(PromptGlyph::Triangle, k, m) == V{"keyboard_s"}, "triangle S");
    CHECK(icons(PromptGlyph::LR, k, m) == (V{"keyboard_q", "keyboard_w"}), "L/R = Q/W");
    CHECK(icons(PromptGlyph::Start, k, m) == V{"keyboard_space"}, "start pill: wide Space key");
    CHECK(icons(PromptGlyph::Select, k, m) == V{"keyboard_v"}, "select V");
    CHECK(icons(PromptGlyph::DpadAll, k, m) == V{"@arrows_all"}, "d-pad all: arrow keycap");
    CHECK(icons(PromptGlyph::DpadVertical, k, m) == V{"@arrows_vertical"}, "d-pad vertical");
    CHECK(icons(PromptGlyph::DpadUp, k, m) == V{"@arrows_up"}, "d-pad up");
    CHECK(icons(PromptGlyph::StickVertical, k, m).empty(), "the analog stick has no key");
    CHECK(icons(PromptGlyph::Cross, k, m, PromptStyle::Color) == V{"keyboard_x"}, "keys have no colour variant");
}

static void pad_defaults() {
    const ControlMap m = map_of("");
    CHECK(icons(PromptGlyph::Cross, InputDevice::Xbox, m) == V{"xbox_button_a"}, "xbox cross = A");
    CHECK(icons(PromptGlyph::Cross, InputDevice::Xbox, m, PromptStyle::Color) == V{"xbox_button_color_a"}, "xbox colour A");
    CHECK(icons(PromptGlyph::Start, InputDevice::Xbox, m) == V{"xbox_button_menu"}, "xbox menu");
    CHECK(icons(PromptGlyph::Select, InputDevice::Xbox, m) == V{"xbox_button_view"}, "xbox view");
    CHECK(icons(PromptGlyph::LR, InputDevice::Xbox, m) == (V{"xbox_lb", "xbox_rb"}), "LB/RB");
    CHECK(icons(PromptGlyph::DpadVertical, InputDevice::Xbox, m) == V{"xbox_dpad_vertical"}, "xbox d-pad");
    CHECK(icons(PromptGlyph::StickVertical, InputDevice::Xbox, m) == V{"xbox_stick_l_vertical"}, "xbox stick");
    CHECK(icons(PromptGlyph::Triangle, InputDevice::DualShock4, m) == V{"playstation_button_triangle"}, "ds4 triangle");
    CHECK(icons(PromptGlyph::Triangle, InputDevice::DualShock4, m, PromptStyle::Color) ==
              V{"playstation_button_color_triangle"}, "ds4 colour triangle");
    CHECK(icons(PromptGlyph::LR, InputDevice::DualSense, m) == (V{"playstation_trigger_l1", "playstation_trigger_r1"}), "L1/R1");
    CHECK(icons(PromptGlyph::Start, InputDevice::DualShock4, m) == V{"playstation4_button_options"}, "ds4 options");
    CHECK(icons(PromptGlyph::Select, InputDevice::DualShock4, m) == V{"playstation4_button_share"}, "ds4 share");
    CHECK(icons(PromptGlyph::Start, InputDevice::DualSense, m) == V{"playstation5_button_options"}, "ds options");
    CHECK(icons(PromptGlyph::Select, InputDevice::DualSense, m) == V{"playstation5_button_create"}, "ds create");
    CHECK(icons(PromptGlyph::DpadAll, InputDevice::DualSense, m) == V{"playstation_dpad_all"}, "ps d-pad");
    CHECK(icons(PromptGlyph::StickVertical, InputDevice::DualSense, m) == V{"playstation_stick_l_vertical"}, "ps stick");
}

static void remaps() {
    const ControlMap m = map_of("cross=Return|b");
    CHECK(icons(PromptGlyph::Cross, InputDevice::Keyboard, m) == V{"keyboard_return"}, "remapped key, narrow symbol");
    CHECK(icons(PromptGlyph::Cross, InputDevice::Xbox, m) == V{"xbox_button_b"}, "the physical Xbox button");
    CHECK(icons(PromptGlyph::Cross, InputDevice::DualSense, m) == V{"playstation_button_circle"}, "same button, PS style");
    const ControlMap u = map_of("circle=|");
    CHECK(icons(PromptGlyph::Circle, InputDevice::Keyboard, u).empty(), "no key -> game glyph");
    CHECK(icons(PromptGlyph::Circle, InputDevice::Xbox, u).empty(), "no pad button -> game glyph");
    const ControlMap l = map_of("l=|");
    CHECK(icons(PromptGlyph::LR, InputDevice::Keyboard, l).empty(), "L/R needs both");
    const ControlMap w = map_of("up=W|dpup");
    CHECK(icons(PromptGlyph::DpadAll, InputDevice::Keyboard, w).empty(), "non-arrow d-pad keys keep the glyph");
    CHECK(icons(PromptGlyph::DpadAll, InputDevice::Xbox, w) == V{"xbox_dpad_all"}, "pad d-pad unchanged");
    const ControlMap p = map_of("up=Up|y");
    CHECK(icons(PromptGlyph::DpadUp, InputDevice::Xbox, p).empty(), "d-pad moved off the pad's d-pad keeps the glyph");
    const ControlMap s = map_of("start=Tab|leftshoulder;l=Q|start");
    CHECK(icons(PromptGlyph::Start, InputDevice::Keyboard, s) == V{"keyboard_tab"}, "wide Tab");
    CHECK(icons(PromptGlyph::Start, InputDevice::DualSense, s) == V{"playstation_trigger_l1"}, "start on L1");
}

static void key_icons() {
    CHECK(prompt_key_icon("Return", false) == "keyboard_return", "return narrow");
    CHECK(prompt_key_icon("Return", true) == "keyboard_enter", "return wide");
    CHECK(prompt_key_icon("Space", true) == "keyboard_space", "space wide");
    CHECK(prompt_key_icon("Space", false) == "keyboard_space_icon", "space narrow");
    CHECK(prompt_key_icon("Tab", false) == "keyboard_tab_icon", "tab narrow");
    CHECK(prompt_key_icon("Left Shift", false) == "keyboard_shift_icon", "left shift");
    CHECK(prompt_key_icon("Right Ctrl", false) == "keyboard_ctrl", "right ctrl");
    CHECK(prompt_key_icon("F5", false) == "keyboard_f5", "F5");
    CHECK(prompt_key_icon("7", false) == "keyboard_7", "digit");
    CHECK(prompt_key_icon("Up", false) == "keyboard_arrow_up", "arrow");
    CHECK(prompt_key_icon(",", false) == "keyboard_comma", "comma");
    CHECK(prompt_key_icon("Keypad 7", false).empty(), "no Kenney icon -> empty");
    CHECK(prompt_key_icon("", false).empty(), "empty name");
}

static void style_from_env() {
    CHECK(prompt_style_from_env("color") == PromptStyle::Color, "color");
    CHECK(prompt_style_from_env("gray") == PromptStyle::Gray, "gray");
    CHECK(prompt_style_from_env(nullptr) == PromptStyle::Gray, "unset = gray");
    CHECK(prompt_style_from_env("blue") == PromptStyle::Gray, "unknown = gray");
}

int main() {
    keyboard_defaults();
    pad_defaults();
    remaps();
    key_icons();
    style_from_env();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
