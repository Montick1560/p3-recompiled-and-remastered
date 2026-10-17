#pragma once
#include "psp_menu_prompt.h"

// Draws the on-screen keyboard text box over the presented frame while a
// sceUtilityOsk request is open (psp_osk_session.h). GL (main) thread only.
// win_w/win_h: drawable size; rx/ry/rw/rh: the game image rectangle in GL
// window coordinates (bottom-left origin), as present_fit_rect returns it.
// Returns true when the overlay drew (it used its own GL state, so callers
// that track GL state must invalidate their shadow); false when inactive.
bool psp_osk_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh);

// The "back to the menu?" Yes/No box (psp_menu_prompt.h), drawn the same way
// over the game image while psp_menu_prompt() is active; texts follow
// PSPRECOMP_UI_LANG. Same arguments and return value as psp_osk_overlay_draw.
bool psp_menu_prompt_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh);
MenuPrompt& psp_menu_prompt();  // process-wide, main thread only
