#pragma once
#include "psp_menu_prompt.h"
#include "psp_osk_keyboard.h"
#include "psp_toast.h"

// Draws the on-screen keyboard text box over the presented frame while a
// sceUtilityOsk request is open (psp_osk_session.h). GL (main) thread only.
// win_w/win_h: drawable size; rx/ry/rw/rh: the game image rectangle in GL
// window coordinates (bottom-left origin), as present_fit_rect returns it.
// Returns true when the overlay drew (it used its own GL state, so callers
// that track GL state must invalidate their shadow); false when inactive.
bool psp_osk_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh);

// The keyboard the event loop drives and this overlay draws (main thread only).
OskKeyboard& psp_osk_keyboard();
// Restart it (layer, focus, shift, pending accent): called when a request opens.
void psp_osk_keyboard_reset();

// The "back to the menu?" Yes/No box (psp_menu_prompt.h), drawn the same way
// over the game image while psp_menu_prompt() is active; texts follow
// PSPRECOMP_UI_LANG. Same arguments and return value as psp_osk_overlay_draw.
bool psp_menu_prompt_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh);
MenuPrompt& psp_menu_prompt();  // process-wide, main thread only

// Achievement toasts: psp_toast_post rasterizes on the caller's thread (the achievements worker, never
// the GL thread) and queues the images; psp_toast_overlay_draw uploads each once and draws the current
// one in the top right of the game image for kToastMs, in PSPRECOMP_UI_COLORS. Same arguments and
// return value as psp_osk_overlay_draw; costs one atomic load per frame while no toast is queued.
void psp_toast_post(const ToastText& t);
bool psp_toast_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh);
