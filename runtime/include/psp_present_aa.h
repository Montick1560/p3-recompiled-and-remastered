#pragma once
// Screen anti-aliasing at present time (PSPRECOMP_AA = off | fxaa | smaa): the front frame's colour texture is
// filtered at the internal resolution into a post FBO, which the window blit then scales as before. Overlays
// (keyboard, prompts, toasts) are drawn after it, and screenshots read the frame before it.

enum class PspAa { Off, Fxaa, Smaa };

// "fxaa" / "smaa" in any case; anything else (null, "", "off") is Off.
PspAa psp_aa_parse(const char* env);

// GL thread. Runs `mode` on the frame (src_fbo with colour texture color_tex, w x h, RGBA8) and returns the FBO
// holding the result, or 0 when mode is Off or the passes are unavailable (shader error: logged once, then Off); the
// caller then blits its own FBO as before. Changes GL state (the caller calls gl_state_invalidate()).
unsigned psp_present_aa_apply(PspAa mode, unsigned src_fbo, unsigned color_tex, int w, int h);
