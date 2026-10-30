#include "psp_pad.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

uint32_t pad_button_to_psp(int b) {
    // SDL_GameControllerButton values (SDL 2.0.x, stable ABI).
    switch (b) {
    case 0: return 0x4000;   // A -> CROSS
    case 1: return 0x2000;   // B -> CIRCLE
    case 2: return 0x8000;   // X -> SQUARE
    case 3: return 0x1000;   // Y -> TRIANGLE
    case 4: return 0x0001;   // BACK -> SELECT
    case 6: return 0x0008;   // START
    case 9: return 0x0100;   // LEFTSHOULDER -> L
    case 10: return 0x0200;  // RIGHTSHOULDER -> R
    case 11: return 0x0010;  // DPAD_UP
    case 12: return 0x0040;  // DPAD_DOWN
    case 13: return 0x0080;  // DPAD_LEFT
    case 14: return 0x0020;  // DPAD_RIGHT
    default: return 0;
    }
}

uint8_t pad_axis_to_psp(int v, int deadzone) {
    if (v > -deadzone && v < deadzone) return 128;
    const double span = 32767.0 - deadzone;
    const double t = v > 0 ? (v - deadzone) / span : (v + deadzone) / span;  // -1..1
    const int out = static_cast<int>(128.0 + std::clamp(t, -1.0, 1.0) * 127.5);
    return static_cast<uint8_t>(std::clamp(out, 0, 255));
}

uint16_t analog_from_keys(uint32_t buttons, uint16_t pad_analog) {
    const auto axis = [&](uint32_t neg, uint32_t pos, uint16_t pad) -> uint16_t {
        const bool n = buttons & neg, p = buttons & pos;
        return n && p ? 128 : n ? 0 : p ? 255 : pad;
    };
    return static_cast<uint16_t>(axis(kAnalogLeft, kAnalogRight, pad_analog & 0xFF) |
                                 axis(kAnalogUp, kAnalogDown, pad_analog >> 8) << 8);
}

bool pad_trigger_pressed(int v) { return v > 32767 / 3; }

void PadSet::set_button(int32_t pad, uint32_t mask, bool down) {
    Pad& p = pads_[pad];
    p.buttons = down ? (p.buttons | mask) : (p.buttons & ~mask);
}

void PadSet::set_trigger(int32_t pad, bool left, bool down) {
    Pad& p = pads_[pad];
    (left ? p.ltrig : p.rtrig) = down;
}

bool parse_stick_mode(const char* text, StickMode* out) {
    std::string t = text ? text : "";
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, StickMode> kModes[] = {
        {"analog", StickMode::Analog}, {"dpad", StickMode::Dpad}, {"face", StickMode::Face}, {"off", StickMode::Off}};
    for (const auto& [name, mode] : kModes)
        if (t == name) return *out = mode, true;
    return false;
}

StickConfig stick_config_from_env(const char* left, const char* right, const char* deadzone_pct,
                                  std::vector<std::string>* log) {
    StickConfig c;
    const auto mode = [&](const char* v, StickMode* m, const char* what) {
        if (v && !parse_stick_mode(v, m)) log->push_back(std::string("[CONTROLS] ") + what + ": unknown mode \"" + v + "\"");
    };
    mode(left, &c.left, "left stick");
    mode(right, &c.right, "right stick");
    if (deadzone_pct) {
        char* end = nullptr;
        const long pct = std::strtol(deadzone_pct, &end, 10);
        if (end != deadzone_pct && *end == 0 && pct >= 10 && pct <= 60) c.deadzone = static_cast<int>(pct * 32767 / 100);
        else log->push_back(std::string("[CONTROLS] stick dead zone: \"") + deadzone_pct + "\" is not 10..60");
    }
    return c;
}

void PadSet::configure(const StickConfig& c) {
    cfg_ = c;
    if (cfg_.left == StickMode::Analog && cfg_.right == StickMode::Analog) cfg_.left = StickMode::Off;
}

// The one button a Dpad / Face stick holds: the dominant axis, pressed beyond the dead zone and kept down to 3/4
// of it, so a stick resting near the edge does not flicker and a diagonal never presses two.
static uint32_t stick_button(StickMode mode, int x, int y, int deadzone, uint32_t held) {
    static const uint32_t kDpad[4] = {0x0010, 0x0020, 0x0040, 0x0080};  // up right down left
    static const uint32_t kFace[4] = {0x1000, 0x2000, 0x4000, 0x8000};  // triangle circle cross square
    const bool horizontal = std::abs(x) >= std::abs(y);
    const int v = horizontal ? x : y;
    const int dir = horizontal ? (v > 0 ? 1 : 3) : (v > 0 ? 2 : 0);
    const uint32_t b = (mode == StickMode::Dpad ? kDpad : kFace)[dir];
    const int threshold = b == held ? deadzone * 3 / 4 : deadzone;
    return std::abs(v) > threshold ? b : 0;
}

void PadSet::set_axis(int32_t pad, int axis, int raw) {
    if (axis < 0 || axis > 3) return;
    Pad& p = pads_[pad];
    p.axis[axis] = raw;
    const int s = axis / 2;
    const StickMode mode = s == 0 ? cfg_.left : cfg_.right;
    if (mode == StickMode::Analog) last_stick_pad_ = pad;
    else if (mode != StickMode::Off)
        p.stick_held[s] = stick_button(mode, p.axis[2 * s], p.axis[2 * s + 1], cfg_.deadzone, p.stick_held[s]);
}

void PadSet::remove(int32_t pad) {
    pads_.erase(pad);
    if (last_stick_pad_ == pad) last_stick_pad_ = -1;
}

uint32_t PadSet::buttons() const {
    uint32_t m = 0;
    for (const auto& [id, p] : pads_) {
        m |= p.buttons | p.stick_held[0] | p.stick_held[1];
        if (p.ltrig) m |= 0x0100;
        if (p.rtrig) m |= 0x0200;
    }
    return m;
}

uint8_t PadSet::analog_x() const { return analog(0); }
uint8_t PadSet::analog_y() const { return analog(1); }

uint8_t PadSet::analog(int axis) const {
    const int stick = cfg_.right == StickMode::Analog ? 1 : cfg_.left == StickMode::Analog ? 0 : -1;
    const auto it = pads_.find(last_stick_pad_);
    if (stick < 0 || it == pads_.end()) return 128;
    return pad_axis_to_psp(it->second.axis[2 * stick + axis], cfg_.deadzone);
}
