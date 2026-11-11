// gles_probe: can this PC give the runtime an OpenGL ES 3.0 context through SDL's EGL path (ANGLE on Windows), with
// the dual-source blending the GE blend path needs (include/psp_ge_blend.h: FIX colours and doubled alpha)?
// Run it next to SDL2.dll (+ libEGL.dll / libGLESv2.dll to test ANGLE). Every line starts with [PROBE];
// exit 0 only when the context, GL_EXT_blend_func_extended and the GE fragment outputs all work.
// --warp: skip SDL's EGL path and ask ANGLE (libEGL.dll next to it) for its D3D11 WARP device, Windows' software
// rasterizer: what a PC without OpenGL 3.3 or a usable D3D adapter still has (beta report #101).
#include <SDL.h>
#include <SDL_syswm.h>
#include <glad/glad.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

static const char* str(GLenum e) {
    const GLubyte* s = glGetString(e);
    return s ? reinterpret_cast<const char*>(s) : "?";
}

using EglProc = void* (*)(const char*);
static EglProc g_egl_proc = nullptr;
static GLADapiproc egl_load(const char* name) {
    return g_egl_proc ? reinterpret_cast<GLADapiproc>(g_egl_proc(name)) : nullptr;
}

// ANGLE's D3D11 WARP device on a window: true with a current ES 3.0 context.
static bool warp_context(SDL_Window* w) {
    void* lib = SDL_LoadObject("libEGL.dll");
    if (!lib) {
        std::printf("[PROBE] libEGL.dll: not found\n");
        return false;
    }
    g_egl_proc = reinterpret_cast<EglProc>(SDL_LoadFunction(lib, "eglGetProcAddress"));
    using GetDisplay = void* (*)(unsigned, void*, const int*);  // the EXT form takes EGLint attributes
    using Initialize = unsigned (*)(void*, int*, int*);
    using ChooseConfig = unsigned (*)(void*, const int*, void**, int, int*);
    using CreateSurface = void* (*)(void*, void*, void*, const int*);
    using CreateContext = void* (*)(void*, void*, void*, const int*);
    using MakeCurrent = unsigned (*)(void*, void*, void*, void*);
    using GetError = int (*)();
    auto get_display = reinterpret_cast<GetDisplay>(g_egl_proc ? g_egl_proc("eglGetPlatformDisplayEXT") : nullptr);
    auto init = reinterpret_cast<Initialize>(SDL_LoadFunction(lib, "eglInitialize"));
    auto choose = reinterpret_cast<ChooseConfig>(SDL_LoadFunction(lib, "eglChooseConfig"));
    auto surface = reinterpret_cast<CreateSurface>(SDL_LoadFunction(lib, "eglCreateWindowSurface"));
    auto context = reinterpret_cast<CreateContext>(SDL_LoadFunction(lib, "eglCreateContext"));
    auto current = reinterpret_cast<MakeCurrent>(SDL_LoadFunction(lib, "eglMakeCurrent"));
    auto error = reinterpret_cast<GetError>(SDL_LoadFunction(lib, "eglGetError"));
    if (!get_display || !init || !choose || !surface || !context || !current || !error) {
        std::printf("[PROBE] EGL entry points: missing\n");
        return false;
    }
    // EGL_PLATFORM_ANGLE_ANGLE with TYPE = D3D11, DEVICE_TYPE = D3D_WARP
    const int display_attrs[] = {0x3203, 0x3208, 0x3209, 0x320B, 0x3038};
    void* dpy = get_display(0x3202, nullptr, display_attrs);
    int major = 0, minor = 0;
    if (!dpy || !init(dpy, &major, &minor)) {
        std::printf("[PROBE] WARP display: failed 0x%X\n", error());
        return false;
    }
    // RENDERABLE_TYPE ES3, SURFACE_TYPE WINDOW, RGBA 8888, depth 24, stencil 8
    const int config_attrs[] = {0x3040, 0x40, 0x3033, 0x4,  0x3024, 8,  0x3023, 8, 0x3022,
                                8,      0x3021, 8,    0x3025, 24, 0x3026, 8, 0x3038};
    void* config = nullptr;
    int n = 0;
    if (!choose(dpy, config_attrs, &config, 1, &n) || n == 0) {
        std::printf("[PROBE] WARP config: none 0x%X\n", error());
        return false;
    }
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(w, &wm);
    void* surf = surface(dpy, config, wm.info.win.window, nullptr);
    const int ctx_attrs[] = {0x3098, 3, 0x3038};  // CONTEXT_CLIENT_VERSION 3
    void* ctx = surf ? context(dpy, config, nullptr, ctx_attrs) : nullptr;
    if (!ctx || !current(dpy, surf, surf, ctx)) {
        std::printf("[PROBE] WARP surface/context: failed 0x%X\n", error());
        return false;
    }
    std::printf("[PROBE] WARP display: EGL %d.%d\n", major, minor);
    return true;
}

