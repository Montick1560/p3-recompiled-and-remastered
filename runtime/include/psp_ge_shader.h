#pragma once
#include "psp_ge.h"
#include <glad/glad.h>

/// Shader uniform locations cache.
struct ShaderUniforms {
    GLint u_texture_enable;
    GLint u_texture;
    GLint u_tex_func;
    GLint u_tex_alpha;
    GLint u_tex_double;
    GLint u_tex_env;
    GLint u_alpha_test_enable;
    GLint u_alpha_test_ref;
    GLint u_alpha_test_func;
    GLint u_src1_rgb;
    GLint u_src1_a;
    GLint u_fix_a;
    GLint u_fog_enable;
    GLint u_fog_color;
    GLint u_point_size;
    GLint u_z_sign;
};

/// Compile and link the uber-shader pair. Call after GL init. point_size: GE points in framebuffer pixels
/// (the render scale; one PSP pixel).
void ge_shader_init(float point_size);

/// Delete shader program.
void ge_shader_shutdown();

/// Bind the shader program.
void ge_shader_use();

/// Update all uniforms from current GE state.
void ge_shader_set_uniforms(const GeState& state);

void ge_shader_set_blend(int src1_rgb, int src1_a, const float fix_a[3]);

/// Get the shader program ID (for external uniform queries).
GLuint ge_shader_get_program();

/// Vertex z sign for this draw (ge_gl_depth_range: -1 for a reversed PSP depth range). Program in use.
void ge_shader_set_z_sign(float sign);
