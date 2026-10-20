#include "psp_input_device.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

const char* const kNames[] = {"keyboard", "xbox", "ps4", "ps5"};

std::mutex g_mu;  // the tracker: event loop and debug socket threads
InputDeviceTracker g_tracker;
bool g_frozen = false;
std::atomic<uint8_t> g_current{static_cast<uint8_t>(InputDevice::Keyboard)};

void publish() {
    const uint8_t now = static_cast<uint8_t>(g_tracker.current());
    if (g_current.exchange(now, std::memory_order_relaxed) != now) {
        std::fprintf(stderr, "[PROMPTS] input device: %s\n", kNames[now]);
    }
}

}  // namespace

const char* input_device_name(InputDevice d) {
    const unsigned i = static_cast<unsigned>(d);
    return i < 4 ? kNames[i] : kNames[0];
}

bool input_device_parse(const char* s, InputDevice* out) {
    if (s == nullptr) return false;
    char low[16] = {};
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (i + 1 >= sizeof(low)) return false;
        low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    for (unsigned i = 0; i < 4; i++) {
        if (std::strcmp(low, kNames[i]) == 0) {
            *out = static_cast<InputDevice>(i);
            return true;
        }
    }
    return false;
}

InputDevice input_device_from_controller_type(int sdl_type) {
    switch (sdl_type) {
    case 3:  // SDL_CONTROLLER_TYPE_PS3
    case 4:  // SDL_CONTROLLER_TYPE_PS4
        return InputDevice::DualShock4;
    case 7:  // SDL_CONTROLLER_TYPE_PS5
        return InputDevice::DualSense;
    default:
        return InputDevice::Xbox;
    }
}

void psp_input_device_init() {
    InputDevice d = InputDevice::Keyboard;
    if (input_device_parse(std::getenv("PSPRECOMP_INPUT_DEVICE"), &d)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_tracker.on_pad(d);
        g_frozen = true;
        publish();
        std::fprintf(stderr, "[PROMPTS] input device forced: %s\n", input_device_name(d));
    }
}

InputDevice psp_input_device() {
    return static_cast<InputDevice>(g_current.load(std::memory_order_relaxed));
}

void psp_input_device_note_key() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_frozen) return;
    g_tracker.on_key();
    publish();
}

void psp_input_device_note_pad(InputDevice d) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_frozen) return;
    g_tracker.on_pad(d);
    publish();
}

void psp_input_device_note_connect(InputDevice d) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_frozen) return;
    g_tracker.on_connect(d);
    publish();
}

void psp_input_device_force(InputDevice d) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_tracker.on_pad(d);
    publish();
}
