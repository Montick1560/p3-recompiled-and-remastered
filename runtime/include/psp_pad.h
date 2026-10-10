#pragma once
// Game controller -> PSP input (pure). SDL GameController button/axis values
// in, PSP sceCtrl button mask and analog bytes out. Event-loop glue lives in
// psp_event_loop.cpp; this file has no SDL linkage (SDL enums are ints here).
#include <cstdint>
#include <map>

uint32_t pad_button_to_psp(int sdl_button);
uint8_t pad_axis_to_psp(int axis_value, int deadzone);
bool pad_trigger_pressed(int axis_value);

// Per-controller state, combined into one PSP pad. Not thread-safe: the
// event loop owns it and publishes the combined values through atomics.
class PadSet {
public:
    void set_button(int32_t pad, uint32_t mask, bool down);
    void set_trigger(int32_t pad, bool left, bool down);
    void set_stick(int32_t pad, uint8_t x, uint8_t y);
    void remove(int32_t pad);
    uint32_t buttons() const;           // OR of every pad
    uint8_t analog_x() const;           // most recently moved pad, else 128
    uint8_t analog_y() const;
    uint8_t stick_x(int32_t pad) const; // one pad's stick, 128 if unknown
    uint8_t stick_y(int32_t pad) const;

private:
    struct Pad {
        uint32_t buttons = 0;
        bool ltrig = false, rtrig = false;
        uint8_t x = 128, y = 128;
    };
    std::map<int32_t, Pad> pads_;
    int32_t last_stick_pad_ = -1;
};
