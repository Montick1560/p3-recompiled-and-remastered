#pragma once
// The player's input device, for on-screen button prompts: the device of the last input wins (a key
// press -> keyboard, a pad button / trigger / stick past its dead zone -> that pad's family). The
// tracker is pure; the globals below are fed by the event loop and read by the GE thread.
#include <cstdint>

enum class InputDevice : uint8_t { Keyboard, Xbox, DualShock4, DualSense };

const char* input_device_name(InputDevice d);              // "keyboard" | "xbox" | "ps4" | "ps5"
bool input_device_parse(const char* s, InputDevice* out);  // those names, case-insensitive

// SDL_GameControllerType (as an int): PS3 / PS4 -> DualShock4, PS5 -> DualSense, anything else
// (Xbox, Steam Deck, virtual and unidentified pads) -> Xbox style.
InputDevice input_device_from_controller_type(int sdl_type);

class InputDeviceTracker {
public:
    void on_key() { set(InputDevice::Keyboard); }
    void on_pad(InputDevice d) { set(d); }
    // A pad connected before any input is the start-up device; later connects change nothing.
    void on_connect(InputDevice d) {
        if (!had_input_) current_ = d;
    }
    InputDevice current() const { return current_; }

private:
    void set(InputDevice d) {
        had_input_ = true;
        current_ = d;
    }
    InputDevice current_ = InputDevice::Keyboard;
    bool had_input_ = false;
};

// Process-wide device. psp_input_device_init() reads PSPRECOMP_INPUT_DEVICE: a valid name forces that
// device and input no longer switches it (tours, screenshots). The note_* calls come from the event
// loop thread; psp_input_device() is a relaxed atomic read, safe on any thread.
void psp_input_device_init();
InputDevice psp_input_device();
void psp_input_device_note_key();
void psp_input_device_note_pad(InputDevice d);
void psp_input_device_note_connect(InputDevice d);
void psp_input_device_force(InputDevice d);  // debug socket `P <name>`: set it now (input still switches it)
