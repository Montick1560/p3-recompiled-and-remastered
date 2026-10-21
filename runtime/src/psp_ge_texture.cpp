#include "psp_ge_texture.h"
#include "psp_ge_texcache.h"
#include "psp_gl_state.h"
#include "psp_perf.h"
#include "psp_ge.h"
#include "psp_ge_sampler.h"
#include "psp_ge_texdecode.h"
#include "psp_ge_texmemo.h"
#include "psp_ge_texpatch.h"
#include "psp_prompt_raster.h"
#include "psp_ge_constants.h"
#include "psp_memory.h"
#include "psp_texrep.h"
#include "psp_texrep_lru.h"
#include "psp_texrep_share.h"
#include "recomp.h"

#include <glad/glad.h>
#include "xxhash.h"
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

// ---- Module state ----
static std::array<TexCacheEntry, TEX_CACHE_SIZE> g_tex_cache;
static bool g_tex_initialized = false;
static TexHashMemo g_hash_memo;  // content-hash memo w/ backoff (psp_ge_texmemo.h)
// Byte budget for uploaded GL textures, shared value with the PNG cache
// (PSPRECOMP_TEXTURES_BUDGET_MB). Render-thread only: eviction does GL deletes.
static TexrepLru<int> g_gl_lru(256u << 20);
// A texture bound within this many display lists counts as **in use**: the
// budget spares it even while over, because evicting a live texture makes the
// pack path re-decode its PNG (the original is drawn until the worker
// finishes) — the HD flicker the hard LRU produced when the pack's working
// set exceeds the budget (measured: mission 1 decodes ~957 MB of 4096x4096
// atlases against the 256 MB budget; the hard LRU churned ~50 re-decodes/s).
// 0 disables the protection (pure LRU); PSPRECOMP_TEX_HOT_FRAMES overrides
// the default 300 (10 s at 30 fps; frame_num counts display lists, one per
// frame in practice).
static uint32_t g_tex_hot_frames = 300;
// One live GL texture per pack file, shared by the slots that resolve to it
// (psp_texrep_share.h). Render-thread only, like the cache.
static TexrepShare g_rep_share;

// ---- Hashes ----

// Texture bytes are hashed with XXH3 (PPSSPP's texture-cache hash,
// TextureCacheCommon.cpp): ~25x faster than byte-wise FNV-1a, which made
// re-hashing every list cost ~0.4 ms/frame in the hideout. FNV stays for the
// 24-byte decode-parameter key.
static uint32_t content_hash32(const uint8_t* data, size_t len) {
    return static_cast<uint32_t>(XXH3_64bits(data, len));
}

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

/// Account `bytes` for `slot` in the GL budget (0 for a texture another slot
/// already accounts).
static void account_slot(int slot, size_t bytes) {
    g_tex_cache[slot].gl_bytes = bytes;
    g_gl_lru.put(slot, bytes);
}

/// Drop `e`'s GL texture: a pack texture other slots still share stays alive
/// (psp_texrep_share.h); anything else is deleted. Render thread only.
static void release_gl_texture(TexCacheEntry& e) {
    if (e.gl_tex != 0 && (e.rep_path.empty() || g_rep_share.drop(e.rep_path))) {
        gl_state_note_texture_deleted(e.gl_tex);
        glDeleteTextures(1, &e.gl_tex);
    }
    e.gl_tex = 0;
    e.rep_path.clear();
    e.rep_owner = false;
}

/// Delete the GL texture of `slot` and drop its byte accounting (LRU victim).
/// A shared pack texture that outlives the slot keeps its bytes in the budget:
/// the accounting moves to another slot that shows it. Render thread only.
static void evict_slot(int slot) {
    TexCacheEntry& e = g_tex_cache[slot];
    if (e.rep_owner && g_rep_share.refs(e.rep_path) > 1) {
        for (int i = 0; i < TEX_CACHE_SIZE; i++) {
            TexCacheEntry& o = g_tex_cache[i];
            if (i != slot && o.valid && o.gl_tex == e.gl_tex) {
                o.rep_owner = true;
                account_slot(i, e.gl_bytes);
                break;
            }
        }
    }
    release_gl_texture(e);
    e.valid = false;
    e.gl_bytes = 0;
    g_gl_lru.remove(slot);
}

