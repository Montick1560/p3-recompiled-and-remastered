// GE BEZIER surface tessellation (see psp_ge_patch.h). Oracle: PPSSPP
// GPU/Common/SplineCommon.cpp. Patapon 3 draws its weather wind gusts as
// bezier strips; without this the wind never showed.

#include "psp_ge_patch.h"
#include "psp_ge_constants.h"

#include <algorithm>
#include <cmath>

namespace {

void bernstein(float t, float b[4]) {
    const float s = 1.0f - t;
    b[0] = s * s * s;
    b[1] = 3.0f * t * s * s;
    b[2] = 3.0f * t * t * s;
    b[3] = t * t * t;
}

DecodedVertex eval_patch(const std::vector<DecodedVertex>& ctrl, int count_u, int pu, int pv,
                         float u, float v, int npu, int npv) {
    float bu[4], bv[4];
    bernstein(u, bu);
    bernstein(v, bv);
    const DecodedVertex& first = ctrl[static_cast<size_t>(pv * 3) * count_u + pu * 3];
    DecodedVertex out{};
    out.has_uv = true;  // generated below when the surface has none
    out.has_color = first.has_color;
    out.has_normal = first.has_normal;
    float col[4] = {0, 0, 0, 0};
    float uv[2] = {0, 0};
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            const DecodedVertex& c = ctrl[static_cast<size_t>(pv * 3 + j) * count_u + pu * 3 + i];
            const float w = bu[i] * bv[j];
            for (int k = 0; k < 3; k++) out.pos[k] += w * c.pos[k];
            for (int k = 0; k < 3; k++) out.normal[k] += w * c.normal[k];
            for (int k = 0; k < 4; k++) col[k] += w * c.color[k];
            uv[0] += w * c.uv[0];
            uv[1] += w * c.uv[1];
        }
    }
    for (int k = 0; k < 4; k++) {
        out.color[k] = static_cast<uint8_t>(std::clamp(std::lround(col[k]), 0L, 255L));
    }
    if (first.has_uv) {
        out.uv[0] = uv[0];
        out.uv[1] = uv[1];
    } else {
        // PPSSPP: surfaces without texture coordinates use the patch
        // parameters, spanning 0..1 across the whole surface.
        out.uv[0] = (static_cast<float>(pu) + u) / static_cast<float>(npu);
        out.uv[1] = (static_cast<float>(pv) + v) / static_cast<float>(npv);
    }
    return out;
}

}  // namespace

bool ge_tessellate_bezier(const std::vector<DecodedVertex>& ctrl, int count_u, int count_v,
                          int div_s, int div_t, int patch_prim, bool flip_facing,
                          int& prim_out, std::vector<DecodedVertex>& out) {
    out.clear();
    if (count_u < 4 || count_v < 4) return false;
    if (ctrl.size() < static_cast<size_t>(count_u) * static_cast<size_t>(count_v)) return false;
    const int npu = (count_u - 1) / 3;
    const int npv = (count_v - 1) / 3;
    const int ds = std::clamp(div_s, 1, 64);
    const int dt = std::clamp(div_t, 1, 64);

    for (int pv = 0; pv < npv; pv++) {
        for (int pu = 0; pu < npu; pu++) {
            // (ds+1) x (dt+1) grid of evaluated vertices for this patch.
            std::vector<DecodedVertex> grid;
            grid.reserve(static_cast<size_t>(ds + 1) * (dt + 1));
            for (int t = 0; t <= dt; t++) {
                for (int s = 0; s <= ds; s++) {
                    grid.push_back(eval_patch(ctrl, count_u, pu, pv,
                                              static_cast<float>(s) / ds,
                                              static_cast<float>(t) / dt, npu, npv));
                }
            }
            auto at = [&](int s, int t) -> const DecodedVertex& {
                return grid[static_cast<size_t>(t) * (ds + 1) + s];
            };
            if (patch_prim == 2) {
                out.insert(out.end(), grid.begin(), grid.end());
            } else if (patch_prim == 1) {
                for (int t = 0; t <= dt; t++) {
                    for (int s = 0; s <= ds; s++) {
                        if (s < ds) { out.push_back(at(s, t)); out.push_back(at(s + 1, t)); }
                        if (t < dt) { out.push_back(at(s, t)); out.push_back(at(s, t + 1)); }
                    }
                }
            } else {
                for (int t = 0; t < dt; t++) {
                    for (int s = 0; s < ds; s++) {
                        const DecodedVertex& a = at(s, t);
                        const DecodedVertex& b = at(s + 1, t);
                        const DecodedVertex& c = at(s, t + 1);
                        const DecodedVertex& d = at(s + 1, t + 1);
                        if (!flip_facing) {
                            out.push_back(a); out.push_back(c); out.push_back(b);
                            out.push_back(b); out.push_back(c); out.push_back(d);
                        } else {
                            out.push_back(a); out.push_back(b); out.push_back(c);
                            out.push_back(b); out.push_back(d); out.push_back(c);
                        }
                    }
                }
            }
        }
    }
    prim_out = patch_prim == 2 ? GE_PRIM_POINTS : patch_prim == 1 ? GE_PRIM_LINES : GE_PRIM_TRIANGLES;
    return !out.empty();
}
