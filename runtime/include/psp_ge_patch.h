#pragma once

#include "psp_ge_vertex.h"

#include <vector>

/// Tessellate a GE BEZIER surface (GE_CMD_BEZIER) into a plain primitive list.
///
/// ctrl: count_u * count_v decoded control points, row-major (u fastest), in
/// model space (skinning/morph already applied by ge_decode_vertices).
/// Surfaces are made of cubic patches sharing edges: (count - 1) / 3 patches
/// per direction (PPSSPP SplineCommon). div_s / div_t come from
/// GE_CMD_PATCHDIVISION (clamped to 1..64); patch_prim from
/// GE_CMD_PATCHPRIMITIVE (0 triangles, 1 lines, 2 points). Every attribute is
/// interpolated with the Bernstein weights; a surface without texture
/// coordinates gets the patch parameters as UVs (PPSSPP). flip_facing
/// (GE_CMD_PATCHFACING) reverses the triangle winding.
///
/// Returns false (out empty) for a surface with fewer than 4 points per
/// direction or with fewer control points than count_u * count_v.
/// prim_out receives the GE primitive the output is meant to be drawn as.
bool ge_tessellate_bezier(const std::vector<DecodedVertex>& ctrl, int count_u, int count_v,
                          int div_s, int div_t, int patch_prim, bool flip_facing,
                          int& prim_out, std::vector<DecodedVertex>& out);
