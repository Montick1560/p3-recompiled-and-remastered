// psp_pad: SDL GameController -> PSP buttons/analog (pure; no SDL linkage).
#include "psp_pad.h"

#include <SDL_gamecontroller.h>
#include <cstdio>
#include <string>
#include <vector>

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
    p.set_axis(7, 2, -32768);
    p.set_axis(7, 3, 32767);
    CHECK(p.analog_x() == 0 && p.analog_y() == 255, "right stick of the moved pad (default analog)");
    p.remove(7);
    CHECK(p.buttons() == 0, "unplugging releases its buttons");
    CHECK(p.analog_x() == 128 && p.analog_y() == 128, "and centres the stick");
}

static void sticks() {
    PadSet p;  // default config: left nothing, right analog
    p.set_axis(1, 2, -32768);
    CHECK(p.analog_x() == pad_axis_to_psp(-32768, 8191) && p.buttons() == 0, "default: right = analog, no buttons");
    p.set_axis(1, 0, 32767);
    CHECK(p.buttons() == 0 && p.analog_x() == pad_axis_to_psp(-32768, 8191), "default: left stick does nothing");

    PadSet f;
    f.configure({StickMode::Analog, StickMode::Face, 8000});
    f.set_axis(1, 2, 20000);
    CHECK(f.buttons() == 0x2000, "right = circle (PON)");
    f.set_axis(1, 2, 7000);
    CHECK(f.buttons() == 0x2000, "inside the 3/4 band: still held");
    f.set_axis(1, 2, 5000);
    CHECK(f.buttons() == 0, "below 6000: released");
    f.set_axis(1, 3, -20000);
    CHECK(f.buttons() == 0x1000, "up = triangle (CHAKA)");
    f.set_axis(1, 2, 25000);
    CHECK(f.buttons() == 0x2000, "dominant axis wins: one drum only");
    f.set_axis(1, 2, 0);
    f.set_axis(1, 3, 0);
    CHECK(f.buttons() == 0, "centred");
    f.set_axis(1, 3, 20000);
    CHECK(f.buttons() == 0x4000, "down = cross (DON)");
    f.set_axis(1, 3, 0);
    f.set_axis(1, 2, -20000);
    CHECK(f.buttons() == 0x8000, "left = square (PATA)");

    PadSet d;
    d.configure({StickMode::Dpad, StickMode::Off, 8000});
    d.set_axis(1, 1, 30000);
    CHECK(d.buttons() == 0x0040 && d.analog_y() == 128, "left = D-pad down, analog centred");
    d.set_axis(1, 2, 30000);
    CHECK(d.buttons() == 0x0040 && d.analog_x() == 128, "right off: nothing");

    PadSet two;
    two.configure({StickMode::Analog, StickMode::Face, 8000});
    two.set_axis(1, 2, 30000);
    two.set_axis(2, 2, 0);
    CHECK(two.buttons() == 0x2000, "another pad's idle stick does not release it");
    two.remove(1);
    CHECK(two.buttons() == 0, "unplugged pad releases its stick button");

    PadSet both;
    both.configure({StickMode::Analog, StickMode::Analog, 8000});
    both.set_axis(1, 0, 30000);
    CHECK(both.analog_x() == 128 && both.buttons() == 0, "only one analog stick: the right one");
    both.set_axis(1, 2, 30000);
    CHECK(both.analog_x() == pad_axis_to_psp(30000, 8000), "right stays analog");

    StickMode m;
    CHECK(parse_stick_mode("Face", &m) && m == StickMode::Face && !parse_stick_mode("x", &m) &&
              parse_stick_mode("off", &m) && m == StickMode::Off,
          "parse");
}

static void config_from_env() {
    std::vector<std::string> log;
    StickConfig c = stick_config_from_env(nullptr, nullptr, nullptr, &log);
    CHECK(c.left == StickMode::Off && c.right == StickMode::Analog && c.deadzone == 8191 && log.empty(),
          "unset: defaults, no log");
    c = stick_config_from_env("dpad", "face", "40", &log);
    CHECK(c.left == StickMode::Dpad && c.right == StickMode::Face && c.deadzone == 13106 && log.empty(), "values");
    c = stick_config_from_env("bogus", nullptr, "99", &log);
    CHECK(c.left == StickMode::Off && c.deadzone == 8191 && log.size() == 2, "invalid: defaults + two lines");
}

static void keys_as_stick() {
    CHECK(analog_from_keys(0, 0x6050) == 0x6050, "no key: the pad's stick");
    CHECK(analog_from_keys(kAnalogLeft, 0x6050) == 0x6000, "left: x = 0, pad y kept");
    CHECK(analog_from_keys(kAnalogRight | kAnalogDown, 0x8080) == 0xFFFF, "right + down: 255, 255");
    CHECK(analog_from_keys(kAnalogLeft | kAnalogRight, 0x8050) == 0x8080, "both ways: x centres");
    CHECK(analog_from_keys(kAnalogUp | 0x4000, 0x8080) == 0x0080, "up: y = 0, other buttons ignored");
}

int main() {
    buttons();
    keys_as_stick();
    deadzone_centres();
    combine_sources();
    unplug_releases();
    sticks();
    config_from_env();
    if (failures == 0) std::printf("test_pad: all passed\n");
    return failures == 0 ? 0 : 1;
}
