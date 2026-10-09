#include "psp_ge_shader.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"

#include <glad/glad.h>
#include <cstdio>

// ---- Shader source ----

static const char* const k_vertex_shader_src = R"GLSL(
#version 330 core

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec2 a_texcoord;
layout(location = 2) in vec4 a_color;

out vec2 v_texcoord;
out vec4 v_color;

void main() {
    // CPU sends clip-space (x,y,z,w) in transform mode (GL's own
    // perspective divide gives near-plane clipping and perspective-
    // correct UV interpolation) or NDC with w = 1 in through mode /
    // fallback paths. Pass through untouched.
    gl_Position = a_position;
    v_texcoord = a_texcoord;
    v_color = a_color;
}
)GLSL";

static const char* const k_fragment_shader_src = R"GLSL(
#version 330 core

in vec2 v_texcoord;
in vec4 v_color;

uniform bool u_texture_enable;
uniform sampler2D u_texture;
uniform int u_tex_func;
uniform bool u_tex_alpha;   // TCC: RGBA (true) or RGB (false)
uniform bool u_tex_double;
uniform vec3 u_tex_env;
uniform bool u_alpha_test_enable;
uniform float u_alpha_test_ref;
uniform int u_alpha_test_func;

out vec4 frag_color;

void main() {
    vec4 color = v_color;

    if (u_texture_enable) {
        vec4 tex = texture(u_texture, v_texcoord);

        // Texture function (GE tex func), PPSSPP FragmentShaderGenerator
        // semantics. With TCC = RGB the texture alpha is ignored and the
        // fragment keeps the primary color's alpha.
        vec4 p = v_color;
        if (u_tex_func == 0) {
            // MODULATE
            color = vec4(p.rgb * tex.rgb, u_tex_alpha ? p.a * tex.a : p.a);
        } else if (u_tex_func == 1) {
            // DECAL
            color = vec4(u_tex_alpha ? mix(p.rgb, tex.rgb, tex.a) : tex.rgb, p.a);
        } else if (u_tex_func == 2) {
            // BLEND (with the texture environment color)
            color = vec4(mix(p.rgb, u_tex_env, tex.rgb), u_tex_alpha ? p.a * tex.a : p.a);
        } else if (u_tex_func == 3) {
            // REPLACE
            color = vec4(tex.rgb, u_tex_alpha ? tex.a : p.a);
        } else {
            // ADD (5..7 behave as ADD too)
            color = vec4(p.rgb + tex.rgb, u_tex_alpha ? p.a * tex.a : p.a);
        }
        if (u_tex_double) {
            color.rgb *= 2.0;
        }
        color = clamp(color, 0.0, 1.0);
    }

    // Alpha test
    if (u_alpha_test_enable) {
        float ref = u_alpha_test_ref;
        float a = color.a;
        bool pass = true;

        if (u_alpha_test_func == 0) {
            pass = false;                  // NEVER
        } else if (u_alpha_test_func == 1) {
            pass = true;                   // ALWAYS
        } else if (u_alpha_test_func == 2) {
            pass = (a == ref);             // EQUAL
        } else if (u_alpha_test_func == 3) {
            pass = (a != ref);             // NOTEQUAL
        } else if (u_alpha_test_func == 4) {
            pass = (a < ref);              // LESS
        } else if (u_alpha_test_func == 5) {
            pass = (a <= ref);             // LEQUAL
        } else if (u_alpha_test_func == 6) {
            pass = (a > ref);              // GREATER
        } else if (u_alpha_test_func == 7) {
            pass = (a >= ref);             // GEQUAL
        }

        if (!pass) discard;
    }

    frag_color = color;
}
)GLSL";

// ---- Module state ----
static GLuint g_program = 0;
static ShaderUniforms g_uniforms{};

// ---- Helpers ----

static GLuint compile_shader(
    GLenum type, const char* src
) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log),
                           nullptr, log);
        std::fprintf(stderr,
            "[SHADER] Compile error (%s):\n%s\n",
            type == GL_VERTEX_SHADER ? "vert" : "frag",
            log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// ---- Public API ----

void ge_shader_init() {
    GLuint vert = compile_shader(
        GL_VERTEX_SHADER, k_vertex_shader_src);
    GLuint frag = compile_shader(
        GL_FRAGMENT_SHADER, k_fragment_shader_src);

    if (!vert || !frag) {
        std::fprintf(stderr,
            "[SHADER] Failed to compile shaders\n");
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return;
    }

    g_program = glCreateProgram();
    glAttachShader(g_program, vert);
    glAttachShader(g_program, frag);
    glLinkProgram(g_program);

    GLint success = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(g_program, sizeof(log),
                            nullptr, log);
        std::fprintf(stderr,
            "[SHADER] Link error:\n%s\n", log);
        glDeleteProgram(g_program);
        g_program = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    if (g_program) {
        // Cache uniform locations
        g_uniforms.u_texture_enable =
            glGetUniformLocation(
                g_program, "u_texture_enable");
        g_uniforms.u_texture =
            glGetUniformLocation(
                g_program, "u_texture");
        g_uniforms.u_tex_func =
            glGetUniformLocation(
                g_program, "u_tex_func");
        g_uniforms.u_tex_alpha = glGetUniformLocation(g_program, "u_tex_alpha");
        g_uniforms.u_tex_double = glGetUniformLocation(g_program, "u_tex_double");
        g_uniforms.u_tex_env = glGetUniformLocation(g_program, "u_tex_env");
        g_uniforms.u_alpha_test_enable =
            glGetUniformLocation(
                g_program, "u_alpha_test_enable");
        g_uniforms.u_alpha_test_ref =
            glGetUniformLocation(
                g_program, "u_alpha_test_ref");
        g_uniforms.u_alpha_test_func =
            glGetUniformLocation(
                g_program, "u_alpha_test_func");

        std::fprintf(stderr,
            "[SHADER] Uber-shader compiled and linked "
            "(program=%u)\n", g_program);
    }
}

void ge_shader_shutdown() {
    if (g_program) {
        glDeleteProgram(g_program);
        g_program = 0;
    }
}

void ge_shader_use() {
    if (g_program) {
        glUseProgram(g_program);
    }
}

void ge_shader_set_uniforms(const GeState& state) {
    if (!g_program) return;

    glUniform1i(g_uniforms.u_texture_enable,
                state.texture_enable ? 1 : 0);
    glUniform1i(g_uniforms.u_texture, 0);  // unit 0
    glUniform1i(g_uniforms.u_tex_func,
                static_cast<int>(state.tex_func));
    glUniform1i(g_uniforms.u_tex_alpha, state.tex_alpha ? 1 : 0);
    glUniform1i(g_uniforms.u_tex_double, state.tex_color_double ? 1 : 0);
    glUniform3f(g_uniforms.u_tex_env,
                (state.tex_env_color & 0xFF) / 255.0f,
                ((state.tex_env_color >> 8) & 0xFF) / 255.0f,
                ((state.tex_env_color >> 16) & 0xFF) / 255.0f);

    // Alpha test
    glUniform1i(g_uniforms.u_alpha_test_enable,
                state.alpha_test_enable ? 1 : 0);
    if (state.alpha_test_enable) {
        int func = state.alpha_test & 0x7;
        int ref_byte = (state.alpha_test >> 8) & 0xFF;
        float ref = ref_byte / 255.0f;
        glUniform1f(g_uniforms.u_alpha_test_ref, ref);
        glUniform1i(g_uniforms.u_alpha_test_func, func);
    }
}

GLuint ge_shader_get_program() {
    return g_program;
}
