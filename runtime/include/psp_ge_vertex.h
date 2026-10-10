#pragma once
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include <cstdint>
#include <vector>

/// Decoded vertex in a uniform format for GL upload.
/// All PSP vertex formats are converted to this before rendering.
struct DecodedVertex {
    float pos[3];       // Position (x, y, z)
    float w = 1.0f;     // Clip-space w (1.0 = already NDC/screen-space)
    float uv[2];        // Texture coordinates (u, v)
    uint8_t color[4];   // RGBA color
    float normal[3];    // Normal vector
    bool has_uv;
    bool has_color;
    bool has_normal;
};

/// Byte offsets of each vertex component (-1 = absent) and the stride.
struct GeVertexLayout {
    int weights = -1;
    int tc = -1;
    int col = -1;
    int nrm = -1;
    int pos = -1;
    int one_vertex = 0;  // size of one morph target
    int stride = 0;      // one_vertex * morph count
};
GeVertexLayout ge_vertex_layout(uint32_t vtype);

/// Compute the byte stride of one PSP vertex based on VTYPE bitfield.
int ge_vertex_stride(uint32_t vtype);

/// Decode `count` vertices from rdram at state.vertex_addr using
/// the vertex format described by state.vertex_type. If index bits
/// are set in VTYPE, reads the index buffer from state.index_addr.
void ge_decode_vertices(
    uint8_t* rdram,
    const GeState& state,
    int prim_type,
    int count,
    std::vector<DecodedVertex>& out
);

/// Apply world*view*proj matrix to positions and tex scale/offset
/// to UVs. Through-mode vertices are mapped to NDC directly.
void ge_transform_vertices(
    std::vector<DecodedVertex>& verts,
    const GeState& state
);

/// Through-mode x/y (PSP pixels, before ge_transform_vertices) -> the pixel
/// edge with the same 1x coverage: ceil(x - 0.5). Used at render scale > 1
/// so 2D pieces that touch at 1x (one ending at x = 328, the next starting
/// at 328.196) still touch when each PSP pixel is N FBO pixels wide.
void ge_snap_through_positions(std::vector<DecodedVertex>& verts);

/// PPSSPP vertex range culling: true when the clip-space position (x,y,z,w)
/// lands outside the GE's [0, 4096) drawing space after the viewport
/// transform, or has w < -1, and is not z-clipped (z < -w). The hardware
/// drops every primitive using such a vertex.
bool ge_vertex_range_culled(const GeState& state, const float clip[4]);

/// Rectangle (GE_PRIM_RECTANGLES) expansion: perspective-divide the two
/// clip-space corner vertices and emit the 6 vertices (two triangles) of
/// the screen-aligned quad, each with w = 1 (NDC). A corner with w <= 0
/// lies behind the eye plane and has no screen position, so the whole
/// rectangle is dropped (returns false). Pure: no GL, no global state.
bool ge_expand_rectangle(
    const DecodedVertex& v0,
    const DecodedVertex& v1,
    DecodedVertex out[6]
);

// ---- Game-module degenerate-matrix fallback (issue #47 Phase 5 seam) ----
// When the guest uploads broken matrices (view all-zero; proj NaN/Inf or
// collapsed diagonal — open issue, FPU/VFPU dataflow family), the real
// transform path cannot work. The mapping that produces a usable frame
// anyway is GAME-TUNED (it depends on the title's intended projection),
// so it installs from the game module. Generic default: world-space
// passthrough (positions used as NDC unchanged) plus the one-time warns.
// Signature: world-space position in, NDC out.
using GeDegenerateFallbackFn = void (*)(const float wpos[3], float out[3]);
void ge_vertex_set_degenerate_fallback(GeDegenerateFallbackFn fn);
