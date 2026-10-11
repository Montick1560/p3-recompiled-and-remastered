#pragma once
// Draws the on-screen keyboard text box over the presented frame while a
// sceUtilityOsk request is open (psp_osk_session.h). GL (main) thread only.
// win_w/win_h: drawable size; rx/ry/rw/rh: the game image rectangle in GL
// window coordinates (bottom-left origin), as present_fit_rect returns it.
void psp_osk_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh);
