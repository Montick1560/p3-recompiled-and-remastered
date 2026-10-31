#pragma once
// Display mode rules (settings.ini display.window_mode, docs/contracts/settings-v1.md): which mode
// the environment selects, the borderless rectangle (1 px past a free edge so OpenGL drivers keep it
// composed) and the F11 / Alt+Enter toggle. Pure, so the event loop only applies what these return.
#include <cctype>

enum class PspWindowMode { Windowed, Borderless, Fullscreen };

struct PspRect { int x, y, w, h; };

namespace psp_window_mode_detail {
inline bool iequals(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

enum Side { Bottom, Right, Top, Left };

// o shares d's edge on `side` and overlaps it along that edge by > 0 px (corner contact does not count).
// A rect equal to d never touches it, so callers may pass d among the others.
inline bool touches(PspRect d, PspRect o, int side) {
    const bool spans_x = o.x < d.x + d.w && d.x < o.x + o.w, spans_y = o.y < d.y + d.h && d.y < o.y + o.h;
    switch (side) {
    case Bottom: return o.y == d.y + d.h && spans_x;
    case Right: return o.x == d.x + d.w && spans_y;
    case Top: return o.y + o.h == d.y && spans_x;
    default: return o.x + o.w == d.x && spans_y;
    }
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

// A borderless window covers its display (taskbar included, as fullscreen does) plus 1 px past the
// first edge (bottom, right, top, left) no other display touches: OpenGL drivers promote a window that
// matches a monitor exactly to exclusive fullscreen (flicker on focus loss, no Game Bar). With no free
// edge it is 1 px shorter instead.
inline PspRect psp_borderless_rect(PspRect d, const PspRect* others, int count) {
    using namespace psp_window_mode_detail;
    for (int side = Bottom; side <= Left; ++side) {
        bool blocked = false;
        for (int i = 0; i < count; ++i) blocked = blocked || touches(d, others[i], side);
        if (blocked) continue;
        switch (side) {
        case Bottom: return {d.x, d.y, d.w, d.h + 1};
        case Right: return {d.x, d.y, d.w + 1, d.h};
        case Top: return {d.x, d.y - 1, d.w, d.h + 1};
        default: return {d.x - 1, d.y, d.w + 1, d.h};
        }
    }
    return {d.x, d.y, d.w, d.h - 1};
}

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
