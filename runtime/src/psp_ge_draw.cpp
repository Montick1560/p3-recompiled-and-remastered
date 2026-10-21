#include "psp_osk_overlay.h"
#include "psp_exit.h"
#include "psp_ge_draw.h"
#include "psp_ge_texpatch.h"
#include "psp_perf.h"
#include "psp_ge_blend.h"
#include "psp_ge_mask.h"
#include "psp_ge_stencil.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include "psp_ge_vertex.h"
#include "psp_ge_texture.h"
#include "psp_ge_transfer.h"
#include "psp_ge_rtt.h"
#include "psp_ge_shader.h"
#include "psp_gl_state.h"
#include "psp_event_loop.h"

#include <SDL.h>
#include <glad/glad.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "psp_present.h"
#include "xxhash.h"

// ---- Generic FBO pool (issue #59 — honor fb_addr / double buffer) ----
// One GL FBO per distinct guest VRAM framebuffer address. The GE binds the
// FBO for the current FRAMEBUFPTR; present blits the FBO matching the
// displayed (sceDisplaySetFrameBuf) address. Keying on the guest's own
// addresses keeps this generic: a single-buffered title keys everything to
// one slot and degenerates to the old single-surface behavior.
static constexpr int GE_MAX_TARGETS = 4;

struct GeTarget {
    uint32_t key = 0;          // fb_addr normalized to eDRAM offset (0 = empty)
    GLuint fbo = 0;
    GLuint color_tex = 0;
    GLuint depth_rb = 0;
    uint64_t last_use = 0;     // LRU stamp (monotonic counter)
    uint32_t stride = 0;        // FRAMEBUFWIDTH & 0x7FC of the last PRIM drawn into it
    uint32_t fb_fmt = 0;        // FRAMEBUFPIXFORMAT of the last PRIM drawn into it
    bool gl_written = false;    // a PRIM was drawn: GL holds pixels guest VRAM does not
    bool cpu_newer = false;     // guest VRAM changed after GL drew: RAM is the truth
    uint64_t guest_hash = 0;    // XXH3 of the guest window at the last check
};

static GeTarget g_targets[GE_MAX_TARGETS];
static int g_current_target = -1;   // slot the GE is rendering into
static int g_front_target = -1;     // slot last presented / most recent render
static uint64_t g_target_clock = 0; // monotonic LRU counter

// Normalize any guest framebuffer address to its eDRAM 2 MB offset key.
// Strips the cached/uncached/eDRAM base via the canonical 0x07FFFFFF PSP
// mask, then takes the 16-byte-aligned eDRAM offset (PPSSPP model). Used by
// both ge_draw_select_target (FRAMEBUFPTR) and ge_present_frame
// (sceDisplaySetFrameBuf) so the two address spaces reconcile.
static inline uint32_t ge_fb_key(uint32_t addr) {
    return (addr & 0x07FFFFFFu) & 0x001FFFF0u;
}

// ---- Liveness present (no-flip safety net) ----
static bool g_present_dirty = false;
static std::chrono::steady_clock::time_point g_last_present;
static int g_present_stale_ms = 100;   // PSPRECOMP_PRESENT_STALE_MS

// ---- Module state ----
static GLuint g_vao = 0;
static GLuint g_vbo = 0;
static uint32_t g_frame_counter = 0;
// PRIM index inside the current display list ([DL] i=, debug socket X).
static int g_list_prim_index = 0;
static bool g_has_drawn_prims = false;
static bool g_screenshot_enabled = false;
static bool g_screenshot_done = false;
static char g_screenshot_path[256] = {};
static std::atomic<bool> g_draw_ready{false};

// ---- PRIM counters (file scope so ge_draw_get_stats can read them) ----
// Written only by the render thread inside ge_draw_prim; read racily by
// the debug socket's I command (a few prims stale is fine).
static int g_prim_type_count[8] = {0};   // PRIM types 0-6 + slot 7 safety
static int g_prim_type_count_clear[8] = {0};
static int g_prim_type_zero_count = 0;   // count <= 0 (degenerate)
static int g_prim_total_observed = 0;

// ---- On-demand screenshot request (issue #35 debug socket S command) ----
// Requester (any thread) posts a path + waits on the condvar; the render
// thread polls ge_draw_service_screenshot_request() each event-loop pass
// and performs the GL readback there (GL never leaves the main thread).
static std::mutex g_ss_req_mutex;
static std::condition_variable g_ss_req_cv;
static char g_ss_req_path[512] = {};
enum SsReqState { SS_IDLE = 0, SS_PENDING = 1, SS_DONE = 2, SS_FAILED = 3 };
static int g_ss_req_state = SS_IDLE;

// ---- Packed vertex for VBO upload ----
// Layout: 4 floats (clip pos) + 2 floats (uv) + 4 bytes (color) + 1 float (fog factor)
// Total: 16 + 8 + 4 + 4 = 32 bytes
struct PackedVertex {
    float pos[4];   // Clip-space (x,y,z,w); GL does the perspective divide
    float uv[2];
    uint8_t color[4];
    float fog;      // GE fog factor, unclamped (1 = none); the fragment shader clamps
};
static_assert(sizeof(PackedVertex) == 32,
              "PackedVertex must be 32 bytes");
static_assert(offsetof(PackedVertex, uv) == 16 && offsetof(PackedVertex, color) == 24 &&
                  offsetof(PackedVertex, fog) == 28,
              "PackedVertex layout must match the shader attributes");

// PSP render-target dimensions. The runtime renders into an FBO of PSP size
// x render scale; window upscaling happens later at present (blit). The PSP
// viewport/scissor are scaled into FBO pixels by ge_compute_viewport_depth /
// ge_compute_scissor.
static constexpr int PSP_FB_WIDTH = 480;
static constexpr int PSP_FB_HEIGHT = 272;

// Internal render scale (PSPRECOMP_RENDER_SCALE, 1..8; ge_draw_init). Every
// FBO-pixel quantity (target size, viewport, scissor, blit, readback) goes
// through it; through-mode vertices are already in NDC.
static int g_render_scale = 1;
static int fb_w() { return PSP_FB_WIDTH * g_render_scale; }
static int fb_h() { return PSP_FB_HEIGHT * g_render_scale; }

// ---- Screenshot (TGA format -- no external dependency) ----

static bool write_tga(
    const char* path,
    const uint8_t* pixels,
    int width,
    int height
) {
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr,
            "[DRAW] Cannot open %s for screenshot\n",
            path);
        return false;
    }

    // TGA header (18 bytes) -- uncompressed RGBA
    uint8_t header[18] = {};
    header[2]  = 2;  // Uncompressed true-color
    header[12] = width & 0xFF;
    header[13] = (width >> 8) & 0xFF;
    header[14] = height & 0xFF;
    header[15] = (height >> 8) & 0xFF;
    header[16] = 32;  // bits per pixel
    header[17] = 0x28; // origin upper-left + 8 alpha bits

    std::fwrite(header, 1, 18, f);

    // Write pixels (BGRA for TGA)
    // Input is RGBA, need to swap R and B
    std::vector<uint8_t> bgra(width * height * 4);
    for (int i = 0; i < width * height; i++) {
        bgra[i * 4 + 0] = pixels[i * 4 + 2];  // B
        bgra[i * 4 + 1] = pixels[i * 4 + 1];  // G
        bgra[i * 4 + 2] = pixels[i * 4 + 0];  // R
        bgra[i * 4 + 3] = pixels[i * 4 + 3];  // A
    }
    std::fwrite(bgra.data(), 1, bgra.size(), f);
    std::fclose(f);

    std::fprintf(stderr,
        "[DRAW] Screenshot saved: %s (%dx%d)\n",
        path, width, height);
    return true;
}

