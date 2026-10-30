#pragma once
// Game controller -> PSP input (pure). SDL GameController button/axis values
// in, PSP sceCtrl button mask and analog bytes out. Event-loop glue lives in
// psp_event_loop.cpp; this file has no SDL linkage (SDL enums are ints here).
#include <cstdint>
#include <map>

uint32_t pad_button_to_psp(int sdl_button);
uint8_t pad_axis_to_psp(int axis_value, int deadzone);
bool pad_trigger_pressed(int axis_value);

// What a stick does: the PSP analog stick, the D-pad, the four face buttons (up triangle, right circle, down
// cross, left square), or nothing.
enum class StickMode : uint8_t { Analog, Dpad, Face, Off };
bool parse_stick_mode(const char* text, StickMode* out);  // analog|dpad|face|off, any case; false: unchanged

struct StickConfig {
    StickMode left = StickMode::Off, right = StickMode::Analog;
    int deadzone = 8191;  // raw axis units (25 %)
};

// Per-controller state, combined into one PSP pad. Not thread-safe: the
// event loop owns it and publishes the combined values through atomics.
class PadSet {
public:
    void set_button(int32_t pad, uint32_t mask, bool down);
    void set_trigger(int32_t pad, bool left, bool down);
    // Both sticks Analog: the left one becomes Off (only one PSP analog stick).
    void configure(const StickConfig& c);
    // axis: 0 left X, 1 left Y, 2 right X, 3 right Y (SDL order); raw -32768..32767.
    void set_axis(int32_t pad, int axis, int raw);
    void remove(int32_t pad);
    uint32_t buttons() const;           // OR of every pad, stick buttons included
    uint8_t analog_x() const;           // the Analog stick of the most recently moved pad, else 128
    uint8_t analog_y() const;

private:
    uint8_t analog(int axis) const;  // 0 x, 1 y of the Analog stick
    struct Pad {
        uint32_t buttons = 0;
        bool ltrig = false, rtrig = false;
        int axis[4] = {0, 0, 0, 0};
        uint32_t stick_held[2] = {0, 0};  // left, right: the button a Dpad / Face stick holds
    };
    std::map<int32_t, Pad> pads_;
    int32_t last_stick_pad_ = -1;
    StickConfig cfg_;
};
