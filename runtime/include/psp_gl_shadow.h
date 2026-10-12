#pragma once
// Shadow state of the render thread's GL calls (perf pass task 1b). Each setter
// stores the value and answers "would this call change GL state?": the GL front
// end (psp_gl_state.cpp) skips the real call when the answer is false.
//
// GL state persists across draws, so most of the ~1420 binds/frame in the
// hideout repeat the previous draw's values. Pure by design (no glad / SDL) so
// the policy is unit-tested without a GL context (tests/test_gl_shadow.cpp).
//
// Scope note: only the calls the GE draw path issues every PRIM are tracked.
// Code that touches GL outside that path (FBO creation, present blit, the OSK
// overlay, texture eviction) must call invalidate_all()/note_texture_deleted()
// so the next tracked call is re-emitted.
#include <cstdint>

class GlShadow {
public:
    enum class Cap { Blend, Depth, Cull, Stencil };

    // ---- Enables ----
    bool set_cap(Cap cap, bool on);

    // ---- Texture unit 0 (the only unit the GE path binds) ----
    bool bind_texture(uint32_t tex);
    uint32_t bound_texture() const { return tex_; }

    // Sampler parameters are per-texture GL state, so they can only be skipped
    // while the same texture stays bound and the values are unchanged. Applying
    // them for a different value emits.
    struct Sampler {
        uint32_t min = 0, mag = 0, wrap_s = 0, wrap_t = 0;
        bool operator==(const Sampler& o) const {
            return min == o.min && mag == o.mag
                && wrap_s == o.wrap_s && wrap_t == o.wrap_t;
        }
    };
    bool apply_sampler(const Sampler& s);

    // ---- Blend / depth ----
    bool set_blend_func(uint32_t src, uint32_t dst);
    bool set_blend_equation(uint32_t equation);
    bool set_blend_color(float r, float g, float b);
    bool set_depth_func(uint32_t func);

    // ---- Program and uniforms ----
    // Switching programs clears the uniform cache: the values were uploaded to
    // the previous program.
    bool use_program(uint32_t program);
    bool uniform1i(int loc, int v);
    bool uniform1f(int loc, float v);
    bool uniform3f(int loc, float x, float y, float z);

    // ---- External GL code ----
    // The context no longer matches the shadow (new context, FBO/present/OSK
    // paths, evictions): every tracked call is emitted once more.
    void invalidate_all();
    // glDeleteTextures may free the name currently shadowed; a later texture
    // that happens to reuse that name must not be mistaken for "already bound".
    void note_texture_deleted(uint32_t tex);

private:
    static const int kNumCaps = 4;
    static const int kMaxUniforms = 32;
    struct CapState {
        bool known = false;
        bool on = false;
    };
    struct Uniform {
        int loc = 0;
        int kind = 0;  // 1 = int, 2 = float, 3 = vec3
        int i = 0;
        float f[3] = {0.0f, 0.0f, 0.0f};
    };

    CapState caps_[kNumCaps];
    bool tex_known_ = false;
    uint32_t tex_ = 0;
    bool sampler_known_ = false;
    Sampler sampler_;
    bool blend_func_known_ = false;
    uint32_t blend_src_ = 0, blend_dst_ = 0;
    bool blend_eq_known_ = false;
    uint32_t blend_eq_ = 0;
    bool blend_color_known_ = false;
    float blend_color_[3] = {0.0f, 0.0f, 0.0f};
    bool depth_func_known_ = false;
    uint32_t depth_func_ = 0;
    bool program_known_ = false;
    uint32_t program_ = 0;
    Uniform uniforms_[kMaxUniforms];
    int uniform_count_ = 0;
};

inline bool GlShadow::set_cap(Cap cap, bool on) {
    CapState& c = caps_[static_cast<int>(cap)];
    if (c.known && c.on == on) return false;
    c.known = true;
    c.on = on;
    return true;
}

inline bool GlShadow::bind_texture(uint32_t tex) {
    if (tex_known_ && tex_ == tex) return false;
    tex_known_ = true;
    tex_ = tex;
    sampler_known_ = false;  // the applied sampler belongs to the old binding
    return true;
}

