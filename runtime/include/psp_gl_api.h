#pragma once
// Which GL API the renderer runs on. Desktop OpenGL 3.3 core is tried first; when it cannot start, OpenGL ES 3.0
// through SDL's EGL path (ANGLE on Windows, Direct3D 11 underneath) with GL_EXT_blend_func_extended, which the GE
// blend path needs (psp_ge_blend.h: FIX colours and doubled alpha are dual-source). Both run the same shaders:
// each source starts after psp_glsl_header(). psp_gl_plan and psp_glsl_header are pure; the rest lives in
// psp_gl_api.cpp and runs on the GL thread.
#include <cstring>
#include <vector>

enum class PspGlApi { Desktop, Es };

struct PspGlPlan {
    std::vector<PspGlApi> order;
    bool fail_desktop = false;  // diagnostics: the desktop attempt asks for an impossible version
};

// PSPRECOMP_GL_API: "desktop" -> {Desktop}; "es" -> {Es}; "fail-desktop" -> {Desktop, Es} with the desktop attempt
// forced to fail; unset or anything else -> {Desktop, Es}.
inline PspGlPlan psp_gl_plan(const char* env) {
    if (env && std::strcmp(env, "desktop") == 0) return {{PspGlApi::Desktop}};
    if (env && std::strcmp(env, "es") == 0) return {{PspGlApi::Es}};
    return {{PspGlApi::Desktop, PspGlApi::Es}, env && std::strcmp(env, "fail-desktop") == 0};
}

inline const char* psp_glsl_header(PspGlApi api, bool fragment) {
    if (api == PspGlApi::Desktop) return "#version 330 core\n";
    return fragment ? "#version 300 es\n#extension GL_EXT_blend_func_extended : require\n"
                      "precision highp float;\nprecision highp int;\n"
                    : "#version 300 es\nprecision highp float;\nprecision highp int;\n";
}

PspGlApi psp_gl_api();                        // API of the live context; Desktop until set
void psp_gl_api_set(PspGlApi api);            // once the context exists; on Es loads glClearDepthf / glDepthRangef
void psp_gl_clear_depth(double depth);        // glClearDepth, or glClearDepthf on ES
void psp_gl_depth_range(double n, double f);  // glDepthRange, or glDepthRangef on ES