/// Read a render target back as top-down RGBA rows and write it as TGA at
/// the FBO size. Render (GL) thread only.
static bool write_fbo_tga(GLuint fbo, const char* path) {
    const int w = fb_w(), h = fb_h();
    const size_t row = static_cast<size_t>(w) * 4;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    std::vector<uint8_t> pixels(row * h);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::vector<uint8_t> flipped(pixels.size());
    for (int y = 0; y < h; y++) {  // GL origin is bottom-left
        std::memcpy(flipped.data() + y * row, pixels.data() + (h - 1 - y) * row, row);
    }
    return write_tga(path, flipped.data(), w, h);
}

// Resolve the FBO id of the most-recently-presented (front) target, or the
// current render target as a fallback. Returns 0 if no target exists yet.
static GLuint front_fbo_id() {
    if (g_front_target >= 0) return g_targets[g_front_target].fbo;
    if (g_current_target >= 0) return g_targets[g_current_target].fbo;
    return 0;
}

/// Read the FBO back, flip, and write a TGA. Render (GL) thread only.
static bool capture_fbo_to_tga(const char* path) {
    return write_fbo_tga(front_fbo_id(), path);
}

// ---- GL state mapping ----

// Map the PSP depth-compare to a GL depth func. glDepthRange(near, far) makes
// the GL window depth equal the PSP z / 65535 even for a reversed range
// (ge_gl_window_depth), so the compare maps 1:1 (PPSSPP
// compareOps[gstate.getDepthTestFunction()]); flipping it hid Patapon 3's
// depth-tested chests.
static GLenum map_depth_func(int func) {
    GLenum gl;
    switch (func) {
    case GE_COMP_NEVER:    return GL_NEVER;
    case GE_COMP_ALWAYS:   return GL_ALWAYS;
    case GE_COMP_EQUAL:    return GL_EQUAL;
    case GE_COMP_NOTEQUAL: return GL_NOTEQUAL;
    case GE_COMP_LESS:     gl = GL_LESS;    break;
    case GE_COMP_LEQUAL:   gl = GL_LEQUAL;  break;
    case GE_COMP_GREATER:  gl = GL_GREATER; break;
    case GE_COMP_GEQUAL:   gl = GL_GEQUAL;  break;
    default:               gl = GL_LEQUAL;  break;
    }
    return gl;
}

// GE stencil op -> GL (PPSSPP StateMappingGLES.cpp stencilOps; 6/7 reserved).
static GLenum map_stencil_op(int op) {
    switch (op) {
    case GE_STENCILOP_ZERO:    return GL_ZERO;
    case GE_STENCILOP_REPLACE: return GL_REPLACE;
    case GE_STENCILOP_INVERT:  return GL_INVERT;
    case GE_STENCILOP_INCR:    return GL_INCR;
    case GE_STENCILOP_DECR:    return GL_DECR;
    default:                   return GL_KEEP;
    }
}

static GeStencilInput stencil_input(const GeState& s) {
    GeStencilInput in;
    in.test_enable = s.stencil_test_enable;
    in.stencil_test = s.stencil_test;
    in.stencil_op = s.stencil_op;
    in.mask_rgb = s.mask_rgb;
    in.mask_alpha = s.mask_alpha;
    in.depth_test = s.depth_test_enable;
    in.depth_write = !s.depth_write_disable;
    in.fb_format = s.framebuf_format & 3;
    return in;
}

static GLenum map_prim_type(int prim_type) {
    switch (prim_type) {
    case GE_PRIM_POINTS:         return GL_POINTS;
    case GE_PRIM_LINES:          return GL_LINES;
    case GE_PRIM_LINE_STRIP:     return GL_LINE_STRIP;
    case GE_PRIM_TRIANGLES:      return GL_TRIANGLES;
    case GE_PRIM_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case GE_PRIM_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
    case GE_PRIM_RECTANGLES:     return GL_TRIANGLES;
    default:                     return GL_TRIANGLES;
    }
}

// Compute the PSP viewport scale/offset + depth-range -> GL viewport/depth
// range for the live state. Generic for all games (issue #23). The math lives
// in the pure ge_compute_viewport_depth (tests/test_ge_viewport.cpp) so it is
// unit-testable without a GL context; callers issue the GL calls themselves.
static GeViewportDepth compute_state_viewport_depth(const GeState& state) {
    return ge_compute_viewport_depth(
        state.viewport_x_scale, state.viewport_y_scale,
        state.viewport_x_center, state.viewport_y_center,
        state.viewport_z_scale, state.viewport_z_center,
        state.offset_x, state.offset_y, PSP_FB_HEIGHT, g_render_scale);
}

// ---- FBO pool helpers ----

// Create one render target (FBO + RGBA8 color texture + Depth24/Stencil8 renderbuffer,
// PSP size x render scale, NEAREST) for the given normalized key. Clears it to black. The
// pool owns the GL objects; ge_draw_shutdown deletes them.
static GeTarget make_target(uint32_t key) {
    GeTarget t;
    t.key = key;
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);

    glGenTextures(1, &t.color_tex);
    gl_state_bind_texture(t.color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb_w(), fb_h(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, t.color_tex, 0);

    glGenRenderbuffers(1, &t.depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, t.depth_rb);
    // The PSP keeps stencil in the color alpha; the host needs a real buffer
    // for the stencil test (psp_ge_stencil.h).
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fb_w(), fb_h());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, t.depth_rb);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[DRAW] FBO incomplete: 0x%04X (key=0x%06X)\n",
                     status, key);
    }
    glDisable(GL_SCISSOR_TEST);  // a new buffer starts fully cleared
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearStencil(0);
    gl_state_stencil_mask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    t.last_use = ++g_target_clock;
    return t;
}

// Find the pool slot for key, or allocate one (LRU-evicting if full).
static int acquire_target(uint32_t key) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo != 0 && g_targets[i].key == key) {
            g_targets[i].last_use = ++g_target_clock;
            return i;
        }
    }
    int slot = -1;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo == 0) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;  // LRU evict
        for (int i = 1; i < GE_MAX_TARGETS; ++i) {
            if (g_targets[i].last_use < g_targets[slot].last_use) slot = i;
        }
        std::fprintf(stderr,
            "[DRAW] FBO pool overflow: evicting key=0x%06X for key=0x%06X\n",
            g_targets[slot].key, key);
        glDeleteFramebuffers(1, &g_targets[slot].fbo);
        gl_state_note_texture_deleted(g_targets[slot].color_tex);
        glDeleteTextures(1, &g_targets[slot].color_tex);
        glDeleteRenderbuffers(1, &g_targets[slot].depth_rb);
    }
    g_targets[slot] = make_target(key);
    std::fprintf(stderr, "[DRAW] FBO target created key=0x%06X slot=%d\n",
                 key, slot);
    return slot;
}

// Bind the FBO for the current FRAMEBUFPTR. key==0 (FRAMEBUFPTR not yet seen)
// keeps the current/last target so early lists still land somewhere.
void ge_draw_select_target(uint32_t fb_addr_raw) {
    // Offset 0 is a real buffer (games double-buffer 0x04000000 and
    // 0x04044000); before any FRAMEBUFPTR the GE default is offset 0 too.
    uint32_t key = ge_fb_key(fb_addr_raw);
    int slot = acquire_target(key);
    g_current_target = slot;
    if (g_front_target < 0) g_front_target = slot;
    glBindFramebuffer(GL_FRAMEBUFFER, g_targets[slot].fbo);
}

void ge_draw_mark_dirty() {
    g_present_dirty = true;
}

// ---- Public API ----

