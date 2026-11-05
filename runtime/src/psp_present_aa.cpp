// Screen anti-aliasing at present time (psp_present_aa.h).
// FXAA: one pass, the classic FXAA by Timothy Lottes as ported in github.com/mattdesl/glsl-fxaa (MIT).
// SMAA: the three SMAA 1x passes (third_party/smaa, MIT) at SMAA_PRESET_MEDIUM. SMAA is written for a top-left
// image origin; GL's is bottom-left, so the frame is copied upside down first and the result flipped back.
#include "psp_present_aa.h"

#include <glad/glad.h>

#include <cstdio>
#include <string>
#include <vector>

#include "psp_gl_api.h"

#include "AreaTex.h"
#include "SearchTex.h"

struct EmbeddedFile { const char* name; const unsigned char* data; unsigned size; };
extern const EmbeddedFile g_smaa_source[];  // smaa_source_data.cpp (generated from third_party/smaa/SMAA.hlsl)

namespace {

struct Target {
    GLuint tex = 0, fbo = 0;
};

struct State {
    int w = 0, h = 0;
    bool failed_fxaa = false, failed_smaa = false;
    GLuint vao = 0;
    GLuint fxaa = 0;
    GLuint smaa_edge = 0, smaa_blend = 0, smaa_neigh = 0;
    GLuint area = 0, search = 0;
    Target post, flip, edges, weights, out;
} g;

// A fullscreen triangle from gl_VertexID (no buffers); v_uv covers [0,1] over the viewport.
const char* kFullscreenVs = R"(
out vec2 v_uv;
vec2 fullscreen_pos() {
    return vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
}
)";

const char* kFxaaVs = R"(
void main() {
    vec2 p = fullscreen_pos();
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char* kFxaaFs = R"(
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_inv;  // 1 / resolution
out vec4 o_color;
#define FXAA_REDUCE_MIN (1.0 / 128.0)
#define FXAA_REDUCE_MUL (1.0 / 8.0)
#define FXAA_SPAN_MAX 8.0
void main() {
    vec3 rgbNW = texture(u_tex, v_uv + vec2(-1.0, -1.0) * u_inv).xyz;
    vec3 rgbNE = texture(u_tex, v_uv + vec2(1.0, -1.0) * u_inv).xyz;
    vec3 rgbSW = texture(u_tex, v_uv + vec2(-1.0, 1.0) * u_inv).xyz;
    vec3 rgbSE = texture(u_tex, v_uv + vec2(1.0, 1.0) * u_inv).xyz;
    vec4 texColor = texture(u_tex, v_uv);
    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma), lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma), lumaSE = dot(rgbSE, luma);
    float lumaM = dot(texColor.xyz, luma);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * u_inv;
    vec3 rgbA = 0.5 * (texture(u_tex, v_uv + dir * (1.0 / 3.0 - 0.5)).xyz +
                       texture(u_tex, v_uv + dir * (2.0 / 3.0 - 0.5)).xyz);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(u_tex, v_uv + dir * -0.5).xyz + texture(u_tex, v_uv + dir * 0.5).xyz);
    float lumaB = dot(rgbB, luma);
    o_color = vec4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, texColor.a);
}
)";

// SMAA stage mains, appended after the SMAA source.
const char* kSmaaEdgeVs = R"(
out vec4 v_off[3];
void main() {
    vec2 p = fullscreen_pos();
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
    SMAAEdgeDetectionVS(v_uv, v_off);
}
)";
const char* kSmaaEdgeFs = R"(
in vec2 v_uv;
in vec4 v_off[3];
uniform sampler2D u_color;
out vec4 o_color;
void main() { o_color = vec4(SMAALumaEdgeDetectionPS(v_uv, v_off, u_color), 0.0, 0.0); }
)";
const char* kSmaaBlendVs = R"(
out vec2 v_pix;
out vec4 v_off[3];
void main() {
    vec2 p = fullscreen_pos();
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
    SMAABlendingWeightCalculationVS(v_uv, v_pix, v_off);
}
)";
const char* kSmaaBlendFs = R"(
in vec2 v_uv;
in vec2 v_pix;
in vec4 v_off[3];
uniform sampler2D u_edges;
uniform sampler2D u_area;
uniform sampler2D u_search;
out vec4 o_color;
void main() { o_color = SMAABlendingWeightCalculationPS(v_uv, v_pix, v_off, u_edges, u_area, u_search, vec4(0.0)); }
)";
const char* kSmaaNeighVs = R"(
out vec4 v_off;
void main() {
    vec2 p = fullscreen_pos();
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
    SMAANeighborhoodBlendingVS(v_uv, v_off);
}
)";
const char* kSmaaNeighFs = R"(
in vec2 v_uv;
in vec4 v_off;
uniform sampler2D u_color;
uniform sampler2D u_blend;
out vec4 o_color;
void main() { o_color = SMAANeighborhoodBlendingPS(v_uv, v_off, u_color, u_blend); }
)";

GLuint compile(GLenum type, const std::vector<std::string>& parts, const char* what) {
    std::vector<const char*> srcs;
    std::vector<GLint> lens;
    for (const std::string& p : parts) {
        srcs.push_back(p.data());
        lens.push_back(static_cast<GLint>(p.size()));
    }
    GLuint s = glCreateShader(type);
    glShaderSource(s, static_cast<GLsizei>(srcs.size()), srcs.data(), lens.data());
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[AA] %s %s shader: %s\n", what, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(const std::vector<std::string>& vs, const std::vector<std::string>& fs, const char* what) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, what);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, what);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[AA] %s link: %s\n", what, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

std::string header(bool fragment) { return psp_glsl_header(psp_gl_api(), fragment); }

GLuint make_texture(int w, int h, GLenum internal, GLenum format, GLenum filter, const void* px) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), w, h, 0, format, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

