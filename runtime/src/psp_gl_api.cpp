// psp_gl_api: the live context's API and the two desktop-only calls the renderer needs (psp_gl_api.h).
#include "psp_gl_api.h"

#include <SDL.h>
#include <glad/glad.h>

static PspGlApi g_api = PspGlApi::Desktop;
// ES spells these with floats; the GLAD loader is GL 3.3 core, so they come from SDL.
static void (GLAD_API_PTR* g_clear_depthf)(GLfloat) = nullptr;
static void (GLAD_API_PTR* g_depth_rangef)(GLfloat, GLfloat) = nullptr;

PspGlApi psp_gl_api() { return g_api; }

void psp_gl_api_set(PspGlApi api) {
    g_api = api;
    if (api != PspGlApi::Es) return;
    g_clear_depthf = reinterpret_cast<decltype(g_clear_depthf)>(SDL_GL_GetProcAddress("glClearDepthf"));
    g_depth_rangef = reinterpret_cast<decltype(g_depth_rangef)>(SDL_GL_GetProcAddress("glDepthRangef"));
}

void psp_gl_clear_depth(double depth) {
    if (g_api == PspGlApi::Es) g_clear_depthf(static_cast<GLfloat>(depth));
    else glClearDepth(depth);
}

void psp_gl_depth_range(double n, double f) {
    if (g_api == PspGlApi::Es) g_depth_rangef(static_cast<GLfloat>(n), static_cast<GLfloat>(f));
    else glDepthRange(n, f);
}