void ge_draw_init() {
    g_render_scale = present_render_scale(std::getenv("PSPRECOMP_RENDER_SCALE"));
    ge_texpatch_set_scale(g_render_scale);
    std::fprintf(stderr, "[DRAW] render scale %d (%dx%d)\n",
                 g_render_scale, fb_w(), fb_h());
    if (g_render_scale > 1) {
        // GE points and lines are one PSP pixel wide: keep that apparent size
        // (no draw path changes these GL states afterwards). Wide lines are
        // allowed in this non-forward-compatible core context; a driver that
        // rejects them leaves 1 px lines.
        GLfloat range[2] = {1.0f, 1.0f};
        glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range);
        while (glGetError() != GL_NO_ERROR) {}
        glLineWidth(static_cast<GLfloat>(g_render_scale));
        glPointSize(static_cast<GLfloat>(g_render_scale));
        const GLenum err = glGetError();
        std::fprintf(stderr, "[DRAW] line/point width %d (driver line range %.0f..%.0f)%s\n",
                     g_render_scale, range[0], range[1],
                     err != GL_NO_ERROR ? " REJECTED, lines stay 1 px" : "");
    }

    // Check screenshot env var
    const char* ss_env =
        std::getenv("PSPRECOMP_SCREENSHOT");
    if (ss_env && ss_env[0]) {
        g_screenshot_enabled = true;
        std::strncpy(g_screenshot_path, ss_env,
                     sizeof(g_screenshot_path) - 1);
    }

    // Liveness-present budget (no-flip safety net). Env-overridable.
    const char* stale_env = std::getenv("PSPRECOMP_PRESENT_STALE_MS");
    if (stale_env && stale_env[0]) {
        int v = std::atoi(stale_env);
        if (v > 0) g_present_stale_ms = v;
    }
    g_last_present = std::chrono::steady_clock::now();

    // FBO targets are created lazily per guest framebuffer address by
    // ge_draw_select_target. No target is pre-created here.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Create VAO + VBO
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    // Vertex attributes for PackedVertex layout
    // location 0: position (4 floats)
    glVertexAttribPointer(
        0, 4, GL_FLOAT, GL_FALSE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(offsetof(PackedVertex, pos)));
    glEnableVertexAttribArray(0);

    // location 1: texcoord (2 floats)
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(offsetof(PackedVertex, uv)));
    glEnableVertexAttribArray(1);

    // location 2: color (4 bytes normalized)
    glVertexAttribPointer(
        2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(offsetof(PackedVertex, color)));
    glEnableVertexAttribArray(2);

    // location 3: fog factor (1 float)
    glVertexAttribPointer(
        3, 1, GL_FLOAT, GL_FALSE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(offsetof(PackedVertex, fog)));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);

    // Init shader
    gl_state_reset();
    ge_shader_init();

    g_frame_counter = 0;
    g_draw_ready.store(true, std::memory_order_release);

    std::fprintf(stderr,
        "[DRAW] Draw infrastructure initialized\n");
}

void ge_draw_shutdown() {
    g_draw_ready.store(false, std::memory_order_release);
    // Auto-capture final frame if any PRIMs were rendered and no screenshot taken yet
    if (g_has_drawn_prims && !g_screenshot_done) {
        const char* out_path = (g_screenshot_path[0] != '\0')
            ? g_screenshot_path : "frame.tga";
        write_fbo_tga(front_fbo_id(), out_path);
        g_screenshot_done = true;
        std::fprintf(stderr, "[DRAW] Auto-capture: %s\n", out_path);
    }

    ge_shader_shutdown();

    if (g_vbo) {
        glDeleteBuffers(1, &g_vbo);
        g_vbo = 0;
    }
    if (g_vao) {
        glDeleteVertexArrays(1, &g_vao);
        g_vao = 0;
    }
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        GeTarget& t = g_targets[i];
        if (t.depth_rb) glDeleteRenderbuffers(1, &t.depth_rb);
        if (t.color_tex) {
            gl_state_note_texture_deleted(t.color_tex);
            glDeleteTextures(1, &t.color_tex);
        }
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
        t = GeTarget{};
    }
    g_current_target = -1;
    g_front_target = -1;
    gl_state_reset();

    std::fprintf(stderr,
        "[DRAW] Draw infrastructure shutdown\n");
}

void ge_draw_begin_list() {
    // Bind the FBO for the list's current render-target address (FRAMEBUFPTR).
    // Geometry and clears in this list now target the per-address FBO, so a
    // clear to address X cannot wipe geometry on address Y (issue #59).
    ge_draw_select_target(ge_get_state().framebuf_ptr);
    // Full-buffer default; the precise PSP viewport (scale/offset + depth
    // range) is applied per-PRIM in ge_draw_prim from the live registers,
    // which may be set after begin_list (issue #23).
    glViewport(0, 0, fb_w(), fb_h());
    g_frame_counter++;
    g_list_prim_index = 0;
    ge_texpatch_begin_list();  // patched textures use one variant for the whole list
}

// Debug socket `D <lists>`: remaining display lists to log ([DL] lines).
static std::atomic<int> g_drawlog_lists{0};

// Debug socket `X <lo> <hi>`: PRIM index range (per list) not drawn.
static std::atomic<int> g_skip_lo{1}, g_skip_hi{0};

void ge_draw_request_skip(int lo, int hi) {
    g_skip_lo.store(lo, std::memory_order_relaxed);
    g_skip_hi.store(hi, std::memory_order_relaxed);
}

void ge_draw_request_log(int lists) {
    g_drawlog_lists.store(lists, std::memory_order_relaxed);
}

bool ge_draw_log_active() {
    return g_drawlog_lists.load(std::memory_order_relaxed) > 0;
}

void ge_draw_end_list() {
    // Mark the front buffer dirty so the liveness net can publish content
    // even for a title that draws but never calls sceDisplaySetFrameBuf.
    ge_draw_mark_dirty();
    if (g_drawlog_lists.load(std::memory_order_relaxed) > 0) {
        std::fprintf(stderr, "[DL] ---- list end (fb=0x%06X)\n",
                     ge_get_state().framebuf_ptr);
        g_drawlog_lists.fetch_sub(1, std::memory_order_relaxed);
    }
}

