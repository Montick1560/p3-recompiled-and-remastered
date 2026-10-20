#pragma once
// Texture patch seam (GE thread). A registered patcher recognises a guest texture by its content and
// redraws parts of it — on the decoded original or on the HD-pack image — for a *variant* (e.g. the
// player's input device) that it reports once per display list. ge_texture_bind mixes the variant
// into the texture's cache hash, so each variant is decoded and composed once (on a cache miss) and
// stays cached; a variant change mid-frame is not seen until the next list (no half-updated frame).
// Patchers are registered by game modules (psp_game_module.h) before the game boots.
#include "psp_ge_texdecode.h"
#include "psp_rgba_image.h"

#include <cstdint>

struct GeTexPatcher {
    // Cheap pre-filter on the decode parameters (size, format): false = never this texture.
    bool (*candidate)(const GeTexDecodeParams& p);
    // The decoded original (RGBA8, p.width x p.height): is this the texture? Memoised per content hash.
    bool (*recognise)(const GeTexDecodeParams& p, const uint8_t* rgba);
    // The variant to draw now (0 = leave the texture alone). Read once per display list.
    uint32_t (*variant)();
    // Redraw `img` (the original, or a larger pack image: scale = img.w / p.width) for `variant`.
    void (*apply)(uint32_t variant, const GeTexDecodeParams& p, RgbaImage& img);
};

// At most 4 patchers; a patcher variant must fit in 28 bits.
void ge_texpatch_register(const GeTexPatcher& p);

// GE thread, at the start of every display list: latch each patcher's variant.
void ge_texpatch_begin_list();

// GE thread, every texture bind (after its content hash): the latched variant of the patcher that
// recognises this texture, tagged with the patcher in the top 4 bits; 0 = not patched. The original
// is decoded at most once per new content hash (memo of 512 hashes, cleared when full).
uint32_t ge_texpatch_variant(const uint8_t* rdram, const GeTexDecodeParams& p, uint32_t content_hash);

// Redraw `img` for a value ge_texpatch_variant() returned.
void ge_texpatch_apply(uint32_t variant, const GeTexDecodeParams& p, RgbaImage& img);

// Scale of the composed texture when no pack image replaces it: the internal render scale clamped to
// 1..4 (set by ge_draw_init), so patched cells stay sharp at high resolutions.
void ge_texpatch_set_scale(int render_scale);
int ge_texpatch_scale();

// True when any patcher is registered (the bind path skips the seam entirely otherwise).
bool ge_texpatch_active();
