#include "psp_pad.h"

#include <algorithm>

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

bool pad_trigger_pressed(int v) { return v > 32767 / 3; }

void PadSet::set_button(int32_t pad, uint32_t mask, bool down) {
    Pad& p = pads_[pad];
    p.buttons = down ? (p.buttons | mask) : (p.buttons & ~mask);
}

void PadSet::set_trigger(int32_t pad, bool left, bool down) {
    Pad& p = pads_[pad];
    (left ? p.ltrig : p.rtrig) = down;
}

void PadSet::set_stick(int32_t pad, uint8_t x, uint8_t y) {
    Pad& p = pads_[pad];
    p.x = x;
    p.y = y;
    last_stick_pad_ = pad;
}

void PadSet::remove(int32_t pad) {
    pads_.erase(pad);
    if (last_stick_pad_ == pad) last_stick_pad_ = -1;
}

uint32_t PadSet::buttons() const {
    uint32_t m = 0;
    for (const auto& [id, p] : pads_) {
        m |= p.buttons;
        if (p.ltrig) m |= 0x0100;
        if (p.rtrig) m |= 0x0200;
    }
    return m;
}

uint8_t PadSet::analog_x() const { return stick_x(last_stick_pad_); }
uint8_t PadSet::analog_y() const { return stick_y(last_stick_pad_); }

uint8_t PadSet::stick_x(int32_t pad) const {
    const auto it = pads_.find(pad);
    return it == pads_.end() ? 128 : it->second.x;
}

uint8_t PadSet::stick_y(int32_t pad) const {
    const auto it = pads_.find(pad);
    return it == pads_.end() ? 128 : it->second.y;
}