static void drawlog_prim(const GeState& s, int index, int prim_type, int count) {
    std::fprintf(stderr,
        "[DL] i=%d prim=%d n=%d vt=0x%06X va=0x%08X base=0x%06X off=0x%08X fb=0x%06X clr=%d thr=%d "
        "tex=%d ta=0x%08X tf=%u tsz=0x%04X bw=%u clut=0x%08X cf=0x%06X "
        "tfn=0x%X blend=%d bm=0x%03X fa=0x%06X fb2=0x%06X at=%d atv=0x%06X "
        "zt=%d zf=%u zw=%d st=%d stt=0x%06X sop=0x%06X mrgb=0x%06X ma=0x%02X fmt=%u cf=%u "
        "lit=%d mat=0x%06X/%02X amb=0x%06X sc=0x%06X/0x%06X "
        "fog=%d f1=%g f2=%g fc=0x%06X cull=%d/%u\n",
        index, prim_type, count, s.vertex_type, s.vertex_addr, s.base_addr, s.offset_addr,
        s.framebuf_ptr,
        s.clear_mode ? 1 : 0, ge_vtype_through(s.vertex_type) ? 1 : 0,
        s.texture_enable ? 1 : 0, s.tex_addr[0], s.tex_format, s.tex_size[0],
        s.tex_bufw[0], s.clut_addr, s.clut_format, s.tex_func_raw,
        s.alpha_blend_enable ? 1 : 0, s.blend_mode, s.blend_fix_a,
        s.blend_fix_b, s.alpha_test_enable ? 1 : 0, s.alpha_test,
        s.depth_test_enable ? 1 : 0, s.depth_func,
        s.depth_write_disable ? 0 : 1, s.stencil_test_enable ? 1 : 0,
        s.stencil_test & 0xFFFFFF, s.stencil_op & 0xFFFFFF, s.mask_rgb & 0xFFFFFF,
        s.mask_alpha & 0xFF, s.framebuf_format, s.clear_flags,
        s.lighting_enable ? 1 : 0, s.material_ambient, s.material_alpha,
        s.ambient_color, s.scissor1, s.scissor2,
        s.fog_enable ? 1 : 0, s.fog1, s.fog2, s.fog_color & 0xFFFFFF,
        s.cull_enable ? 1 : 0, s.cull_face);
    if (!ge_vtype_through(s.vertex_type)) {
        const float* w = s.world_matrix;
        const float* v = s.view_matrix;
        const float* p = s.proj_matrix;
        std::fprintf(stderr,
            "[DL]   world=[%g %g %g|%g %g %g|%g %g %g|%g %g %g] "
            "view=[%g %g %g|%g %g %g|%g %g %g|%g %g %g] "
            "proj=[%g %g %g %g|%g %g %g %g|%g %g %g %g|%g %g %g %g] "
            "vp=%g,%g,%g/%g,%g,%g z=%u..%u\n",
            w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
            p[11], p[12], p[13], p[14], p[15],
            s.viewport_x_scale, s.viewport_y_scale, s.viewport_z_scale,
            s.viewport_x_center, s.viewport_y_center, s.viewport_z_center,
            s.min_z, s.max_z);
        if (s.lighting_enable) {
            std::fprintf(stderr,
                "[DL]   light amb=0x%06X/%02X emis=0x%06X diff=0x%06X spec=0x%06X^%g mupd=%u "
                "lmode=%u rev=%d\n",
                s.ambient_color, s.ambient_alpha, s.material_emissive, s.material_diffuse,
                s.material_specular, s.material_specular_coef, s.material_update,
                s.light_mode, s.reverse_normal ? 1 : 0);
            for (int li = 0; li < 4; li++) {
                if (!s.light_enable[li]) continue;
                std::fprintf(stderr,
                    "[DL]   L%d type=0x%03X pos=%g,%g,%g dir=%g,%g,%g att=%g,%g,%g "
                    "cut=%g exp=%g ac=0x%06X dc=0x%06X sc=0x%06X\n",
                    li, s.light_type[li], s.light_pos[li][0], s.light_pos[li][1],
                    s.light_pos[li][2], s.light_dir[li][0], s.light_dir[li][1],
                    s.light_dir[li][2], s.light_att[li][0], s.light_att[li][1],
                    s.light_att[li][2], s.light_spot_cutoff[li], s.light_spot_exp[li],
                    s.light_ambient[li], s.light_diffuse[li], s.light_specular[li]);
            }
        }
    }
}

// ---- RTT detector (logs only) ----
// Which draws, transfers and CPU writes touch a GL-written target. GL never writes guest
// VRAM, so a change of a target's guest bytes is the CPU (or a transfer) writing it.
static int rtt_targets(GeRttTarget* out) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        const GeTarget& t = g_targets[i];
        out[i] = {t.fbo != 0 && t.gl_written && !t.cpu_newer, t.key, t.stride, t.fb_fmt};
    }
    return GE_MAX_TARGETS;
}

static uint32_t rtt_window_bytes(const GeTarget& t) {
    return t.stride * (t.fb_fmt == 3 ? 4u : 2u) * PSP_FB_HEIGHT;
}

// Once per list: hash each GL-written target's guest window.
static void rtt_check_cpu_writes(const uint8_t* rdram) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        GeTarget& t = g_targets[i];
        if (t.fbo == 0 || !t.gl_written || t.stride == 0) continue;
        const uint32_t base = 0x04000000u + t.key, bytes = rtt_window_bytes(t);
        if (uint64_t{base} + bytes > 0x08000000u) continue;   // 128 MB guest image
        const uint64_t h = XXH3_64bits(rdram + base, bytes);
        if (t.guest_hash != 0 && h != t.guest_hash) {
            t.cpu_newer = true;
            static uint32_t count = 0;
            if (++count <= 16 || count % 300 == 0)
                std::fprintf(stderr, "[RTT] cpu-write key=0x%06X count=%u\n", t.key, count);
        }
        t.guest_hash = h;
    }
}

// One line per distinct (target, texture address, format) the game samples.
static void rtt_note_sample(const GeState& s) {
    const uint32_t addr = s.tex_addr[0] | (((s.tex_bufw[0] >> 16) & 0xFFu) << 24);
    const int w = 1 << (s.tex_size[0] & 0xF), h = 1 << ((s.tex_size[0] >> 8) & 0xF);
    const uint32_t bufw = s.tex_bufw[0] & 0x7FFu;
    const uint32_t bytes = ge_rtt_tex_bytes(static_cast<int>(s.tex_format), bufw, h);
    GeRttTarget t[GE_MAX_TARGETS];
    const int n = rtt_targets(t);
    int idx = ge_rtt_overlap(t, n, addr, bytes);
    bool depth = false;
    if (idx < 0 && s.zbuf_ptr != 0) {
        const GeRttTarget z = {true, (s.zbuf_ptr & 0x07FFFFFFu) & 0x001FFFF0u,
                               s.zbuf_width & 0x7FCu, 1};
        if (ge_rtt_overlap(&z, 1, addr, bytes) == 0) { idx = 0; depth = true; }
    }
    if (idx < 0) return;
    static uint64_t seen[64];
    static int nseen = 0;
    const uint64_t k = (static_cast<uint64_t>(depth ? 0xFFu : static_cast<uint32_t>(idx)) << 56) ^
                       (static_cast<uint64_t>(addr) << 8) ^ s.tex_format;
    for (int i = 0; i < nseen; ++i) if (seen[i] == k) return;
    if (nseen < 64) seen[nseen++] = k; else return;
    const GeRttHit hit = depth ? GeRttHit{-1, 0, 0}
        : ge_rtt_find(t, n, addr, static_cast<int>(s.tex_format), bufw, h);
    const GeTarget& g = g_targets[depth ? 0 : idx];
    std::fprintf(stderr,
        "[RTT] sample key=0x%06X%s ta=0x%08X fmt=%u %dx%d bufw=%u fb_stride=%u fb_fmt=%u "
        "match=%d (%d,%d)\n",
        depth ? ((s.zbuf_ptr & 0x07FFFFFFu) & 0x001FFFF0u) : g.key,
        depth ? " depth" : (idx == g_current_target ? " self" : ""),
        addr, s.tex_format, w, h, bufw, depth ? (s.zbuf_width & 0x7FCu) : g.stride,
        depth ? 1u : g.fb_fmt, hit.index, hit.x, hit.y);
}

void ge_draw_note_transfer(const GeTransferRegs& r) {
    const GeTransferRect x = psp_ge_transfer_rect(r);
    const uint32_t src_bytes = ((x.sy + x.height - 1) * x.src_stride + x.sx + x.width) * x.bpp;
    const uint32_t dst_bytes = ((x.dy + x.height - 1) * x.dst_stride + x.dx + x.width) * x.bpp;
    GeRttTarget t[GE_MAX_TARGETS];
    const int n = rtt_targets(t);
    const int from = ge_rtt_overlap(t, n, x.src, src_bytes);
    const int into = ge_rtt_overlap(t, n, x.dst, dst_bytes);
    if (into >= 0) g_targets[into].cpu_newer = true;   // guest bytes now differ from GL
    if (from < 0 && into < 0) return;
    static int logged = 0;
    if (++logged <= 64)
        std::fprintf(stderr, "[RTT] xfer src=0x%08X dst=0x%08X %ux%u bpp=%u from_target=%d into_target=%d\n",
                     x.src, x.dst, x.width, x.height, x.bpp, from, into);
}

