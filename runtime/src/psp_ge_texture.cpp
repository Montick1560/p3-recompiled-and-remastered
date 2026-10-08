#include "psp_ge_texture.h"
#include "psp_ge.h"
#include "psp_ge_texdecode.h"
#include "psp_ge_constants.h"
#include "psp_memory.h"
#include "recomp.h"

#include <glad/glad.h>
#include <array>
#include <vector>
#include <cstdio>
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

// ---- Public API ----

void ge_texture_init() {
    for (auto& e : g_tex_cache) {
        e = TexCacheEntry{};
    }
    g_tex_initialized = true;
    std::fprintf(stderr, "[TEX] Texture cache initialized "
                         "(%d entries)\n", TEX_CACHE_SIZE);
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
    uint32_t frame_num
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
        return entry.gl_tex;
    }

    // Cache miss -- decode texture (stride, swizzle and CLUT transform in
    // psp_ge_texdecode.cpp). Unsupported formats: magenta placeholder.
    std::vector<uint8_t> rgba;
    if (!ge_decode_texture(rdram, params, rgba)) {
        for (int i = 0; i < width * height; i++) {
            rgba[i * 4 + 0] = 0xFF;
            rgba[i * 4 + 1] = 0x00;
            rgba[i * 4 + 2] = 0xFF;
            rgba[i * 4 + 3] = 0xFF;
        }
    }

    // Upload to GL
    if (entry.gl_tex == 0) {
        glGenTextures(1, &entry.gl_tex);
    }
    glBindTexture(GL_TEXTURE_2D, entry.gl_tex);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8,
        width, height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE,
        rgba.data());
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
        GL_NEAREST);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
        GL_NEAREST);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
        GL_CLAMP_TO_EDGE);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
        GL_CLAMP_TO_EDGE);

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
