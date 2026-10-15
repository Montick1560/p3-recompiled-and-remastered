#pragma once
// Real-GL front end for GlShadow (psp_gl_shadow.h, perf pass task 1b): each
// tracked call is issued only when the shadow reports the value changed.
// Render (GL) thread only. GL code outside this path (FBO creation, present
// blit, OSK overlay, texture eviction) must invalidate the shadow, as the
// psp_ge_* call sites do.
#include <glad/glad.h>

#include "psp_gl_shadow.h"

// Tracked enables: GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_STENCIL_TEST.
// Any other cap passes through untracked (e.g. GL_SCISSOR_TEST).
void gl_state_set_cap(GLenum cap, bool on);

// GL_TEXTURE_2D on unit 0 (the only unit the GE path uses). Returns true when
// the binding was emitted (the caller's "did this bind do driver work?").
bool gl_state_bind_texture(GLuint tex);
// Returns true when the sampler parameters were emitted.
bool gl_state_apply_sampler(const GlShadow::Sampler& s);

void gl_state_blend_func(GLenum src, GLenum dst);
void gl_state_blend_equation(GLenum equation);
void gl_state_blend_color(GLfloat r, GLfloat g, GLfloat b);
void gl_state_depth_func(GLenum func);
void gl_state_stencil_func(GLenum func, GLint ref, GLuint mask);
void gl_state_stencil_op(GLenum sfail, GLenum zfail, GLenum zpass);
// glStencilMask: also gates glClear(GL_STENCIL_BUFFER_BIT).
void gl_state_stencil_mask(GLuint mask);

void gl_state_use_program(GLuint program);
void gl_state_uniform1i(GLint loc, GLint v);
void gl_state_uniform1f(GLint loc, GLfloat v);
void gl_state_uniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z);

// ---- Shadow invalidation ----
// New / recreated GL context: nothing tracked is guaranteed to match.
void gl_state_reset();
// External GL code changed tracked state (FBO/present/OSK paths).
void gl_state_invalidate();
// glDeleteTextures freed a name the shadow may be pointing at.
void gl_state_note_texture_deleted(GLuint tex);