void ge_draw_prim(
    uint8_t* rdram,
    int prim_type,
    int count,
    std::vector<DecodedVertex>* predecoded
) {
    if (predecoded) count = static_cast<int>(predecoded->size());
    if (perf_enabled()) g_perf.prims.fetch_add(1, std::memory_order_relaxed);
    // Phase 11.4 / Plan 02 Task 2 — alternate-path GE PRIM investigation.
    //
    // Even when count <= 0 we tally [GE_PRIM_DETAIL] before the early-return
    // so the totals match the GE_SUMMARY total_prims figure exactly. The
    // counters here are NEVER rate-limited (they are scalar increments, no
    // I/O per call); the dump happens once in ge_prim_detail_atexit_summary.
    //
    // Tertiary finding from Plan 01 (DIAGNOSTIC §"Raw Log Excerpt"): the
    // GE pipeline processes 243 PRIM commands but only 57 [DRAW_PRIM] log
    // lines reach stderr (all clear=1). The pre-existing [DRAW_PRIM] log
    // rate-limit (first 50 + every 25th + first 5 non-clear) hides the
    // breakdown. This block counts EVERY call by (prim_type, clear) bucket.
    // (Counter definitions moved to file scope -- g_prim_* above -- so the
    // debug socket's I command can snapshot them via ge_draw_get_stats.)
    static bool prim_detail_atexit_installed = false;
    if (!prim_detail_atexit_installed) {
        psp_on_exit([]() {
            std::fprintf(stderr,
                "[GE_PRIM_DETAIL_SUMMARY] total_observed=%d zero_count=%d\n",
                g_prim_total_observed, g_prim_type_zero_count);
            int real_ns = 0, sprite_nc = 0, clears = 0;
            for (int t = 0; t < 8; ++t) {
                int normal = g_prim_type_count[t];
                int clear = g_prim_type_count_clear[t];
                clears += clear;
                if (t == 6) sprite_nc += normal; else real_ns += normal;
                if (normal == 0 && clear == 0) continue;
                std::fprintf(stderr,
                    "[GE_PRIM_DETAIL] type=%d normal=%d clear=%d total=%d\n",
                    t, normal, clear, normal + clear);
            }
            // Canonical one-line verdict for the verification harness.
            std::fprintf(stderr,
                "[GE_GEOM_VERDICT] real_nonsprite=%d sprite_nonclear=%d "
                "clears=%d => %s\n",
                real_ns, sprite_nc, clears,
                real_ns > 0 ? "GRAPHICS" : "NO-GRAPHICS");
        });
        prim_detail_atexit_installed = true;
    }

    // Self-test for the verification harness (scripts/verify_geometry.sh):
    // PSPRECOMP_GEOM_SELFTEST=1 synthesizes ONE real type-4 (TRIANGLE_STRIP,
    // non-clear, non-sprite) draw through the exact counter + sentinel path, so
    // the GRAPHICS detection can be proven end-to-end without the real asset bug
    // fixed. Proves the detector is not a constant-false oracle. No GL side
    // effects; runs once.
    // The flag is read once: getenv costs ~1.6 us on Windows, and this runs on every PRIM (thousands per
    // frame in a battle).
    static const bool selftest_wanted = std::getenv("PSPRECOMP_GEOM_SELFTEST") != nullptr;
    static bool selftest_done = false;
    if (!selftest_done && selftest_wanted) {
        selftest_done = true;
        g_prim_type_count[GE_PRIM_TRIANGLE_STRIP]++;
        std::fprintf(stderr,
            "[GE_GEOM_REAL_DRAW] FIRST real geometry draw: "
            "type=%d count=%d (success-bar (b) MET) [SELFTEST]\n",
            GE_PRIM_TRIANGLE_STRIP, 3);
        std::fflush(stderr);
    }

    g_prim_total_observed++;
    if (count <= 0) {
        g_prim_type_zero_count++;
        return;
    }

    static int prim_call_count = 0;
    prim_call_count++;

    const GeState& state = ge_get_state();
    const bool drawlog = g_drawlog_lists.load(std::memory_order_relaxed) > 0;
    const int list_index = g_list_prim_index++;
    if (drawlog) drawlog_prim(state, list_index, prim_type, count);
    if (list_index == 0) rtt_check_cpu_writes(rdram);
    if (g_current_target >= 0) {
        GeTarget& t = g_targets[g_current_target];
        t.stride = state.framebuf_width & 0x7FCu;
        t.fb_fmt = state.framebuf_format & 3u;
        t.gl_written = true;
        // After a CPU write, GL's copy lacks the CPU's pixels until a clear redraws the
        // buffer from scratch (Task 6 would upload them instead).
        if (state.clear_mode) t.cpu_newer = false;
    }
    if (state.texture_enable && !state.clear_mode) rtt_note_sample(state);
    if (!state.clear_mode && list_index >= g_skip_lo.load(std::memory_order_relaxed) &&
        list_index <= g_skip_hi.load(std::memory_order_relaxed)) {
        return;
    }

    // Scissor (GE_CMD_SCISSOR1/2: inclusive x1,y1 / x2,y2, 10 bits each,
    // top-left origin) applies to every draw, clears included. The FBO uses
    // GL's bottom-left origin, so y is flipped.
    {
        const GeScissor sc = ge_compute_scissor(state.scissor1, state.scissor2,
                                                PSP_FB_HEIGHT, g_render_scale);
        if (!sc.enabled) {
            glDisable(GL_SCISSOR_TEST);  // never set: whole buffer
        } else {
            glEnable(GL_SCISSOR_TEST);
            glScissor(sc.x, sc.y, sc.w, sc.h);
        }
    }
    {
        int t_idx = (prim_type >= 0 && prim_type < 8) ? prim_type : 7;
        if (state.clear_mode) {
            g_prim_type_count_clear[t_idx]++;
        } else {
            g_prim_type_count[t_idx]++;
        }

        // [GE_GEOM_VERDICT] live verification sentinels — the definitive answer
        // to "are graphics being set up?". These print DURING the run and are
        // flushed immediately, so they survive ANY exit path (SIGTERM, SIGKILL,
        // or a hung shutdown) — unlike the atexit [GE_PRIM_DETAIL_SUMMARY],
        // which only fires on a clean return from main(). Pure diagnostic
        // output: changes no game behavior.
        //
        // Success-bar half (b) == ONE real draw with type!=6 (non-sprite) AND
        // clear==0 (non-clear) AND normal>0. The line below appears in the log
        // if and only if that condition is ever met.
        static bool geom_first_real_logged = false;
        if (!state.clear_mode && t_idx != 6 && !geom_first_real_logged) {
            geom_first_real_logged = true;
            std::fprintf(stderr,
                "[GE_GEOM_REAL_DRAW] FIRST real geometry draw: "
                "type=%d count=%d (success-bar (b) MET)\n",
                prim_type, count);
            std::fflush(stderr);
        }

        // Periodic heartbeat: a killed or hung run still leaves a current tally
        // in the log (every 64K PRIMs observed). real_nonsprite>0 == graphics.
        if (g_prim_total_observed % 65536 == 0) {
            int real_ns = 0, sprite_nc = 0, clears = 0;
            for (int t = 0; t < 8; ++t) {
                clears += g_prim_type_count_clear[t];
                if (t == 6) sprite_nc += g_prim_type_count[t];
                else real_ns += g_prim_type_count[t];
            }
            std::fprintf(stderr,
                "[GE_GEOM_HEARTBEAT] prims=%d real_nonsprite=%d "
                "sprite_nonclear=%d clears=%d\n",
                g_prim_total_observed, real_ns, sprite_nc, clears);
            std::fflush(stderr);
        }
    }

    // Log the first 50 PRIMs and the first non-clear ones; every 25th PRIM
    // only with PSPRECOMP_PRIM_LOG (it grew logs to 1 GB in a 3 h run).
    static int non_clear_logged = 0;
    static const bool prim_log_all = std::getenv("PSPRECOMP_PRIM_LOG") != nullptr;
    bool is_non_clear_first = (!state.clear_mode && non_clear_logged < 5);
    if (prim_call_count <= 50
        || (prim_log_all && prim_call_count % 25 == 0)
        || is_non_clear_first) {
        std::fprintf(stderr,
            "[DRAW_PRIM] #%d type=%d count=%d clear=%d "
            "vtype=0x%06X vaddr=0x%08X\n",
            prim_call_count, prim_type, count,
            state.clear_mode ? 1 : 0,
            state.vertex_type, state.vertex_addr);
        if (is_non_clear_first) non_clear_logged++;
    }

    // Clear mode: use glClear instead of drawing
    if (state.clear_mode) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // Decode first vertex for clear color
        std::vector<DecodedVertex> clear_verts;
        ge_decode_vertices(
            rdram, state, prim_type, count, clear_verts);
        if (!clear_verts.empty()) {
            float r = clear_verts[0].color[0] / 255.0f;
            float g = clear_verts[0].color[1] / 255.0f;
            float b = clear_verts[0].color[2] / 255.0f;
            float a = clear_verts[0].color[3] / 255.0f;
            if (prim_call_count <= 20) {
                std::fprintf(stderr,
                    "[DRAW_CLEAR] color=(%0.2f,%0.2f,%0.2f,%0.2f) "
                    "flags=0x%X verts=%zu\n",
                    r, g, b, a, state.clear_flags,
                    clear_verts.size());
            }
            glClearColor(r, g, b, a);
            if (prim_call_count <= 5) {
                std::fprintf(stderr,
                    "[DRAW_CLEAR] glClearColor(%.3f, %.3f, %.3f, %.3f) "
                    "flags=0x%X\n",
                    r, g, b, a, state.clear_flags);
            }
        }

        GLbitfield clear_bits = 0;
        if (state.clear_flags & 1)
            clear_bits |= GL_COLOR_BUFFER_BIT;
        if (state.clear_flags & 2)
            clear_bits |= GL_STENCIL_BUFFER_BIT;
        if (state.clear_flags & 4)
            clear_bits |= GL_DEPTH_BUFFER_BIT;
        if (clear_bits == 0)
            clear_bits = GL_COLOR_BUFFER_BIT
                         | GL_DEPTH_BUFFER_BIT;

        // Clear depth to the clear draw's vertex z (PSP/PPSSPP clear mode
        // writes the vertex depth; GL depth = PSP z / 65535). A transform-mode
        // clear keeps the range's far value.
        if (clear_bits & GL_DEPTH_BUFFER_BIT) {
            if (ge_vtype_through(state.vertex_type) && !clear_verts.empty()) {
                glClearDepth(static_cast<GLclampd>(ge_clear_depth(clear_verts[0].pos[2])));
            } else {
                const GeViewportDepth vp = compute_state_viewport_depth(state);
                glClearDepth(static_cast<GLclampd>(vp.far_z));
            }
        }

        // Stencil: the clear's vertex alpha through MASKALPHA (psp_ge_stencil.h).
        // PPSSPP takes the first rectangle's second vertex; other clear prims
        // draw with stencil ref 0xFF.
        if (clear_bits & GL_STENCIL_BUFFER_BIT) {
            uint8_t alpha = 0xFF;
            if (prim_type == GE_PRIM_RECTANGLES) {
                alpha = clear_verts.size() > 1 ? clear_verts[1].color[3] : 0;
            }
            const GeStencilClear sc =
                ge_stencil_clear(stencil_input(state), state.clear_flags, alpha);
            glClearStencil(sc.value);
            gl_state_stencil_mask(sc.write_mask);
        }

        // Temporarily disable depth mask restriction
        glDepthMask(GL_TRUE);
        glClear(clear_bits);
        g_has_drawn_prims = true;  // Clear counts as drawing
        return;
    }

    // Viewport / depth-range (issue #23). Transform-mode draws emit NDC via
    // the real MVP path, so they must map through the PSP viewport
    // scale/offset + depth range to land on screen. Through-mode (2D/sprite)
    // already bakes the full-screen 240/136 mapping into NDC in
    // ge_transform_vertices, so it keeps the full-buffer viewport set by
    // ge_draw_begin_list and an identity depth range.
    if (!ge_vtype_through(state.vertex_type)) {
        const GeViewportDepth vp = compute_state_viewport_depth(state);
        glViewport(vp.x, vp.y, vp.w, vp.h);
        glDepthRange(vp.near_z, vp.far_z);
    } else {
        glViewport(0, 0, fb_w(), fb_h());
        glDepthRange(0.0, 1.0);
    }

    // Decode vertices
    std::vector<DecodedVertex> decoded;
    if (predecoded) {
        decoded.swap(*predecoded);
    } else {
        ge_decode_vertices(
            rdram, state, prim_type, count, decoded);
    }
    if (decoded.empty()) return;

    // PSPRECOMP_VTX_WATCH=<hex vtype>: log the first draws of that vertex
    // type (raw bytes of the first vertices + decoded positions).
    {
        static const char* vw = std::getenv("PSPRECOMP_VTX_WATCH");
        static int vw_logs = 0;
        if (vw && vw_logs < 12 &&
            state.vertex_type == static_cast<uint32_t>(std::strtoul(vw, nullptr, 16))) {
            vw_logs++;
            const int stride = ge_vertex_stride(state.vertex_type);
            const uint8_t* rv = rdram + (state.vertex_addr & 0x07FFFFFFu);
            std::fprintf(stderr, "[VTX-WATCH] prim=%d count=%d stride=%d vaddr=0x%08X\n",
                         prim_type, count, stride, state.vertex_addr);
            for (int i = 0; i < count && i < 6; i++) {
                std::fprintf(stderr, "[VTX-WATCH]   v%d raw:", i);
                for (int b = 0; b < stride; b++) {
                    std::fprintf(stderr, " %02X", rv[i * stride + b]);
                }
                std::fprintf(stderr, "  -> pos %.1f,%.1f,%.1f rgba %u,%u,%u,%u\n",
                             decoded[i].pos[0], decoded[i].pos[1], decoded[i].pos[2],
                             decoded[i].color[0], decoded[i].color[1], decoded[i].color[2],
                             decoded[i].color[3]);
            }
        }
    }

    // Largest V (texels) this draw samples: PPSSPP's vertBounds.maxV, used by
    // texture replacement to hash only the visible rows of 512-tall textures.
    uint16_t draw_max_v = 0;
    if (state.texture_enable && ge_vtype_through(state.vertex_type)) {
        for (const DecodedVertex& v : decoded) {
            if (v.has_uv && v.uv[1] > draw_max_v) {
                draw_max_v = static_cast<uint16_t>(std::min(v.uv[1], 65535.0f));
            }
        }
    }

    // Render scale > 1: snap 2D (through-mode) fills to the PSP pixel grid so
    // pieces that touch at 1x still touch (ge_snap_through_positions). Lines
    // and points keep their positions; scale 1 is untouched.
    if (g_render_scale > 1 && ge_vtype_through(state.vertex_type) &&
        prim_type >= GE_PRIM_TRIANGLES) {
        ge_snap_through_positions(decoded);
    }

    // Transform
    ge_transform_vertices(decoded, state);

    // Through-mode GE lines are drawn as 1-px quads biased right/down by
    // PPSSPP (SoftwareTransformCommon.cpp:ExpandLines); GL_LINES lands an
    // integer-x vertical line one pixel left of that, which put the game's
    // menu shadows and the beat outline 1 px left of the reference. Expand
    // to the same two triangles and draw them as triangles.
    if (ge_vtype_through(state.vertex_type) && prim_type == GE_PRIM_LINES) {
        std::vector<DecodedVertex> quads;
        quads.reserve((decoded.size() / 2) * 6);
        DecodedVertex quad[6];
        for (size_t i = 0; i + 1 < decoded.size(); i += 2) {
            ge_expand_line_through(decoded[i], decoded[i + 1], quad);
            quads.insert(quads.end(), quad, quad + 6);
        }
        decoded.swap(quads);
        prim_type = GE_PRIM_TRIANGLES;
    }

    // Handle RECTANGLES: expand 2 verts to 6 (two tris)
    std::vector<PackedVertex> packed;
    if (prim_type == GE_PRIM_RECTANGLES) {
        // Each rectangle is 2 vertices (top-left, bottom-right). The
        // corners arrive in clip space (transform mode); ge_expand_rectangle
        // divides by w and drops rectangles with a corner behind the eye
        // plane (w <= 0), emitting flat NDC quads with w = 1.
        int rect_count = count / 2;
        packed.reserve(rect_count * 6);
        for (int r = 0; r < rect_count; r++) {
            int i0 = r * 2;
            int i1 = r * 2 + 1;
            if (i1 >= static_cast<int>(decoded.size()))
                break;

            DecodedVertex expanded[6];
            if (!ge_expand_rectangle(decoded[i0], decoded[i1],
                                     expanded))
                continue;
            for (int k = 0; k < 6; k++) {
                const DecodedVertex& v = expanded[k];
                packed.push_back({
                    {v.pos[0], v.pos[1], v.pos[2], v.w},
                    {v.uv[0], v.uv[1]},
                    {v.color[0], v.color[1],
                     v.color[2], v.color[3]},
                    v.fog
                });
            }
        }
    } else {
        // Pack decoded vertices
        packed.resize(decoded.size());
        for (size_t i = 0; i < decoded.size(); i++) {
            const auto& v = decoded[i];
            packed[i] = {
                {v.pos[0], v.pos[1], v.pos[2], v.w},
                {v.uv[0], v.uv[1]},
                {v.color[0], v.color[1],
                 v.color[2], v.color[3]},
                v.fog
            };
        }
    }

    if (drawlog) {
        // Screen rectangle (PSP pixels, 480x272) covered by all vertices
        // with w > 0: finds the draw that paints a given screen region.
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const PackedVertex& pv : packed) {
            if (pv.pos[3] <= 0.0f) continue;
            const float px = (pv.pos[0] / pv.pos[3] + 1.0f) * 240.0f;
            const float py = (1.0f - pv.pos[1] / pv.pos[3]) * 136.0f;
            x0 = std::min(x0, px); x1 = std::max(x1, px);
            y0 = std::min(y0, py); y1 = std::max(y1, py);
        }
        // Draws over the screen centre log every vertex (up to 256), the
        // rest their first two.
        const bool centre = x0 <= 240.0f && x1 >= 240.0f && y0 <= 136.0f && y1 >= 136.0f;
        const size_t nlog = centre ? std::min<size_t>(packed.size(), 256) : std::min<size_t>(packed.size(), 2);
        for (size_t i = 0; i < nlog; i++) {
            const PackedVertex& pv = packed[i];
            std::fprintf(stderr,
                "[DL]   v%zu/%zu pos=%.3f,%.3f,%.3f,%.3f uv=%.3f,%.3f rgba=%u,%u,%u,%u fog=%.3f\n",
                i, packed.size(), pv.pos[0], pv.pos[1], pv.pos[2], pv.pos[3],
                pv.uv[0], pv.uv[1], pv.color[0], pv.color[1], pv.color[2],
                pv.color[3], pv.fog);
        }
        if (x0 <= x1) {
            std::fprintf(stderr, "[DL]   bbox x=%.0f..%.0f y=%.0f..%.0f\n", x0, x1, y0, y1);
        }
    }
    if (packed.empty()) return;

    // Upload to VBO
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 packed.size() * sizeof(PackedVertex),
                 packed.data(),
                 GL_STREAM_DRAW);

    // Bind shader and set uniforms
    ge_shader_use();
    ge_shader_set_uniforms(state);

    // Bind texture if enabled
    if (state.texture_enable) {
        glActiveTexture(GL_TEXTURE0);
        ge_texture_bind(rdram, g_frame_counter, draw_max_v);
        // PSPRECOMP_TEX_WATCH companion: the watched texture's first vertex.
        static const char* watch = std::getenv("PSPRECOMP_TEX_WATCH");
        static int watch_logs = 0;
        if (watch &&
            (state.tex_addr[0] & 0x00FFFFFFu) ==
                (static_cast<uint32_t>(std::strtoul(watch, nullptr, 16)) & 0x00FFFFFFu) &&
            (++watch_logs % 60) == 1) {
            const PackedVertex& pv = packed[0];
            std::fprintf(stderr,
                         "[TEX-WATCH] verts=%zu v0 pos=%.1f,%.1f,%.1f uv=%.3f,%.3f rgba=%u,%u,%u,%u\n",
                         packed.size(), pv.pos[0], pv.pos[1], pv.pos[2], pv.uv[0], pv.uv[1],
                         pv.color[0], pv.color[1], pv.color[2], pv.color[3]);
            const uint8_t* rv = rdram + (state.vertex_addr & 0x07FFFFFFu);
            std::fprintf(stderr,
                         "[TEX-WATCH] texfunc_raw=0x%X vaddr=0x%08X raw v0: "
                         "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                         "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                         state.tex_func_raw, state.vertex_addr, rv[0], rv[1], rv[2], rv[3],
                         rv[4], rv[5], rv[6], rv[7], rv[8], rv[9], rv[10], rv[11], rv[12],
                         rv[13], rv[14], rv[15], rv[16], rv[17], rv[18], rv[19], rv[20], rv[21],
                         rv[22], rv[23]);
        }
    }

    // Pixel write masks (MASKRGB/MASKALPHA): games draw opaque quads with
    // RGB masked off to touch only alpha/stencil; ignoring the mask painted
    // them as black rectangles.
    {
        bool w[4];
        ge_color_write_mask(state.mask_rgb, state.mask_alpha, w);
        glColorMask(w[0], w[1], w[2], w[3]);
    }

    // Set GL state: alpha blend
    if (state.alpha_blend_enable) {
        GeBlendSetup b = ge_blend_setup(
            state.blend_mode, state.blend_fix_a, state.blend_fix_b);
        gl_state_set_cap(GL_BLEND, true);
        gl_state_blend_func(b.src, b.dst);
        gl_state_blend_equation(b.equation);
        if (b.set_constant) {
            gl_state_blend_color(b.constant[0], b.constant[1], b.constant[2]);
        }
        ge_shader_set_blend(b.src1_rgb, b.src1_a, b.fix_a);
    } else {
        gl_state_set_cap(GL_BLEND, false);
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        ge_shader_set_blend(0, 0, zero);
    }

    // Depth test (glDepthMask stays direct: the clear path also sets it)
    gl_state_set_cap(GL_DEPTH_TEST, state.depth_test_enable);
    if (state.depth_test_enable) {
        gl_state_depth_func(map_depth_func(state.depth_func));
        glDepthMask(
            state.depth_write_disable
            ? GL_FALSE : GL_TRUE);
    }

    // Stencil test (PPSSPP ConvertStencilFuncState, psp_ge_stencil.h)
    {
        const GeStencilState st = ge_stencil_state(stencil_input(state));
        gl_state_set_cap(GL_STENCIL_TEST, st.enabled);
        if (st.enabled) {
            gl_state_stencil_func(map_depth_func(st.func), st.ref, st.test_mask);
            gl_state_stencil_op(map_stencil_op(st.sfail), map_stencil_op(st.zfail),
                                map_stencil_op(st.zpass));
            gl_state_stencil_mask(st.write_mask);
        }
    }

    // Cull face
    gl_state_set_cap(GL_CULL_FACE, state.cull_enable);
    if (state.cull_enable) {
        glCullFace(
            state.cull_face == 0
            ? GL_BACK : GL_FRONT);
    }

    // Draw
    GLenum gl_prim = map_prim_type(prim_type);
    if (perf_enabled()) g_perf.draw_calls.fetch_add(1, std::memory_order_relaxed);
    glDrawArrays(gl_prim, 0,
                 static_cast<GLsizei>(packed.size()));
    g_has_drawn_prims = true;

    glBindVertexArray(0);
}