/// Trim the GL texture budget, sparing `keep_slot` (in use right now) and any
/// texture bound within the last g_tex_hot_frames display lists (see above).
/// Render thread only.
static void trim_gl_budget(uint32_t frame_num, int keep_slot) {
    auto cold = [frame_num](int slot) {
        return g_tex_hot_frames == 0
            || frame_num - g_tex_cache[slot].last_frame > g_tex_hot_frames;
    };
    for (int victim : g_gl_lru.trim_if({keep_slot}, cold)) evict_slot(victim);
}

/// Find cache entry matching (addr, hash) or evict LRU. Evictions leave vacant
/// slots behind, so a match is honoured from any slot before a vacancy is
/// reused; the selection itself is the pure, unit-tested ge_texcache_pick().
static int find_or_evict(
    uint32_t psp_addr, uint32_t content_hash
) {
    int slot = 0;
    if (ge_texcache_pick(g_tex_cache, psp_addr, content_hash, &slot)
        == GeTexCachePick::Evict) {
        evict_slot(slot);
    }
    return slot;
}

/// When the pack file this texture resolves to is already on the GPU for
/// another slot, show that texture in `slot` (no decode, no upload) and
/// return true. Drops the slot's own original-texture object, if any.
static bool try_share(int slot, const uint8_t* rdram, const GeState& state,
                      uint16_t draw_max_v) {
    std::string path;
    TexrepFilter filter = TexrepFilter::None;
    if (!ge_texrep_enabled() || !ge_texrep_resolve(rdram, state, draw_max_v, &path, &filter)) {
        return false;
    }
    const uint32_t tex = g_rep_share.acquire(path);
    if (tex == 0) return false;
    TexCacheEntry& e = g_tex_cache[slot];
    release_gl_texture(e);
    e.gl_tex = tex;
    e.rep_path = path;
    e.forced_filter = static_cast<int>(filter);
    e.replace_pending = false;
    account_slot(slot, 0);  // the owner slot accounts the shared bytes
    return true;
}

// ---- Texture patches (psp_ge_texpatch.h) ----

/// The image a patched texture uploads: the pack image when it is ready,
/// else the decoded original enlarged to ge_texpatch_scale() (nearest or
/// bilinear like the draw's magnification filter, so it looks the same),
/// redrawn by its patcher.
static RgbaImage build_patched(const uint8_t* rdram, const GeTexDecodeParams& params,
                               const GeState& state, uint32_t patch, const GeTexrepImage* rep) {
    RgbaImage img;
    if (rep != nullptr) {
        img.w = rep->w;
        img.h = rep->h;
        img.px.assign(rep->rgba, rep->rgba + static_cast<size_t>(rep->w) * rep->h * 4);
    } else {
        RgbaImage orig;
        orig.w = params.width;
        orig.h = params.height;
        if (!ge_decode_texture(rdram, params, orig.px)) {
            orig.px.assign(static_cast<size_t>(orig.w) * orig.h * 4, 0);
        }
        const int k = ge_texpatch_scale();
        static const GeFilterOverride ov = ge_filter_override_parse(std::getenv("PSPRECOMP_TEX_FILTER"));
        if (k == 1) {
            img = std::move(orig);
        } else if (ge_sampler_from_regs(state.tex_filter, state.tex_wrap, ov).mag_linear) {
            img = rgba_resample(orig, orig.w * k, orig.h * k);
        } else {
            img.w = orig.w * k;
            img.h = orig.h * k;
            img.px.resize(static_cast<size_t>(img.w) * img.h * 4);
            for (int y = 0; y < img.h; y++)
                for (int x = 0; x < img.w; x++)
                    std::memcpy(&img.px[(static_cast<size_t>(y) * img.w + x) * 4],
                                &orig.px[(static_cast<size_t>(y / k) * orig.w + x / k) * 4], 4);
        }
    }
    ge_texpatch_apply(patch, params, img);
    return img;
}

