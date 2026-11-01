// gles_probe: can this PC give the runtime an OpenGL ES 3.0 context through SDL's EGL path (ANGLE on Windows), with
// the dual-source blending the GE blend path needs (include/psp_ge_blend.h: FIX colours and doubled alpha)?
// Run it next to SDL2.dll (+ libEGL.dll / libGLESv2.dll to test ANGLE). Every line starts with [PROBE];
// exit 0 only when the context, GL_EXT_blend_func_extended and the GE fragment outputs all work.
#include <SDL.h>
#include <glad/glad.h>

#include <cstdio>
#include <cstring>

static const char* str(GLenum e) {
    const GLubyte* s = glGetString(e);
    return s ? reinterpret_cast<const char*>(s) : "?";
}

int main(int, char**) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("[PROBE] SDL_Init: failed %s\n", SDL_GetError());
        return 1;
    }
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
    return ctx && ext && ok ? 0 : 2;
}