// Service the pending screenshot/auto-capture from the given front FBO.
static void present_service_screenshot(GLuint fbo) {
    if (!g_screenshot_enabled || g_screenshot_done || !g_has_drawn_prims) {
        return;
    }
    write_fbo_tga(fbo, g_screenshot_path);
    g_screenshot_done = true;
}

// GL filter for the window blit: PSPRECOMP_PRESENT_FILTER=nearest keeps sharp
// pixels, anything else scales linearly.
static GLenum present_filter() {
    static const GLenum f = [] {
        const char* e = std::getenv("PSPRECOMP_PRESENT_FILTER");
        return (e && std::strcmp(e, "nearest") == 0) ? GLenum(GL_NEAREST) : GLenum(GL_LINEAR);
    }();
    return f;
}

// Blit the given front FBO to the window and swap. Common to the routine
// (page-flip) present and the liveness net.
static void present_blit(GLuint fbo) {
    SDL_Window* window = psp_get_sdl_window();
    if (!window) return;
    if (perf_enabled()) g_perf.presents.fetch_add(1, std::memory_order_relaxed);
    glDisable(GL_SCISSOR_TEST);  // the GE scissor must not crop the blit
    present_service_screenshot(fbo);
    // Letterbox the rendered image into the drawable, keeping the PSP aspect
    // ratio; linear scaling unless PSPRECOMP_PRESENT_FILTER=nearest. The GE
    // path re-sets viewport, colour mask and clear colour before its draws.
    int win_w = 480, win_h = 272;
    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
    const PresentRect r = present_fit_rect(win_w, win_h);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, fb_w(), fb_h(), r.x, r.y, r.x + r.w, r.y + r.h,
                      GL_COLOR_BUFFER_BIT, present_filter());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The overlay uses its own program, textures and enables while active;
    // drop the shadow so the next GE draw re-emits everything it needs.
    const bool osk_drew = psp_osk_overlay_draw(win_w, win_h, r.x, r.y, r.w, r.h);
    const bool prompt_drew = psp_menu_prompt_overlay_draw(win_w, win_h, r.x, r.y, r.w, r.h);
    const bool toast_drew = psp_toast_overlay_draw(win_w, win_h, r.x, r.y, r.w, r.h);
    if (osk_drew || prompt_drew || toast_drew) {
        gl_state_invalidate();
    }
    {
        PerfScope swap_scope(g_perf.swap_ns);
        SDL_GL_SwapWindow(window);
    }
    perf_frame_end();  // PSPRECOMP_FRAMELOG: this frame's row
    g_present_dirty = false;
    g_last_present = std::chrono::steady_clock::now();
}

