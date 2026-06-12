#pragma once
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include <cstdint>
#include <vector>

/// Decoded vertex in a uniform format for GL upload.
/// All PSP vertex formats are converted to this before rendering.
struct DecodedVertex {
    float pos[3];       // Position (x, y, z)
    float uv[2];        // Texture coordinates (u, v)
    uint8_t color[4];   // RGBA color
    float normal[3];    // Normal vector
    bool has_uv;
    bool has_color;
    bool has_normal;
};

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
