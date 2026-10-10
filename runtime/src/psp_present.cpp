#include "psp_present.h"

#include <cstdlib>

PresentRect present_fit_rect(int win_w, int win_h) {
    if (win_w <= 0 || win_h <= 0) return {0, 0, 0, 0};
    int w, h;
    if (static_cast<long long>(win_w) * 272 >= static_cast<long long>(win_h) * 480) {
        h = win_h;  // window wider than 480:272: full height, bars left/right
        w = static_cast<int>(static_cast<long long>(win_h) * 480 / 272);
    } else {
        w = win_w;  // taller: full width, bars top/bottom
        h = static_cast<int>(static_cast<long long>(win_w) * 272 / 480);
    }
    return {(win_w - w) / 2, (win_h - h) / 2, w, h};
}

int present_window_scale(const char* env) {
    if (env == nullptr || env[0] == '\0') return 2;
    const int s = std::atoi(env);
    return (s >= 1 && s <= 8) ? s : 2;
}

int present_render_scale(const char* env) {
    if (env == nullptr || env[0] == '\0') return 1;
    const int s = std::atoi(env);
    return (s >= 1 && s <= 8) ? s : 1;
}