// ---- Sampler ----

/// Set the GL_TEXTURE_2D sampler from the draw's TEXFILTER / TEXWRAP. The same
/// cached texture can be drawn with different registers, so this runs on every
/// bind; the shadow skips it while the same texture keeps the same values.
/// PSPRECOMP_TEX_FILTER=nearest|linear overrides the filter bits; wrap is
/// always taken from the registers. Returns true when the parameters were
/// emitted (the caller's bind/work accounting).
static bool apply_sampler(const GeState& state, int forced_filter) {
    static const GeFilterOverride ov = ge_filter_override_parse(
        std::getenv("PSPRECOMP_TEX_FILTER"));
    const GeSampler s =
        ge_sampler_from_regs(state.tex_filter, state.tex_wrap, ov);
    GlShadow::Sampler sm;
    if (forced_filter != 0) {  // texture pack [filtering]: 1 nearest, 2 linear
        const GLint f = forced_filter == 1 ? GL_NEAREST : GL_LINEAR;
        sm.min = static_cast<uint32_t>(f);
        sm.mag = static_cast<uint32_t>(f);
    } else {
        sm.min = s.min_linear ? GL_LINEAR : GL_NEAREST;
        sm.mag = s.mag_linear ? GL_LINEAR : GL_NEAREST;
    }
    sm.wrap_s = s.clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    sm.wrap_t = s.clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    return gl_state_apply_sampler(sm);
}

// ---- Public API ----

// PSPRECOMP_TEX_HASH_BACKOFF=<lists>: the memo's max reuse window. Default 1
// verifies every list, like PPSSPP (it rehashes each texture once per sync
// domain): with no write tracking, a longer window can draw a stale texture
// for up to that many lists after the CPU rewrites it. Opt-in for slow hosts.
static uint32_t tex_hash_backoff_window() {
    const char* e = std::getenv("PSPRECOMP_TEX_HASH_BACKOFF");
    if (e == nullptr || *e == '\0') return 1;
    const unsigned long v = std::strtoul(e, nullptr, 10);
    if (v == 0) return 1;
    return static_cast<uint32_t>(v > 65536u ? 65536u : v);
}

void ge_texture_init() {
    for (auto& e : g_tex_cache) {
        e = TexCacheEntry{};
    }
    g_tex_initialized = true;
    g_hash_memo = TexHashMemo(tex_hash_backoff_window());
    // PSPRECOMP_TEX_HOT_FRAMES=<lists>: "in use" window of the GL budget
    // trim. 0 = pure LRU. A value with no digits keeps the default.
    g_tex_hot_frames = 300;
    if (const char* e = std::getenv("PSPRECOMP_TEX_HOT_FRAMES")) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(e, &end, 10);
        if (end != e) g_tex_hot_frames = static_cast<uint32_t>(v > 100000ul ? 100000ul : v);
    }
    std::fprintf(stderr, "[TEX] Texture cache initialized "
                         "(%d entries)\n", TEX_CACHE_SIZE);
    ge_texrep_init();
    g_rep_share.clear();
    g_gl_lru.clear();
    g_gl_lru.set_budget(ge_texrep_budget_bytes());
}

void ge_texture_shutdown() {
    for (auto& e : g_tex_cache) {
        release_gl_texture(e);
        e.valid = false;
        e.gl_bytes = 0;
    }
    g_rep_share.clear();
    g_gl_lru.clear();
    g_tex_initialized = false;
    gl_state_invalidate();
    std::fprintf(stderr, "[TEX] Texture cache shutdown\n");
}

void ge_texture_memo_invalidate() { g_hash_memo.invalidate(); }

void ge_texture_invalidate_all() {
    g_hash_memo.invalidate();
    for (auto& e : g_tex_cache) {
        release_gl_texture(e);
        e.valid = false;
        e.gl_bytes = 0;
    }
    g_rep_share.clear();
    g_gl_lru.clear();
    gl_state_invalidate();
}

