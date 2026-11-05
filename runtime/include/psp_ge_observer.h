#pragma once
// A game module's view of the GE stream as it is drawn (GL thread): every non-clear PRIM after its vertices are
// transformed, with its screen box and writable vertex colours, and the end of each presented frame. Used by
// per-game overlays that follow game objects (Patapon 3: the ping under each online hero). Unset = no cost beyond
// one pointer test per PRIM.
#include <cstddef>
#include <cstdint>

#include "psp_osk_overlay.h"  // WorldTag

struct GePrimInfo {
    int prim = 0;          // GE primitive type (4 = triangle strip, 6 = sprites)
    int count = 0;         // vertices of the PRIM command
    uint32_t vtype = 0;    // vertex type word (24 bits)
    bool texture = false;  // texture mapping enabled
    uint32_t tex_addr = 0, tex_size = 0, tex_fmt = 0, clut_addr = 0;  // level 0 (24-bit addresses)
    float world_x = 0.0f, world_y = 0.0f;  // translation row of the world matrix (transform mode)
};

struct GePrimBox {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // PSP pixels (480x272)
};

// rgba: the first drawn vertex's colour (4 bytes), then every `stride` bytes for n vertices; writable.
// Return false to drop the PRIM.
using GePrimObserver = bool (*)(const GePrimInfo& p, const GePrimBox& box, uint8_t* rgba, size_t n, size_t stride);
using GeFrameObserver = void (*)(const uint8_t* rdram);  // once per presented frame, before the overlays draw

void psp_ge_set_observers(GePrimObserver prim, GeFrameObserver frame);

// From a GePrimObserver: draws `t` into the current render target. on_top = false: now, after the PRIMs so far
// (the observed one included) and under the ones still to come, so game windows drawn later cover it.
// on_top = true: at the end of this display list, over everything it draws (sprites drawn after the anchor).
void psp_ge_draw_tag(const WorldTag& t, bool on_top);
