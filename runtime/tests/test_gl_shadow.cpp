// GlShadow: pure policy of the render thread's GL state cache
// (used by runtime/src/psp_gl_state.cpp). Header-only, no GL / threads / I/O.
#include "psp_gl_shadow.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

// ---- Enables (blend / depth / cull / stencil) --------------------------------

static void caps_track_each_target() {
    GlShadow s;
    CHECK(s.set_cap(GlShadow::Cap::Blend, true), "first blend enable emits");
    CHECK(!s.set_cap(GlShadow::Cap::Blend, true), "same blend enable skipped");
    CHECK(s.set_cap(GlShadow::Cap::Depth, true), "depth is tracked separately");
    CHECK(s.set_cap(GlShadow::Cap::Blend, false), "blend disable emits");
    CHECK(!s.set_cap(GlShadow::Cap::Blend, false), "same blend disable skipped");
    CHECK(s.set_cap(GlShadow::Cap::Blend, true), "re-enable emits");
    CHECK(s.set_cap(GlShadow::Cap::Cull, true), "cull is tracked separately");
    CHECK(s.set_cap(GlShadow::Cap::Stencil, true), "stencil is tracked separately");
}

// ---- Texture binding ---------------------------------------------------------

static void bind_texture_skips_repeats() {
    GlShadow s;
    CHECK(s.bind_texture(7), "first bind emits");
    CHECK(!s.bind_texture(7), "same texture skipped");
    CHECK(s.bind_texture(9), "different texture emits");
    CHECK(!s.bind_texture(9), "still skipped");
    CHECK(s.bind_texture(0), "unbind emits");
    CHECK(!s.bind_texture(0), "same unbind skipped");
    CHECK(s.bound_texture() == 0, "bound_texture reports the shadowed binding");
}

// ---- Sampler -----------------------------------------------------------------

static void sampler_skips_only_when_unchanged() {
    GlShadow s;
    const GlShadow::Sampler a{1, 2, 3, 4};
    const GlShadow::Sampler b{1, 2, 3, 4};
    GlShadow::Sampler c{1, 2, 3, 5};
    CHECK(s.apply_sampler(a), "first sampler emits");
    CHECK(!s.apply_sampler(b), "identical sampler skipped for the same binding");
    CHECK(s.apply_sampler(c), "changed wrap emits");
    CHECK(!s.apply_sampler(c), "changed value now cached");
    // A different texture means the previously applied sampler belongs to the
    // old binding: the next application must emit even with equal values.
    s.bind_texture(11);
    CHECK(s.apply_sampler(c), "sampler re-emits after a texture change");
    CHECK(!s.apply_sampler(c), "and is cached again");
}

// ---- Blend, depth ------------------------------------------------------------

static void blend_calls_skip_repeats() {
    GlShadow s;
    CHECK(s.set_blend_func(0x0302, 0x0303), "first blend func emits");
    CHECK(!s.set_blend_func(0x0302, 0x0303), "same blend func skipped");
    CHECK(s.set_blend_func(0x0302, 0x0302), "dst change emits");
    CHECK(s.set_blend_equation(0x8006), "first equation emits");
    CHECK(!s.set_blend_equation(0x8006), "same equation skipped");
    CHECK(s.set_blend_equation(0x800A), "equation change emits");
    CHECK(s.set_blend_color(0.5f, 0.25f, 1.0f), "first blend color emits");
    CHECK(!s.set_blend_color(0.5f, 0.25f, 1.0f), "same blend color skipped");
    CHECK(s.set_blend_color(0.5f, 0.25f, 0.75f), "blend color change emits");
}

static void depth_func_skips_repeats() {
    GlShadow s;
    CHECK(s.set_depth_func(0x0201), "first depth func emits");
    CHECK(!s.set_depth_func(0x0201), "same depth func skipped");
    CHECK(s.set_depth_func(0x0203), "depth func change emits");
}

// ---- Program and uniforms ----------------------------------------------------