void ge_present_frame(
    uint8_t* rdram,
    uint32_t fb_addr,
    uint32_t fb_stride,
    uint32_t fb_format
) {
    (void)rdram;
    (void)fb_stride;
    (void)fb_format;

    // Select the FBO matching the displayed (scanned-out) VRAM address — the
    // front buffer. On key miss for a real (non-zero) address the guest is
    // scanning out a VRAM buffer the GE has not rendered into yet (e.g. the
    // first flip to the back buffer before any list targets it). Create that
    // buffer's own FBO — cleared to black — and present THAT, rather than
    // blitting an unrelated sibling slot (which would publish a different
    // buffer's stale/cleared contents as if they were this frame). This keeps
    // present strictly per-address: a fresh flip shows a clean buffer, and any
    // geometry the GE later renders into this address lands in the same FBO.
    uint32_t key = ge_fb_key(fb_addr);
    int slot = -1;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo != 0 && g_targets[i].key == key) { slot = i; break; }
    }
    if (slot < 0) {
        if (fb_addr != 0) {
            // Materialize the displayed buffer's own (cleared) FBO.
            slot = acquire_target(key);
        } else {
            // fb_addr==0 (no display buffer set yet): keep the
            // most-recently-rendered target so present never blanks.
            slot = (g_front_target >= 0) ? g_front_target : g_current_target;
            static int fallback_count = 0;
            if (++fallback_count <= 20) {
                std::fprintf(stderr,
                    "[PRESENT] fallback: no FBO for fb=0x%08X key=0x%06X "
                    "(using slot=%d)\n", fb_addr, key, slot);
            }
        }
    }
    if (slot < 0) return;  // nothing rendered yet

    g_front_target = slot;
    g_targets[slot].last_use = ++g_target_clock;

    static int present_count = 0;
    if (++present_count <= 5) {
        std::fprintf(stderr,
            "[PRESENT] Frame %d (fb=0x%08X key=0x%06X stride=%u fmt=%u)\n",
            present_count, fb_addr, key, fb_stride, fb_format);
    }
    present_blit(g_targets[slot].fbo);
}

