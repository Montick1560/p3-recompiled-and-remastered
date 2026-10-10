#include "psp_ge_texture.h"
#include "psp_ge.h"
#include "psp_ge_sampler.h"
#include "psp_ge_texdecode.h"
#include "psp_ge_constants.h"
#include "psp_memory.h"
#include "psp_texrep.h"
#include "recomp.h"

#include <glad/glad.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

// ---- Module state ----
static std::array<TexCacheEntry, TEX_CACHE_SIZE> g_tex_cache;
static bool g_tex_initialized = false;

// ---- FNV-1a hash ----

static uint32_t fnv1a_hash(
    const uint8_t* data, size_t len
) {
    uint32_t hash = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

// ---- Cache operations ----

/// Find cache entry matching (addr, hash) or evict LRU.
static int find_or_evict(
    uint32_t psp_addr, uint32_t content_hash
) {
    int best_slot = 0;
    uint32_t oldest_frame = UINT32_MAX;

    for (int i = 0; i < TEX_CACHE_SIZE; i++) {
        auto& e = g_tex_cache[i];
        if (e.valid
            && e.psp_addr == psp_addr
            && e.content_hash == content_hash) {
            return i;  // Exact match
        }
        if (!e.valid) {
            return i;  // Empty slot
        }
        if (e.last_frame < oldest_frame) {
            oldest_frame = e.last_frame;
            best_slot = i;
        }
    }

    // Evict oldest
    auto& evict = g_tex_cache[best_slot];
    if (evict.gl_tex != 0) {
        glDeleteTextures(1, &evict.gl_tex);
        evict.gl_tex = 0;
    }
    evict.valid = false;
    return best_slot;
}

// ---- Sampler ----

/// Set the GL_TEXTURE_2D sampler from the draw's TEXFILTER / TEXWRAP. The same
/// cached texture can be drawn with different registers, so this runs on every
/// bind. PSPRECOMP_TEX_FILTER=nearest|linear overrides the filter bits; wrap is
/// always taken from the registers.
static void apply_sampler(const GeState& state, int forced_filter) {
    static const GeFilterOverride ov = ge_filter_override_parse(
        std::getenv("PSPRECOMP_TEX_FILTER"));
    const GeSampler s =
        ge_sampler_from_regs(state.tex_filter, state.tex_wrap, ov);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    s.min_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    s.mag_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    s.clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    s.clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    if (forced_filter != 0) {  // texture pack [filtering]: 1 nearest, 2 linear
        const GLint f = forced_filter == 1 ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    }
}

// ---- Public API ----

void ge_texture_init() {
    for (auto& e : g_tex_cache) {
        e = TexCacheEntry{};
    }
    g_tex_initialized = true;
    std::fprintf(stderr, "[TEX] Texture cache initialized "
                         "(%d entries)\n", TEX_CACHE_SIZE);
    ge_texrep_init();
}

void ge_texture_shutdown() {
    for (auto& e : g_tex_cache) {
        if (e.gl_tex != 0) {
            glDeleteTextures(1, &e.gl_tex);
            e.gl_tex = 0;
        }
        e.valid = false;
    }
    g_tex_initialized = false;
    std::fprintf(stderr, "[TEX] Texture cache shutdown\n");
}

void ge_texture_invalidate_all() {
    for (auto& e : g_tex_cache) {
        if (e.gl_tex != 0) {
            glDeleteTextures(1, &e.gl_tex);
            e.gl_tex = 0;
        }
        e.valid = false;
    }
}

GLuint ge_texture_bind(
    uint8_t* rdram,
    uint32_t frame_num,
    uint16_t draw_max_v
) {
    const GeState& state = ge_get_state();

    if (!state.texture_enable) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return 0;
    }

    // Extract texture parameters
    int log2_w = state.tex_size[0] & 0xFF;
    int log2_h = (state.tex_size[0] >> 8) & 0xFF;
    int width  = 1 << log2_w;
    int height = 1 << log2_h;
    int fmt    = state.tex_format;

    // Compute PSP address
    uint32_t psp_addr = state.tex_addr[0];
    // Combine with TEXBUFWIDTH upper bits for high address
    uint32_t bufw_upper =
        (state.tex_bufw[0] >> 16) & 0xFF;
    psp_addr |= (bufw_upper << 24);
    uint32_t masked_addr = psp_addr & PSP_ADDR_MASK;

    // Decode parameters (PPSSPP semantics: CLUTADDR low 24 bits, CLUTADDRUPPER
    // shifted left 8; CLUTFORMAT = fmt | shift<<2 | mask<<8 | start<<16).
    GeTexDecodeParams params{};
    params.addr = psp_addr;
    params.width = width;
    params.height = height;
    params.bufw = static_cast<int>(state.tex_bufw[0] & 0x7FF);
    params.format = fmt;
    params.swizzle = (state.tex_mode & 1) != 0;
    params.clut_addr = (state.clut_addr & 0xFFFFFF)
        | ((state.clut_addr_upper << 8) & 0x0F000000);
    params.clut_format = static_cast<int>(state.clut_format & 0x3);
    params.clut_shift = static_cast<int>((state.clut_format >> 2) & 0x1F);
    params.clut_mask = static_cast<int>((state.clut_format >> 8) & 0xFF);
    params.clut_start = static_cast<int>((state.clut_format >> 16) & 0x1F);

    // Hash the whole texture span (row stride included), the decode
    // parameters and, for CLUT formats, the palette: a font atlas updated past
    // its first bytes or a palette fade must re-decode.
    const uint32_t span = ge_tex_byte_span(params);
    if (masked_addr + span > static_cast<uint32_t>(PSP_MEM_SIZE)) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return 0;
    }
    // PSPRECOMP_TEX_WATCH=<hex addr>: log the draw state of the first binds of
    // a texture at that address (diagnosing invisible textures).
    static const uint32_t watch = [] {
        const char* w = std::getenv("PSPRECOMP_TEX_WATCH");
        return w ? static_cast<uint32_t>(std::strtoul(w, nullptr, 16)) & PSP_ADDR_MASK : 0u;
    }();
    static int watch_logs = 0;
    if (watch != 0 && masked_addr == watch && (++watch_logs % 60) == 1) {
        std::fprintf(stderr,
                     "[TEX-WATCH] addr=0x%08X %dx%d fmt=%d bufw=%d texfunc=0x%X blend=%d mode=0x%X "
                     "atest=%d 0x%X vtype=0x%X clear=%d\n",
                     psp_addr, width, height, fmt, params.bufw, state.tex_func,
                     state.alpha_blend_enable ? 1 : 0, state.blend_mode,
                     state.alpha_test_enable ? 1 : 0, state.alpha_test, state.vertex_type,
                     state.clear_mode ? 1 : 0);
    }
    uint32_t content_hash = fnv1a_hash(rdram + masked_addr, static_cast<int>(span));
    const uint32_t param_words[6] = {
        static_cast<uint32_t>(params.bufw), static_cast<uint32_t>(fmt),
        params.swizzle ? 1u : 0u, state.clut_format, params.clut_addr,
        static_cast<uint32_t>(width | (height << 16)),
    };
    content_hash ^= fnv1a_hash(reinterpret_cast<const uint8_t*>(param_words),
                               static_cast<int>(sizeof(param_words))) * 0x9E3779B1u;
    if (fmt >= GE_TFMT_CLUT4 && fmt <= GE_TFMT_CLUT32) {
        const uint32_t clut_off = params.clut_addr & PSP_ADDR_MASK;
        if (clut_off + 1024 <= static_cast<uint32_t>(PSP_MEM_SIZE)) {
            content_hash ^= fnv1a_hash(rdram + clut_off, 1024) * 0x85EBCA6Bu;
        }
    }

    // Cache lookup
    int slot = find_or_evict(psp_addr, content_hash);
    auto& entry = g_tex_cache[slot];

    if (entry.valid
        && entry.psp_addr == psp_addr
        && entry.content_hash == content_hash) {
        // Cache hit
        entry.last_frame = frame_num;
        glBindTexture(GL_TEXTURE_2D, entry.gl_tex);
        if (entry.replace_pending && ge_texrep_epoch() != entry.replace_epoch) {
            // A pack image finished decoding since the last try: retry.
            entry.replace_epoch = ge_texrep_epoch();
            GeTexrepImage rep;
            const GeTexrepResult r = ge_texrep_replacement(rdram, state, draw_max_v, &rep);
            if (r == GeTexrepResult::Ready) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rep.w, rep.h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, rep.rgba);
                entry.forced_filter = static_cast<int>(rep.filter);
            }
            entry.replace_pending = r == GeTexrepResult::Pending;
        }
        apply_sampler(state, entry.forced_filter);
        return entry.gl_tex;
    }

    // Cache miss. Texture replacement (PSPRECOMP_TEXTURES): a pack PNG
    // replaces the decoded texture at its own size; UVs are normalised, so
    // nothing else changes. Otherwise decode from guest RAM (stride, swizzle
    // and CLUT transform in psp_ge_texdecode.cpp; unsupported formats:
    // magenta placeholder).
    GeTexrepImage rep;
    const uint64_t epoch = ge_texrep_epoch();
    const GeTexrepResult r = ge_texrep_enabled()
        ? ge_texrep_replacement(rdram, state, draw_max_v, &rep) : GeTexrepResult::None;
    const bool replaced = r == GeTexrepResult::Ready;
    entry.replace_pending = r == GeTexrepResult::Pending;
    entry.replace_epoch = epoch;
    if (entry.gl_tex == 0) {
        glGenTextures(1, &entry.gl_tex);
    }
    glBindTexture(GL_TEXTURE_2D, entry.gl_tex);
    if (replaced) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rep.w, rep.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rep.rgba);
    } else {
        std::vector<uint8_t> rgba;
        if (!ge_decode_texture(rdram, params, rgba)) {
            for (int i = 0; i < width * height; i++) {
                rgba[i * 4 + 0] = 0xFF;
                rgba[i * 4 + 1] = 0x00;
                rgba[i * 4 + 2] = 0xFF;
                rgba[i * 4 + 3] = 0xFF;
            }
        }
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8,
            width, height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE,
            rgba.data());
    }
    entry.forced_filter = replaced ? static_cast<int>(rep.filter) : 0;
    apply_sampler(state, entry.forced_filter);

    // Store entry
    entry.psp_addr = psp_addr;
    entry.content_hash = content_hash;
    entry.width = width;
    entry.height = height;
    entry.format = fmt;
    entry.last_frame = frame_num;
    entry.valid = true;

    return entry.gl_tex;
}
