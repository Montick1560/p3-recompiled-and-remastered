#pragma once
#include "psp_menu_prompt.h"
#include "psp_osk_keyboard.h"
#include "psp_toast.h"

#include <cstdint>
#include <string>
#include <vector>

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

// Short texts that follow game objects (Patapon 3: the ping under each online hero), drawn INTO the game image in
// GE order (psp_ge_draw_tag, psp_ge_observer.h): game windows drawn later cover them, screenshots and recordings
// show them, and they scale with the render scale. Dark outline; RGB only (the target's alpha is kept).
struct WorldTag {
    float x = 0.0f, y = 0.0f;  // PSP pixels: top-centre of the text, or its bottom-centre with above
    uint8_t r = 255, g = 255, b = 255;
    std::string text;
    bool above = false;
    float size_px = 0.0f;  // text height in PSP pixels; 0 = the default size
    int icon = -1;         // frame of the icon strip (psp_world_tag_set_icon) drawn left of the text; -1 none
    float align = 0.5f;    // share of the tag's width left of x: 0.5 centred, 0 starts at x, 1 ends at x
};
// GL thread: draws `t` into the bound render target of fb_w x fb_h pixels (PSP 480x272 scaled), moved clear of
// the tags already drawn this frame; psp_world_tags_begin_frame forgets those.
void psp_world_tag_draw_target(const WorldTag& t, int fb_w, int fb_h);
// Any thread: the PNG strip of `frames` equal frames a tag's `icon` indexes (psp_icon_strip.h); uploaded on the GL
// thread at the next tag draw. False (tags keep drawing without icons) when the file is missing or malformed.
bool psp_world_tag_set_icon(const std::string& png_path, int frames);
void psp_world_tags_begin_frame();
// Heroes standing together put their tags on top of each other: a tag that overlaps one already placed moves a
// line away (up for `above` tags, down otherwise) until it is free; `placed` then holds it. Pure.
struct TagRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};
inline TagRect psp_tag_unstack(TagRect r, std::vector<TagRect>& placed, bool up) {  // NOLINT
    for (bool moved = true; moved;) {
        moved = false;
        for (const TagRect& p : placed) {
            if (r.x < p.x + p.w && p.x < r.x + r.w && r.y < p.y + p.h && p.y < r.y + r.h) {
                r.y = up ? p.y - r.h : p.y + p.h;
                moved = true;
            }
        }
    }
    placed.push_back(r);
    return r;
}