// Liveness net (§2.1): if lists were processed but no real page-flip arrived
// within the stale budget, present the current front buffer once. Preserves
// "content becomes visible even without a flip" without making every list a
// frame boundary. Main (GL) thread only — same thread as the routine present.
void ge_draw_present_if_stale(uint8_t* rdram) {
    (void)rdram;
    if (!g_present_dirty) return;
    auto age = std::chrono::steady_clock::now() - g_last_present;
    if (age < std::chrono::milliseconds(g_present_stale_ms)) return;
    GLuint fbo = front_fbo_id();
    if (fbo == 0) { g_present_dirty = false; return; }
    present_blit(fbo);
}

// ---- Debug socket accessors (issue #35) ----

void ge_draw_get_stats(
    uint32_t* frames,
    int* prims_total,
    int* real_nonsprite,
    int* sprite_nonclear,
    int* clears
) {
    // Racy reads of render-thread counters -- diagnostics only.
    if (frames) *frames = g_frame_counter;
    if (prims_total) *prims_total = g_prim_total_observed;
    int real_ns = 0, sprite_nc = 0, clr = 0;
    for (int t = 0; t < 8; ++t) {
        clr += g_prim_type_count_clear[t];
        if (t == 6) sprite_nc += g_prim_type_count[t];
        else real_ns += g_prim_type_count[t];
    }
    if (real_nonsprite) *real_nonsprite = real_ns;
    if (sprite_nonclear) *sprite_nonclear = sprite_nc;
    if (clears) *clears = clr;
}

bool ge_draw_capture_screenshot(const char* path, int timeout_ms) {
    if (!g_draw_ready.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
            "[DRAW] Screenshot request before GL init: %s\n", path);
        return false;
    }
    std::unique_lock<std::mutex> lock(g_ss_req_mutex);
    if (g_ss_req_state == SS_PENDING) {
        // Another client's capture is in flight -- refuse instead of
        // overwriting its path.
        return false;
    }
    std::strncpy(g_ss_req_path, path, sizeof(g_ss_req_path) - 1);
    g_ss_req_path[sizeof(g_ss_req_path) - 1] = '\0';
    g_ss_req_state = SS_PENDING;
    bool finished = g_ss_req_cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [] { return g_ss_req_state == SS_DONE
                    || g_ss_req_state == SS_FAILED; });
    bool ok = finished && g_ss_req_state == SS_DONE;
    g_ss_req_state = SS_IDLE;   // reset (also cancels on timeout)
    return ok;
}

void ge_draw_service_screenshot_request() {
    char path[sizeof(g_ss_req_path)];
    {
        std::lock_guard<std::mutex> lock(g_ss_req_mutex);
        if (g_ss_req_state != SS_PENDING) return;
        std::memcpy(path, g_ss_req_path, sizeof(path));
    }
    // GL work outside the request lock (requester only waits on the cv).
    bool ok = g_draw_ready.load(std::memory_order_acquire)
              && capture_fbo_to_tga(path);
    {
        std::lock_guard<std::mutex> lock(g_ss_req_mutex);
        // Only publish if the requester hasn't timed out and reset state.
        if (g_ss_req_state == SS_PENDING) {
            g_ss_req_state = ok ? SS_DONE : SS_FAILED;
        }
    }
    g_ss_req_cv.notify_all();
}
