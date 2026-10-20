// Unit tests for psp_input_device: device names, SDL controller type -> icon family, and the
// last-input tracker (a connect only picks the start-up device; real input always wins).
#include "psp_input_device.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void names_round_trip() {
    const InputDevice all[] = {InputDevice::Keyboard, InputDevice::Xbox, InputDevice::DualShock4,
                               InputDevice::DualSense};
    for (InputDevice d : all) {
        InputDevice back = InputDevice::Keyboard;
        CHECK(input_device_parse(input_device_name(d), &back) && back == d, "name round-trips");
    }
    InputDevice d = InputDevice::Keyboard;
    CHECK(input_device_parse("PS5", &d) && d == InputDevice::DualSense, "case-insensitive");
    CHECK(std::strcmp(input_device_name(InputDevice::DualShock4), "ps4") == 0, "ps4 name");
    CHECK(!input_device_parse("switch", &d) && !input_device_parse(nullptr, &d), "unknown names rejected");
}

static void controller_types() {
    // SDL_GameControllerType: 0 unknown, 1 360, 2 One, 3 PS3, 4 PS4, 5 Switch Pro, 6 virtual, 7 PS5.
    const InputDevice want[] = {InputDevice::Xbox, InputDevice::Xbox, InputDevice::Xbox,
                                InputDevice::DualShock4, InputDevice::DualShock4, InputDevice::Xbox,
                                InputDevice::Xbox, InputDevice::DualSense};
    for (int t = 0; t < 8; t++) CHECK(input_device_from_controller_type(t) == want[t], "controller type family");
    CHECK(input_device_from_controller_type(42) == InputDevice::Xbox, "future types are Xbox style");
}

static void tracker_last_input_wins() {
    InputDeviceTracker t;
    CHECK(t.current() == InputDevice::Keyboard, "starts on the keyboard");
    t.on_connect(InputDevice::DualSense);
    CHECK(t.current() == InputDevice::DualSense, "a pad connected before any input is the start-up device");
    t.on_key();
    CHECK(t.current() == InputDevice::Keyboard, "a key press switches to the keyboard");
    t.on_connect(InputDevice::Xbox);
    CHECK(t.current() == InputDevice::Keyboard, "a connect after real input does not switch");
    t.on_pad(InputDevice::DualShock4);
    CHECK(t.current() == InputDevice::DualShock4, "pad input switches to that pad");
}

int main() {
    names_round_trip();
    controller_types();
    tracker_last_input_wins();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