GLuint ge_texture_bind(
    uint8_t* rdram,
    uint32_t frame_num,
    uint16_t draw_max_v
) {
    const GeState& state = ge_get_state();

    if (!state.texture_enable) {
        gl_state_bind_texture(0);
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
        gl_state_bind_texture(0);
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
    const uint32_t param_words[6] = {
        static_cast<uint32_t>(params.bufw), static_cast<uint32_t>(fmt),
        params.swizzle ? 1u : 0u, state.clut_format, params.clut_addr,
        static_cast<uint32_t>(width | (height << 16)),
    };
    const uint32_t params_hash = fnv1a_hash(reinterpret_cast<const uint8_t*>(param_words),
                                            static_cast<int>(sizeof(param_words)));
    // The bytes are hashed once per display list (frame_num counts lists);
    // later binds of the same texture in the list reuse that hash. With
    // PSPRECOMP_TEX_HASH_BACKOFF > 1 a texture whose hash verified unchanged
    // is only re-read every few lists (psp_ge_texmemo.h).
    uint32_t content_hash = 0;
    if (!g_hash_memo.find(frame_num, masked_addr, params_hash, &content_hash)) {
        content_hash = content_hash32(rdram + masked_addr, span);
        content_hash ^= params_hash * 0x9E3779B1u;
        if (perf_enabled()) g_perf.tex_hash_bytes.fetch_add(span, std::memory_order_relaxed);
        if (fmt >= GE_TFMT_CLUT4 && fmt <= GE_TFMT_CLUT32) {
            const uint32_t clut_off = params.clut_addr & PSP_ADDR_MASK;
            if (clut_off + 1024 <= static_cast<uint32_t>(PSP_MEM_SIZE)) {
                content_hash ^= content_hash32(rdram + clut_off, 1024) * 0x85EBCA6Bu;
                if (perf_enabled()) g_perf.tex_hash_bytes.fetch_add(1024, std::memory_order_relaxed);
            }
        }
        g_hash_memo.put(frame_num, masked_addr, params_hash, content_hash);
    }
    // A patched texture (psp_ge_texpatch.h) is cached per variant: its own
    // private GL texture, never shared under the pack file's path.
    uint32_t patch = 0;
    if (ge_texpatch_active()) {
        patch = ge_texpatch_variant(rdram, params, content_hash);
        if (patch != 0) content_hash ^= patch * 0x27D4EB2Fu;
    }

    // Cache lookup
    int slot = find_or_evict(psp_addr, content_hash);
    auto& entry = g_tex_cache[slot];

    if (entry.valid
        && entry.psp_addr == psp_addr
        && entry.content_hash == content_hash) {
        // Cache hit
        entry.last_frame = frame_num;
        g_gl_lru.touch(slot);
        if (entry.replace_pending && ge_texrep_epoch() != entry.replace_epoch
            && (patch != 0 || !try_share(slot, rdram, state, draw_max_v))) {
            // A pack image finished decoding since the last try: retry.
            entry.replace_epoch = ge_texrep_epoch();
            GeTexrepImage rep;
            const GeTexrepResult r = ge_texrep_replacement(rdram, state, draw_max_v, &rep);
            if (r == GeTexrepResult::Ready && patch != 0) {
                const RgbaImage img = build_patched(rdram, params, state, patch, &rep);
                gl_state_bind_texture(entry.gl_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.w, img.h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, img.px.data());
                entry.forced_filter = static_cast<int>(rep.filter);
                ge_texrep_image_uploaded(rep);
                account_slot(slot, static_cast<size_t>(img.w) * img.h * 4);
                trim_gl_budget(frame_num, slot);
            } else if (r == GeTexrepResult::Ready) {
                gl_state_bind_texture(entry.gl_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rep.w, rep.h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, rep.rgba);
                entry.forced_filter = static_cast<int>(rep.filter);
                ge_texrep_image_uploaded(rep);
                g_rep_share.adopt(rep.path, entry.gl_tex);
                entry.rep_path = rep.path;
                entry.rep_owner = true;
                // The replacement changed the texture size: re-account and
                // evict other textures if the budget is now over.
                account_slot(slot, static_cast<size_t>(rep.w) * rep.h * 4);
                trim_gl_budget(frame_num, slot);
            }
            entry.replace_pending = r == GeTexrepResult::Pending;
        }
        const bool bound = gl_state_bind_texture(entry.gl_tex);
        const bool sampled = apply_sampler(state, entry.forced_filter);
        // Perf: count the binds that reached the driver (the shadow skips
        // repeats), not the GE's bind requests.
        if (perf_enabled() && (bound || sampled)) {
            g_perf.tex_binds.fetch_add(1, std::memory_order_relaxed);
        }
        return entry.gl_tex;
    }

    // Cache miss. Texture replacement (PSPRECOMP_TEXTURES): a pack PNG
    // replaces the decoded texture at its own size; UVs are normalised, so
    // nothing else changes. Otherwise decode from guest RAM (stride, swizzle
    // and CLUT transform in psp_ge_texdecode.cpp; unsupported formats:
    // magenta placeholder).
    // A pack file already on the GPU for another slot is shared as is.
    if (perf_enabled()) g_perf.tex_misses.fetch_add(1, std::memory_order_relaxed);
    PerfScope miss_scope(g_perf.tex_miss_ns);  // decode + upload of this miss
    const bool shared = patch == 0 && try_share(slot, rdram, state, draw_max_v);
    GeTexrepImage rep;
    GeTexrepResult r = GeTexrepResult::None;
    if (!shared) {
        const uint64_t epoch = ge_texrep_epoch();
        r = ge_texrep_enabled()
            ? ge_texrep_replacement(rdram, state, draw_max_v, &rep) : GeTexrepResult::None;
        entry.replace_pending = r == GeTexrepResult::Pending;
        entry.replace_epoch = epoch;
    }
    const bool replaced = r == GeTexrepResult::Ready;
    if (entry.gl_tex == 0) {
        glGenTextures(1, &entry.gl_tex);
    }
    const bool bound = gl_state_bind_texture(entry.gl_tex);
    size_t patched_bytes = 0;
    if (shared) {
        // Already uploaded; forced_filter came with the share.
    } else if (patch != 0) {
        // The slot may hold a pack texture other slots share: get a private one.
        if (!entry.rep_path.empty()) {
            release_gl_texture(entry);
            glGenTextures(1, &entry.gl_tex);
            gl_state_bind_texture(entry.gl_tex);
        }
        const RgbaImage img = build_patched(rdram, params, state, patch, replaced ? &rep : nullptr);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.w, img.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.px.data());
        if (replaced) ge_texrep_image_uploaded(rep);
        patched_bytes = static_cast<size_t>(img.w) * img.h * 4;
    } else if (replaced) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rep.w, rep.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rep.rgba);
        ge_texrep_image_uploaded(rep);
        g_rep_share.adopt(rep.path, entry.gl_tex);
        entry.rep_path = rep.path;
        entry.rep_owner = true;
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
    if (!shared) entry.forced_filter = replaced ? static_cast<int>(rep.filter) : 0;
    const bool sampled = apply_sampler(state, entry.forced_filter);

    // Store entry
    entry.psp_addr = psp_addr;
    entry.content_hash = content_hash;
    entry.width = width;
    entry.height = height;
    entry.format = fmt;
    entry.last_frame = frame_num;
    entry.valid = true;

    // Account the uploaded RGBA8 and evict least-recently-used cold textures
    // while over budget (this slot is in use right now, so it is kept). A
    // shared pack texture is accounted once, by the slot that uploaded it.
    g_gl_lru.remove(slot);
    const size_t tex_bytes = patch != 0 ? patched_bytes : shared ? 0 : (replaced
        ? static_cast<size_t>(rep.w) * rep.h
        : static_cast<size_t>(width) * height) * 4;
    account_slot(slot, tex_bytes);
    trim_gl_budget(frame_num, slot);

    if (perf_enabled() && (bound || sampled)) {
        g_perf.tex_binds.fetch_add(1, std::memory_order_relaxed);
    }
    return entry.gl_tex;
}
