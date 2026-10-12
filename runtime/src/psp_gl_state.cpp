#include "psp_gl_state.h"

// Shadow of the tracked GL state (see psp_gl_shadow.h for the policy). Static
// by design: there is exactly one GL context, owned by the render thread.
static GlShadow g_shadow;

static bool tracked_cap(GLenum cap, GlShadow::Cap* out) {
    switch (cap) {
    case GL_BLEND:        *out = GlShadow::Cap::Blend;   return true;
    case GL_DEPTH_TEST:   *out = GlShadow::Cap::Depth;   return true;
    case GL_CULL_FACE:    *out = GlShadow::Cap::Cull;    return true;
    case GL_STENCIL_TEST: *out = GlShadow::Cap::Stencil; return true;
    default:              return false;
    }
}

void gl_state_set_cap(GLenum cap, bool on) {
    GlShadow::Cap c;
    if (!tracked_cap(cap, &c)) {
        (on ? glEnable(cap) : glDisable(cap));  // untracked: plain pass-through
        return;
    }
    if (g_shadow.set_cap(c, on)) {
        (on ? glEnable(cap) : glDisable(cap));
    }
}

bool gl_state_bind_texture(GLuint tex) {
    if (!g_shadow.bind_texture(tex)) return false;
    glBindTexture(GL_TEXTURE_2D, tex);
    return true;
}

bool gl_state_apply_sampler(const GlShadow::Sampler& s) {
    if (!g_shadow.apply_sampler(s)) return false;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(s.min));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(s.mag));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(s.wrap_s));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(s.wrap_t));
    return true;
}

void gl_state_blend_func(GLenum src, GLenum dst) {
    if (g_shadow.set_blend_func(src, dst)) glBlendFunc(src, dst);
}

void gl_state_blend_equation(GLenum equation) {
    if (g_shadow.set_blend_equation(equation)) glBlendEquation(equation);
}

void gl_state_blend_color(GLfloat r, GLfloat g, GLfloat b) {
    if (g_shadow.set_blend_color(r, g, b)) glBlendColor(r, g, b, 1.0f);
}

void gl_state_depth_func(GLenum func) {
    if (g_shadow.set_depth_func(func)) glDepthFunc(func);
}

void gl_state_use_program(GLuint program) {
    if (g_shadow.use_program(program)) glUseProgram(program);
}

void gl_state_uniform1i(GLint loc, GLint v) {
    if (g_shadow.uniform1i(loc, v)) glUniform1i(loc, v);
}

void gl_state_uniform1f(GLint loc, GLfloat v) {
    if (g_shadow.uniform1f(loc, v)) glUniform1f(loc, v);
}

void gl_state_uniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) {
    if (g_shadow.uniform3f(loc, x, y, z)) glUniform3f(loc, x, y, z);
}

void gl_state_reset() {
    g_shadow.invalidate_all();
}

void gl_state_invalidate() {
    g_shadow.invalidate_all();
}

void gl_state_note_texture_deleted(GLuint tex) {
    g_shadow.note_texture_deleted(tex);
}