int main(int argc, char** argv) {
    SDL_SetMainReady();
    const bool warp = argc > 1 && std::strcmp(argv[1], "--warp") == 0;
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("[PROBE] SDL_Init: failed %s\n", SDL_GetError());
        return 1;
    }
    if (warp) {
        SDL_Window* w = SDL_CreateWindow("gles_probe", 0, 0, 64, 64, SDL_WINDOW_HIDDEN);
        if (!w || !warp_context(w)) return 1;
        std::printf("[PROBE] context: ok\n[PROBE] gladLoadGL: %d\n", gladLoadGL(egl_load));
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_Window* w = SDL_CreateWindow("gles_probe", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        SDL_GLContext ctx = w ? SDL_GL_CreateContext(w) : nullptr;
        if (!ctx) {
            std::printf("[PROBE] context: failed %s\n", SDL_GetError());
            return 1;
        }
        std::printf("[PROBE] context: ok\n");
        std::printf("[PROBE] gladLoadGL: %d\n", gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)));
    }
    std::printf("[PROBE] GL_VERSION: %s\n[PROBE] GL_RENDERER: %s\n[PROBE] GL_SHADING_LANGUAGE_VERSION: %s\n",
                str(GL_VERSION), str(GL_RENDERER), str(GL_SHADING_LANGUAGE_VERSION));

    GLint n = 0;
    bool ext = false;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const GLubyte* e = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (e && std::strcmp(reinterpret_cast<const char*>(e), "GL_EXT_blend_func_extended") == 0) ext = true;
    }
    GLint dual = 0;
    glGetIntegerv(0x88FC, &dual);  // GL_MAX_DUAL_SOURCE_DRAW_BUFFERS_EXT
    GLfloat points[2] = {}, lines[2] = {};
    glGetFloatv(0x846D, points);  // GL_ALIASED_POINT_SIZE_RANGE
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lines);
    std::printf("[PROBE] GL_EXT_blend_func_extended: %s\n[PROBE] MAX_DUAL_SOURCE_DRAW_BUFFERS: %d\n"
                "[PROBE] point size range: %.0f..%.0f\n[PROBE] line width range: %.0f..%.0f\n",
                ext ? "yes" : "no", dual, points[0], points[1], lines[0], lines[1]);

    // The GE fragment shader's outputs, verbatim (psp_ge_shader.cpp).
    const char* fs =
        "#version 300 es\n#extension GL_EXT_blend_func_extended : require\nprecision highp float;\n"
        "layout(location = 0, index = 0) out vec4 frag_color;\n"
        "layout(location = 0, index = 1) out vec4 frag_color1;\n"
        "void main() { frag_color = vec4(1.0); frag_color1 = vec4(0.5); }\n";
    GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(sh, 1, &fs, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    char log[1024] = {};
    if (!ok) glGetShaderInfoLog(sh, sizeof log, nullptr, log);
    std::printf("[PROBE] fragment shader: %s %s\n", ok ? "ok" : "error", log);
    return ext && ok ? 0 : 2;
}
