#pragma once

// Destination rectangle (GL window coordinates, origin bottom-left) for the
// 480x272 PSP image inside a drawable of win_w x win_h: the largest rectangle
// with the 480:272 aspect ratio that fits, centred; integer pixels.
struct PresentRect { int x, y, w, h; };
PresentRect present_fit_rect(int win_w, int win_h);

// Initial window scale from PSPRECOMP_WINDOW_SCALE: 1..8, anything else -> 2.
int present_window_scale(const char* env);

// Internal render scale from PSPRECOMP_RENDER_SCALE: 1..8, anything else -> 1.
// 1 = native 480x272, the setting every PPSSPP comparison uses.
int present_render_scale(const char* env);