inline bool GlShadow::apply_sampler(const Sampler& s) {
    if (sampler_known_ && sampler_ == s) return false;
    sampler_ = s;
    sampler_known_ = true;
    return true;
}

inline bool GlShadow::set_blend_func(uint32_t src, uint32_t dst) {
    if (blend_func_known_ && blend_src_ == src && blend_dst_ == dst) return false;
    blend_func_known_ = true;
    blend_src_ = src;
    blend_dst_ = dst;
    return true;
}

inline bool GlShadow::set_blend_equation(uint32_t equation) {
    if (blend_eq_known_ && blend_eq_ == equation) return false;
    blend_eq_known_ = true;
    blend_eq_ = equation;
    return true;
}

inline bool GlShadow::set_blend_color(float r, float g, float b) {
    if (blend_color_known_ && blend_color_[0] == r && blend_color_[1] == g
        && blend_color_[2] == b) {
        return false;
    }
    blend_color_known_ = true;
    blend_color_[0] = r;
    blend_color_[1] = g;
    blend_color_[2] = b;
    return true;
}

inline bool GlShadow::set_depth_func(uint32_t func) {
    if (depth_func_known_ && depth_func_ == func) return false;
    depth_func_known_ = true;
    depth_func_ = func;
    return true;
}

inline bool GlShadow::use_program(uint32_t program) {
    if (program_known_ && program_ == program) return false;
    program_known_ = true;
    program_ = program;
    uniform_count_ = 0;  // uniform values belong to the previous program
    return true;
}

inline bool GlShadow::uniform1i(int loc, int v) {
    for (int i = 0; i < uniform_count_; i++) {
        if (uniforms_[i].loc != loc) continue;
        if (uniforms_[i].kind == 1 && uniforms_[i].i == v) return false;
        uniforms_[i].kind = 1;
        uniforms_[i].i = v;
        return true;
    }
    if (uniform_count_ >= kMaxUniforms) return true;  // uncached: always emit
    uniforms_[uniform_count_++] = Uniform{loc, 1, v, {0.0f, 0.0f, 0.0f}};
    return true;
}

inline bool GlShadow::uniform1f(int loc, float v) {
    for (int i = 0; i < uniform_count_; i++) {
        if (uniforms_[i].loc != loc) continue;
        if (uniforms_[i].kind == 2 && uniforms_[i].f[0] == v) return false;
        uniforms_[i].kind = 2;
        uniforms_[i].f[0] = v;
        return true;
    }
    if (uniform_count_ >= kMaxUniforms) return true;
    uniforms_[uniform_count_++] = Uniform{loc, 2, 0, {v, 0.0f, 0.0f}};
    return true;
}

inline bool GlShadow::uniform3f(int loc, float x, float y, float z) {
    for (int i = 0; i < uniform_count_; i++) {
        if (uniforms_[i].loc != loc) continue;
        if (uniforms_[i].kind == 3 && uniforms_[i].f[0] == x
            && uniforms_[i].f[1] == y && uniforms_[i].f[2] == z) {
            return false;
        }
        uniforms_[i].kind = 3;
        uniforms_[i].f[0] = x;
        uniforms_[i].f[1] = y;
        uniforms_[i].f[2] = z;
        return true;
    }
    if (uniform_count_ >= kMaxUniforms) return true;
    uniforms_[uniform_count_++] = Uniform{loc, 3, 0, {x, y, z}};
    return true;
}

inline void GlShadow::invalidate_all() {
    for (int i = 0; i < kNumCaps; i++) caps_[i] = CapState{};
    tex_known_ = false;
    sampler_known_ = false;
    blend_func_known_ = false;
    blend_eq_known_ = false;
    blend_color_known_ = false;
    depth_func_known_ = false;
    program_known_ = false;
    uniform_count_ = 0;
}

inline void GlShadow::note_texture_deleted(uint32_t tex) {
    if (tex_known_ && tex_ == tex) {
        tex_known_ = false;
        sampler_known_ = false;
    }
}
