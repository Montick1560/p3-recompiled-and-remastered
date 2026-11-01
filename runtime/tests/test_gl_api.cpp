// psp_gl_api.h: which GL APIs the runtime tries (PSPRECOMP_GL_API) and the GLSL header each one gets.
#include "psp_gl_api.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

using Order = std::vector<PspGlApi>;

static void plan_default() {
    for (const char* env : {static_cast<const char*>(nullptr), "auto", "", "garbage"}) {
        const PspGlPlan p = psp_gl_plan(env);
        CHECK((p.order == Order{PspGlApi::Desktop, PspGlApi::Es}), "desktop first, then ES");
        CHECK(!p.fail_desktop, "desktop is not forced to fail");
    }
}

static void plan_forced() {
    CHECK((psp_gl_plan("desktop").order == Order{PspGlApi::Desktop}), "desktop only");
    CHECK((psp_gl_plan("es").order == Order{PspGlApi::Es}), "ES only");
    const PspGlPlan f = psp_gl_plan("fail-desktop");
    CHECK((f.order == Order{PspGlApi::Desktop, PspGlApi::Es}) && f.fail_desktop, "diagnostics: desktop fails, ES follows");
}

static void headers() {
    CHECK(std::strcmp(psp_glsl_header(PspGlApi::Desktop, true), "#version 330 core\n") == 0, "desktop fragment");
    CHECK(std::strcmp(psp_glsl_header(PspGlApi::Desktop, false), "#version 330 core\n") == 0, "desktop vertex");
    CHECK(std::strcmp(psp_glsl_header(PspGlApi::Es, true),
                      "#version 300 es\n#extension GL_EXT_blend_func_extended : require\n"
                      "precision highp float;\nprecision highp int;\n") == 0, "ES fragment");
    CHECK(std::strcmp(psp_glsl_header(PspGlApi::Es, false),
                      "#version 300 es\nprecision highp float;\nprecision highp int;\n") == 0, "ES vertex");
}

int main() {
    plan_default();
    plan_forced();
    headers();
    if (failures == 0) std::printf("gl_api: all tests passed\n");
    return failures ? 1 : 0;
}