Target make_target(int w, int h) {
    Target t;
    t.tex = make_texture(w, h, GL_RGBA8, GL_RGBA, GL_LINEAR, nullptr);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    return t;
}

void drop(Target& t) {
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    t = Target{};
}

// Frame-size targets follow the internal resolution (render scale), not the window.
void ensure_targets(PspAa mode, int w, int h) {
    if (g.w != w || g.h != h) {
        for (Target* t : {&g.post, &g.flip, &g.edges, &g.weights, &g.out}) drop(*t);
        g.w = w;
        g.h = h;
    }
    if (!g.post.fbo) g.post = make_target(w, h);
    if (mode == PspAa::Smaa && !g.flip.fbo) {
        g.flip = make_target(w, h);
        g.edges = make_target(w, h);
        g.weights = make_target(w, h);
        g.out = make_target(w, h);
    }
}

bool ensure_fxaa() {
    if (g.fxaa || g.failed_fxaa) return g.fxaa != 0;
    g.fxaa = link({header(false), kFullscreenVs, kFxaaVs}, {header(true), kFxaaFs}, "FXAA");
    g.failed_fxaa = g.fxaa == 0;
    return g.fxaa != 0;
}

bool ensure_smaa() {
    if (g.smaa_neigh || g.failed_smaa) return g.smaa_neigh != 0;
    const std::string smaa(reinterpret_cast<const char*>(g_smaa_source[0].data), g_smaa_source[0].size);
    const std::string defs = "#define SMAA_GLSL_3\n#define SMAA_PRESET_MEDIUM\nuniform vec4 u_metrics;\n"
                             "#define SMAA_RT_METRICS u_metrics\n";
    auto vs = [&](const char* main) {
        return std::vector<std::string>{header(false), defs, "#define SMAA_INCLUDE_VS 1\n#define SMAA_INCLUDE_PS 0\n",
                                        smaa, kFullscreenVs, main};
    };
    auto fs = [&](const char* main) {
        return std::vector<std::string>{header(true), "precision highp float;\n", defs,
                                        "#define SMAA_INCLUDE_VS 0\n#define SMAA_INCLUDE_PS 1\n", smaa, main};
    };
    g.smaa_edge = link(vs(kSmaaEdgeVs), fs(kSmaaEdgeFs), "SMAA edges");
    g.smaa_blend = link(vs(kSmaaBlendVs), fs(kSmaaBlendFs), "SMAA weights");
    g.smaa_neigh = link(vs(kSmaaNeighVs), fs(kSmaaNeighFs), "SMAA blend");
    if (!g.smaa_edge || !g.smaa_blend || !g.smaa_neigh) {
        for (GLuint* p : {&g.smaa_edge, &g.smaa_blend, &g.smaa_neigh})
            if (*p) { glDeleteProgram(*p); *p = 0; }
        g.failed_smaa = true;
        return false;
    }
    g.area = make_texture(AREATEX_WIDTH, AREATEX_HEIGHT, GL_RG8, GL_RG, GL_LINEAR, areaTexBytes);
    g.search = make_texture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, GL_R8, GL_RED, GL_NEAREST, searchTexBytes);
    return true;
}

void bind_tex(int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void draw_pass(GLuint program, GLuint fbo, bool clear) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, g.w, g.h);
    if (clear) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glUseProgram(program);
    const GLint m = glGetUniformLocation(program, "u_metrics");
    if (m >= 0) glUniform4f(m, 1.0f / g.w, 1.0f / g.h, float(g.w), float(g.h));
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void set_unit(GLuint program, const char* name, int unit) {
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, name), unit);
}

}  // namespace

unsigned psp_present_aa_apply(PspAa mode, unsigned src_fbo, unsigned color_tex, int w, int h) {
    if (mode == PspAa::Off || w <= 0 || h <= 0) return 0;
    if (mode == PspAa::Fxaa ? !ensure_fxaa() : !ensure_smaa()) return 0;
    ensure_targets(mode, w, h);
    if (!g.vao) glGenVertexArrays(1, &g.vao);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(g.vao);

    if (mode == PspAa::Fxaa) {
        set_unit(g.fxaa, "u_tex", 0);
        glUniform2f(glGetUniformLocation(g.fxaa, "u_inv"), 1.0f / w, 1.0f / h);
        // FXAA samples between texels; the frame texture is NEAREST for the GE (no sampler objects: they are GL 3.3,
        // and the GL loader stops at 3.0 under ANGLE's ES 3.0), so it is linear for this pass only.
        bind_tex(0, color_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        draw_pass(g.fxaa, g.post.fbo, false);
        bind_tex(0, color_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        // Upside down into flip (SMAA's top-left origin), the three passes, then right way up into post.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.flip.fbo);
        glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        set_unit(g.smaa_edge, "u_color", 0);
        bind_tex(0, g.flip.tex);
        draw_pass(g.smaa_edge, g.edges.fbo, true);
        set_unit(g.smaa_blend, "u_edges", 0);
        set_unit(g.smaa_blend, "u_area", 1);
        set_unit(g.smaa_blend, "u_search", 2);
        bind_tex(0, g.edges.tex);
        bind_tex(1, g.area);
        bind_tex(2, g.search);
        draw_pass(g.smaa_blend, g.weights.fbo, true);
        set_unit(g.smaa_neigh, "u_color", 0);
        set_unit(g.smaa_neigh, "u_blend", 1);
        bind_tex(0, g.flip.tex);
        bind_tex(1, g.weights.tex);
        draw_pass(g.smaa_neigh, g.out.fbo, false);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.out.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.post.fbo);
        glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        bind_tex(2, 0);
        bind_tex(1, 0);
    }
    bind_tex(0, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return g.post.fbo;
}
