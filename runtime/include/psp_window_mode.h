#pragma once
// Display mode rules (settings.ini display.window_mode, docs/contracts/settings-v1.md): which mode
// the environment selects, the borderless rectangle and the F11 / Alt+Enter toggle. Pure, so the
// event loop only applies what these return.
#include <cctype>

enum class PspWindowMode { Windowed, Borderless, Fullscreen };

struct PspRect { int x, y, w, h; };

namespace psp_window_mode_detail {
inline bool iequals(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}
}  // namespace psp_window_mode_detail

// PSPRECOMP_WINDOW_MODE ("windowed" | "borderless" | "fullscreen", any case) wins. Otherwise the
// legacy PSPRECOMP_FULLSCREEN: "1" is fullscreen, anything else windowed.
inline PspWindowMode psp_window_mode(const char* mode_env, const char* fullscreen_env) {
    using psp_window_mode_detail::iequals;
    if (mode_env) {
        if (iequals(mode_env, "windowed")) return PspWindowMode::Windowed;
        if (iequals(mode_env, "borderless")) return PspWindowMode::Borderless;
        if (iequals(mode_env, "fullscreen")) return PspWindowMode::Fullscreen;
    }
    return fullscreen_env && fullscreen_env[0] == '1' && fullscreen_env[1] == '\0'
        ? PspWindowMode::Fullscreen : PspWindowMode::Windowed;
}

// A borderless window covers its display's bounds exactly (taskbar included, as fullscreen does).
inline PspRect psp_borderless_rect(PspRect display_bounds) { return display_bounds; }

// F11 / Alt+Enter: from a window to the configured mode (fullscreen when that is the window
// itself); from either full-display mode back to a window.
inline PspWindowMode psp_window_mode_toggle(PspWindowMode current, PspWindowMode configured) {
    if (current != PspWindowMode::Windowed) return PspWindowMode::Windowed;
    return configured == PspWindowMode::Windowed ? PspWindowMode::Fullscreen : configured;
}

inline const char* psp_window_mode_name(PspWindowMode m) {
    switch (m) {
    case PspWindowMode::Windowed: return "windowed";
    case PspWindowMode::Borderless: return "borderless";
    case PspWindowMode::Fullscreen: return "fullscreen";
    }
    return "windowed";
}
