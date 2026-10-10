// psp_pad: SDL GameController -> PSP buttons/analog (pure; no SDL linkage).
#include "psp_pad.h"

#include <SDL_gamecontroller.h>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static void buttons() {
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_A) == 0x4000, "A = CROSS");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_B) == 0x2000, "B = CIRCLE");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_X) == 0x8000, "X = SQUARE");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_Y) == 0x1000, "Y = TRIANGLE");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) == 0x0100, "LB = L");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) == 0x0200, "RB = R");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_START) == 0x0008, "START");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_BACK) == 0x0001, "BACK = SELECT");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_DPAD_UP) == 0x0010, "dpad up");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) == 0x0020, "dpad right");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_DPAD_DOWN) == 0x0040, "dpad down");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_DPAD_LEFT) == 0x0080, "dpad left");
    CHECK(pad_button_to_psp(SDL_CONTROLLER_BUTTON_GUIDE) == 0, "guide unmapped");
}

static void deadzone_centres() {
    CHECK(pad_axis_to_psp(0, 8000) == 128, "rest = 128");
    CHECK(pad_axis_to_psp(7999, 8000) == 128, "inside dead zone = 128");
    CHECK(pad_axis_to_psp(-7999, 8000) == 128, "inside dead zone (neg) = 128");
    CHECK(pad_axis_to_psp(32767, 8000) == 255, "full right = 255");
    CHECK(pad_axis_to_psp(-32768, 8000) == 0, "full left = 0");
    const uint8_t half = pad_axis_to_psp(20384, 8000);  // halfway between dead zone and max
    CHECK(half > 180 && half < 200, "rescaled past the dead zone");
    CHECK(!pad_trigger_pressed(10000) && pad_trigger_pressed(12000), "trigger threshold ~1/3");
}

static void combine_sources() {
    PadSet p;
    p.set_button(1, 0x4000, true);
    p.set_button(2, 0x4000, true);
    p.set_button(1, 0x4000, false);
    CHECK(p.buttons() == 0x4000, "pad 2 still holds CROSS after pad 1 releases it");
    p.set_trigger(1, true, true);
    CHECK(p.buttons() == (0x4000 | 0x0100), "left trigger = L");
    p.set_button(1, 0x0100, true);
    p.set_trigger(1, true, false);
    CHECK((p.buttons() & 0x0100) != 0, "LB still holds L after the trigger releases");
}

static void unplug_releases() {
    PadSet p;
    p.set_button(7, 0x0010, true);
    p.set_stick(7, 10, 250);
    CHECK(p.analog_x() == 10 && p.analog_y() == 250, "stick of the moved pad");
    CHECK(p.stick_x(7) == 10 && p.stick_y(7) == 250, "per-pad stick read back");
    CHECK(p.stick_x(8) == 128 && p.stick_y(8) == 128, "unknown pad stick centred");
    p.remove(7);
    CHECK(p.buttons() == 0, "unplugging releases its buttons");
    CHECK(p.analog_x() == 128 && p.analog_y() == 128, "and centres the stick");
}

int main() {
    buttons();
    deadzone_centres();
    combine_sources();
    unplug_releases();
    if (failures == 0) std::printf("test_pad: all passed\n");
    return failures == 0 ? 0 : 1;
}
