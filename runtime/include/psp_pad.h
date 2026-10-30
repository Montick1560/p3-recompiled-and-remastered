#pragma once
// Game controller -> PSP input (pure). SDL GameController button/axis values
// in, PSP sceCtrl button mask and analog bytes out. Event-loop glue lives in
// psp_event_loop.cpp; this file has no SDL linkage (SDL enums are ints here).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

uint32_t pad_button_to_psp(int sdl_button);
uint8_t pad_axis_to_psp(int axis_value, int deadzone);
bool pad_trigger_pressed(int axis_value);

// Keys (or pad buttons) bound to the analog stick ([controls] analog_up ...) travel in the button words as these
// bits, which no PSP button uses; analog_from_keys turns them into the analog bytes and the game never sees them.
constexpr uint32_t kAnalogUp = 0x10000000, kAnalogDown = 0x20000000, kAnalogLeft = 0x40000000,
                   kAnalogRight = 0x80000000, kAnalogBits = 0xF0000000;
// pad_analog and the result are packed x | y << 8. Per axis: one direction held gives 0 or 255, both give 128,
// neither keeps the pad's value.
uint16_t analog_from_keys(uint32_t buttons, uint16_t pad_analog);

// What a stick does: the PSP analog stick, the D-pad, the four face buttons (up triangle, right circle, down
// cross, left square), or nothing.
enum class StickMode : uint8_t { Analog, Dpad, Face, Off };
bool parse_stick_mode(const char* text, StickMode* out);  // analog|dpad|face|off, any case; false: unchanged

struct StickConfig {
    StickMode left = StickMode::Off, right = StickMode::Analog;
    int deadzone = 8191;  // raw axis units (25 %)
};
// PSPRECOMP_PAD_LEFT_STICK / _RIGHT_STICK (a mode name) and PSPRECOMP_PAD_DEADZONE (10..60 %); unset keeps the
// default, an invalid value keeps it too and adds one "[CONTROLS] ..." line.
StickConfig stick_config_from_env(const char* left, const char* right, const char* deadzone_pct,
                                  std::vector<std::string>* log);

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