static void program_change_invalidates_uniforms() {
    GlShadow s;
    CHECK(s.use_program(3), "first use emits");
    CHECK(!s.use_program(3), "same program skipped");
    CHECK(s.uniform1i(0, 1), "first uniform emits");
    CHECK(!s.uniform1i(0, 1), "same uniform skipped");
    CHECK(s.use_program(4), "program change emits");
    CHECK(s.uniform1i(0, 1), "uniform re-emits under the new program");
    CHECK(!s.uniform1i(0, 1), "and is cached under the new program");
}

static void uniforms_track_each_location_and_kind() {
    GlShadow s;
    s.use_program(3);
    CHECK(s.uniform1i(0, 1), "int uniform first");
    CHECK(s.uniform1i(1, 1), "other location tracked separately");
    CHECK(!s.uniform1i(0, 1), "int still cached");
    CHECK(s.uniform1i(0, 2), "int change emits");
    CHECK(s.uniform1f(4, 0.25f), "float uniform first");
    CHECK(!s.uniform1f(4, 0.25f), "same float skipped");
    CHECK(s.uniform1f(4, 0.5f), "float change emits");
    CHECK(s.uniform3f(5, 1.0f, 2.0f, 3.0f), "vec3 first");
    CHECK(!s.uniform3f(5, 1.0f, 2.0f, 3.0f), "same vec3 skipped");
    CHECK(s.uniform3f(5, 1.0f, 2.0f, 3.5f), "vec3 component change emits");
}

// ---- Invalidation ------------------------------------------------------------

static void invalidate_all_forces_one_re_emission() {
    GlShadow s;
    s.set_cap(GlShadow::Cap::Blend, true);
    s.bind_texture(7);
    s.apply_sampler(GlShadow::Sampler{1, 1, 1, 1});
    s.set_blend_func(1, 2);
    s.set_blend_equation(3);
    s.set_blend_color(0.1f, 0.2f, 0.3f);
    s.set_depth_func(4);
    s.use_program(5);
    s.uniform1i(0, 1);
    s.invalidate_all();
    CHECK(s.set_cap(GlShadow::Cap::Blend, true), "cap re-emits after invalidation");
    CHECK(!s.set_cap(GlShadow::Cap::Blend, true), "and caches again");
    CHECK(s.bind_texture(7), "texture re-emits after invalidation");
    CHECK(s.apply_sampler(GlShadow::Sampler{1, 1, 1, 1}), "sampler re-emits");
    CHECK(s.set_blend_func(1, 2), "blend func re-emits");
    CHECK(s.set_blend_equation(3), "equation re-emits");
    CHECK(s.set_blend_color(0.1f, 0.2f, 0.3f), "blend color re-emits");
    CHECK(s.set_depth_func(4), "depth func re-emits");
    CHECK(s.use_program(5), "program re-emits");
    CHECK(s.uniform1i(0, 1), "uniform re-emits");
}

static void deleted_texture_invalidates_binding() {
    GlShadow s;
    s.bind_texture(7);
    s.apply_sampler(GlShadow::Sampler{1, 1, 1, 1});
    s.note_texture_deleted(9);  // unrelated name: nothing changes
    CHECK(!s.bind_texture(7), "unrelated delete keeps the cached binding");
    s.note_texture_deleted(7);
    CHECK(s.bind_texture(7), "deleted bound texture forces a re-bind");
    CHECK(s.apply_sampler(GlShadow::Sampler{1, 1, 1, 1}),
          "sampler re-emits for the re-bound texture");
}

int main() {
    caps_track_each_target();
    bind_texture_skips_repeats();
    sampler_skips_only_when_unchanged();
    blend_calls_skip_repeats();
    depth_func_skips_repeats();
    program_change_invalidates_uniforms();
    uniforms_track_each_location_and_kind();
    invalidate_all_forces_one_re_emission();
    deleted_texture_invalidates_binding();
    if (failures == 0) std::printf("test_gl_shadow: all passed\n");
    return failures == 0 ? 0 : 1;
}
