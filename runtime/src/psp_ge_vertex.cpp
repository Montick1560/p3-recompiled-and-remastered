#include "psp_ge_vertex.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include "psp_ge_lighting.h"
#include "recomp.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <algorithm>

// ---- Matrix helpers (PSP column-major layout) ----
//
// The GE uploads matrices COLUMN-major: world/view are
// 4 columns x 3 rows (translation at flat indices 9,10,11);
// proj is 4 columns x 4 rows. These match PPSSPP's
// Vec3ByMatrix43 / Vec3ByMatrix44.

/// Transform (x,y,z,1) by a 4x3 column-major matrix.
static void vec3_by_matrix43(
    const float m[12], const float v[3], float out[3]
) {
    out[0] = v[0] * m[0] + v[1] * m[3]
           + v[2] * m[6] + m[9];
    out[1] = v[0] * m[1] + v[1] * m[4]
           + v[2] * m[7] + m[10];
    out[2] = v[0] * m[2] + v[1] * m[5]
           + v[2] * m[8] + m[11];
}

/// Transform (x,y,z,1) by a 4x4 column-major matrix.
static void vec3_by_matrix44(
    const float m[16], const float v[3], float out[4]
) {
    out[0] = v[0] * m[0] + v[1] * m[4]
           + v[2] * m[8] + m[12];
    out[1] = v[0] * m[1] + v[1] * m[5]
           + v[2] * m[9] + m[13];
    out[2] = v[0] * m[2] + v[1] * m[6]
           + v[2] * m[10] + m[14];
    out[3] = v[0] * m[3] + v[1] * m[7]
           + v[2] * m[11] + m[15];
}

// ---- Vertex stride calculation ----

// PSP vertex layout (PPSSPP VertexDecoder::SetVertexType): weights,
// texcoord, color, normal, position, each aligned to its own element size;
// the vertex size is rounded up to the largest alignment. Morph targets
// repeat the whole vertex morphcount times.
GeVertexLayout ge_vertex_layout(uint32_t vtype) {
    GeVertexLayout L;
    int size = 0;
    int biggest = 1;
    auto place = [&](int align, int bytes) {
        size = (size + align - 1) & ~(align - 1);
        const int off = size;
        size += bytes;
        if (align > biggest) biggest = align;
        return off;
    };
    const int wt = ge_vtype_weight(vtype);
    if (wt != 0) {
        const int wcount = ((vtype >> GE_VTYPE_WEIGHTCOUNT_SHIFT)
                            & GE_VTYPE_WEIGHTCOUNT_MASK) + 1;
        const int wsize = wt == 1 ? 1 : (wt == 2 ? 2 : 4);
        L.weights = place(wsize, wsize * wcount);
    }
    switch (ge_vtype_tc(vtype)) {
    case GE_VTYPE_TC_8BIT:  L.tc = place(1, 2); break;
    case GE_VTYPE_TC_16BIT: L.tc = place(2, 4); break;
    case GE_VTYPE_TC_FLOAT: L.tc = place(4, 8); break;
    default: break;
    }
    switch (ge_vtype_col(vtype)) {
    case GE_VTYPE_COL_565:
    case GE_VTYPE_COL_5551:
    case GE_VTYPE_COL_4444: L.col = place(2, 2); break;
    case GE_VTYPE_COL_8888: L.col = place(4, 4); break;
    default: break;
    }
    switch (ge_vtype_nrm(vtype)) {
    case GE_VTYPE_NRM_8BIT:  L.nrm = place(1, 3); break;
    case GE_VTYPE_NRM_16BIT: L.nrm = place(2, 6); break;
    case GE_VTYPE_NRM_FLOAT: L.nrm = place(4, 12); break;
    default: break;
    }
    switch (ge_vtype_pos(vtype)) {
    case GE_VTYPE_POS_8BIT:  L.pos = place(1, 3); break;
    case GE_VTYPE_POS_16BIT: L.pos = place(2, 6); break;
    case GE_VTYPE_POS_FLOAT: L.pos = place(4, 12); break;
    default: break;
    }
    size = (size + biggest - 1) & ~(biggest - 1);
    L.one_vertex = size;
    const int morphs = static_cast<int>((vtype >> 18) & 7u) + 1;
    L.stride = size * morphs;
    return L;
}

int ge_vertex_stride(uint32_t vtype) {
    return ge_vertex_layout(vtype).stride;
}

// ---- Inline read helpers ----

static inline uint8_t read_u8(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint8_t>(rdram, addr);
}

static inline int8_t read_s8(
    uint8_t* rdram, uint32_t addr
) {
    return static_cast<int8_t>(
        psp_mem_read<uint8_t>(rdram, addr));
}

static inline uint16_t read_u16(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint16_t>(rdram, addr);
}

static inline int16_t read_s16(
    uint8_t* rdram, uint32_t addr
) {
    return static_cast<int16_t>(
        psp_mem_read<uint16_t>(rdram, addr));
}

static inline uint32_t read_u32(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint32_t>(rdram, addr);
}

static inline float read_float(
    uint8_t* rdram, uint32_t addr
) {
    uint32_t bits = psp_mem_read<uint32_t>(rdram, addr);
    float f;
    std::memcpy(&f, &bits, sizeof(float));
    return f;
}

// ---- Vertex decode ----

void ge_decode_vertices(
    uint8_t* rdram,
    const GeState& state,
    int prim_type,
    int count,
    std::vector<DecodedVertex>& out
) {
    uint32_t vtype = state.vertex_type;
    const GeVertexLayout layout = ge_vertex_layout(vtype);
    int stride = layout.stride;

    if (stride <= 0 || count <= 0) return;

    int tc_type  = ge_vtype_tc(vtype);
    int col_type = ge_vtype_col(vtype);
    int nrm_type = ge_vtype_nrm(vtype);
    int pos_type = ge_vtype_pos(vtype);
    int idx_type = ge_vtype_idx(vtype);

    // Read index buffer if indexed
    std::vector<uint32_t> indices;
    if (idx_type != GE_VTYPE_IDX_NONE) {
        indices.resize(count);
        uint32_t iaddr = state.index_addr;
        for (int i = 0; i < count; i++) {
            switch (idx_type) {
            case GE_VTYPE_IDX_8BIT:
                indices[i] = read_u8(rdram, iaddr);
                iaddr += 1;
                break;
            case GE_VTYPE_IDX_16BIT:
                indices[i] = read_u16(rdram, iaddr);
                iaddr += 2;
                break;
            case GE_VTYPE_IDX_32BIT:
                indices[i] = read_u32(rdram, iaddr);
                iaddr += 4;
                break;
            default:
                indices[i] = i;
                break;
            }
        }
    }

    const bool through = ge_vtype_through(vtype);
    const int morphs = static_cast<int>((vtype >> GE_VTYPE_MORPHCOUNT_SHIFT)
                                        & GE_VTYPE_MORPHCOUNT_MASK) + 1;
    const uint32_t one = static_cast<uint32_t>(layout.one_vertex);
    const int wt = ge_vtype_weight(vtype);
    const int wcount = wt != 0 ? static_cast<int>(((vtype >> GE_VTYPE_WEIGHTCOUNT_SHIFT)
                                                   & GE_VTYPE_WEIGHTCOUNT_MASK) + 1) : 0;
    const bool skin = wt != 0 && !through;

    // One morph target's attributes at target base address b.
    auto read_uv = [&](uint32_t b, float uv[2]) {
        const uint32_t o = b + static_cast<uint32_t>(layout.tc);
        if (tc_type == GE_VTYPE_TC_8BIT) {
            uv[0] = read_u8(rdram, o) / 128.0f;
            uv[1] = read_u8(rdram, o + 1) / 128.0f;
        } else if (tc_type == GE_VTYPE_TC_16BIT && through) {
            // Raw unsigned texels (PPSSPP Step_TcU16ThroughToFloat): the
            // P3 mission command list uses this and vanished when scaled.
            uv[0] = static_cast<float>(read_u16(rdram, o));
            uv[1] = static_cast<float>(read_u16(rdram, o + 2));
        } else if (tc_type == GE_VTYPE_TC_16BIT) {
            uv[0] = read_s16(rdram, o) / 32768.0f;
            uv[1] = read_s16(rdram, o + 2) / 32768.0f;
        } else if (tc_type == GE_VTYPE_TC_FLOAT) {
            uv[0] = read_float(rdram, o);
            uv[1] = read_float(rdram, o + 4);
        } else {
            uv[0] = uv[1] = 0.0f;
        }
    };
    auto read_color = [&](uint32_t b, float c[4]) {
        const uint32_t o = b + static_cast<uint32_t>(layout.col);
        if (col_type == GE_VTYPE_COL_565) {
            const uint16_t val = read_u16(rdram, o);
            c[0] = static_cast<float>(((val & 0x1F) * 255) / 31);
            c[1] = static_cast<float>((((val >> 5) & 0x3F) * 255) / 63);
            c[2] = static_cast<float>((((val >> 11) & 0x1F) * 255) / 31);
            c[3] = 255.0f;
        } else if (col_type == GE_VTYPE_COL_5551) {
            const uint16_t val = read_u16(rdram, o);
            c[0] = static_cast<float>(((val & 0x1F) * 255) / 31);
            c[1] = static_cast<float>((((val >> 5) & 0x1F) * 255) / 31);
            c[2] = static_cast<float>((((val >> 10) & 0x1F) * 255) / 31);
            c[3] = (val & 0x8000) ? 255.0f : 0.0f;
        } else if (col_type == GE_VTYPE_COL_4444) {
            const uint16_t val = read_u16(rdram, o);
            c[0] = static_cast<float>(((val & 0xF) * 255) / 15);
            c[1] = static_cast<float>((((val >> 4) & 0xF) * 255) / 15);
            c[2] = static_cast<float>((((val >> 8) & 0xF) * 255) / 15);
            c[3] = static_cast<float>((((val >> 12) & 0xF) * 255) / 15);
        } else {  // GE_VTYPE_COL_8888
            for (int k = 0; k < 4; k++) c[k] = read_u8(rdram, o + k);
        }
    };
    auto read_normal = [&](uint32_t b, float n[3]) {
        const uint32_t o = b + static_cast<uint32_t>(layout.nrm);
        if (nrm_type == GE_VTYPE_NRM_8BIT) {
            for (int k = 0; k < 3; k++) n[k] = read_s8(rdram, o + k) / 127.0f;
        } else if (nrm_type == GE_VTYPE_NRM_16BIT) {
            for (int k = 0; k < 3; k++) n[k] = read_s16(rdram, o + 2 * k) / 32767.0f;
        } else if (nrm_type == GE_VTYPE_NRM_FLOAT) {
            for (int k = 0; k < 3; k++) n[k] = read_float(rdram, o + 4 * k);
        } else {
            n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f;
        }
    };
    // Transform-mode 8/16-bit positions are fixed point (PPSSPP Step_PosS8 /
    // Step_PosS16: / 128, / 32768); through mode keeps raw screen units.
    auto read_pos = [&](uint32_t b, float p[3]) {
        const uint32_t o = b + static_cast<uint32_t>(layout.pos);
        if (pos_type == GE_VTYPE_POS_8BIT) {
            const float sc = through ? 1.0f : 1.0f / 128.0f;
            for (int k = 0; k < 3; k++) p[k] = read_s8(rdram, o + k) * sc;
        } else if (pos_type == GE_VTYPE_POS_16BIT) {
            const float sc = through ? 1.0f : 1.0f / 32768.0f;
            for (int k = 0; k < 3; k++) p[k] = read_s16(rdram, o + 2 * k) * sc;
            if (through) p[2] = static_cast<float>(read_u16(rdram, o + 4));  // z unsigned (PPSSPP)
        } else if (pos_type == GE_VTYPE_POS_FLOAT) {
            for (int k = 0; k < 3; k++) p[k] = read_float(rdram, o + 4 * k);
        } else {
            p[0] = p[1] = p[2] = 0.0f;
        }
    };

    out.resize(count);
    for (int i = 0; i < count; i++) {
        DecodedVertex& v = out[i];
        v.has_uv = (tc_type != GE_VTYPE_TC_NONE);
        v.has_color = (col_type != GE_VTYPE_COL_NONE);
        v.has_normal = (nrm_type != GE_VTYPE_NRM_NONE);

        // Determine vertex index
        uint32_t vidx = (idx_type != GE_VTYPE_IDX_NONE)
                         ? indices[i]
                         : static_cast<uint32_t>(i);

        uint32_t base = state.vertex_addr
                        + vidx * static_cast<uint32_t>(stride);

        // Attributes; with morph targets (PPSSPP Step_*Morph), each one is
        // the MORPHWEIGHT-weighted sum over the vertex's targets.
        float uv[2] = {0, 0}, col[4] = {0, 0, 0, 0}, nrm[3] = {0, 0, 0}, pos[3] = {0, 0, 0};
        if (morphs == 1) {
            read_uv(base, uv);
            if (v.has_color) read_color(base, col);
            read_normal(base, nrm);
            read_pos(base, pos);
        } else {
            for (int m = 0; m < morphs; m++) {
                const float w = state.morph_weights[m];
                const uint32_t b = base + static_cast<uint32_t>(m) * one;
                float tuv[2], tcol[4] = {0, 0, 0, 0}, tnrm[3], tpos[3];
                read_uv(b, tuv);
                if (v.has_color) read_color(b, tcol);
                read_normal(b, tnrm);
                read_pos(b, tpos);
                for (int k = 0; k < 2; k++) uv[k] += tuv[k] * w;
                for (int k = 0; k < 4; k++) col[k] += tcol[k] * w;
                for (int k = 0; k < 3; k++) { nrm[k] += tnrm[k] * w; pos[k] += tpos[k] * w; }
            }
            if (!v.has_normal) { nrm[0] = 0.0f; nrm[1] = 0.0f; nrm[2] = 1.0f; }
        }

        // Skinning (PPSSPP ComputeSkinMatrix + Step_Pos*Skin / Normal*Skin):
        // the vertex weights blend the bone matrices; position and normal go
        // through the blended 4x3 matrix before the world matrix.
        if (skin) {
            const uint32_t wo = base + static_cast<uint32_t>(layout.weights);
            float m[12] = {0};
            for (int j = 0; j < wcount; j++) {
                float w;
                if (wt == 1) w = read_u8(rdram, wo + j) / 128.0f;
                else if (wt == 2) w = read_u16(rdram, wo + 2 * j) / 32768.0f;
                else w = read_float(rdram, wo + 4 * j);
                if (w == 0.0f) continue;
                const float* bone = &state.bone_matrix[j * 12];
                for (int k = 0; k < 12; k++) m[k] += w * bone[k];
            }
            float sp[3];
            vec3_by_matrix43(m, pos, sp);
            pos[0] = sp[0]; pos[1] = sp[1]; pos[2] = sp[2];
            if (v.has_normal) {
                const float n0 = nrm[0], n1 = nrm[1], n2 = nrm[2];
                nrm[0] = n0 * m[0] + n1 * m[3] + n2 * m[6];
                nrm[1] = n0 * m[1] + n1 * m[4] + n2 * m[7];
                nrm[2] = n0 * m[2] + n1 * m[5] + n2 * m[8];
            }
        }

        v.uv[0] = uv[0];
        v.uv[1] = uv[1];
        if (v.has_color) {
            for (int k = 0; k < 4; k++) {
                const float c = col[k] < 0.0f ? 0.0f : (col[k] > 255.0f ? 255.0f : col[k]);
                v.color[k] = static_cast<uint8_t>(c);
            }
        } else {
            // No vertex color: the material ambient RGB + MATERIALALPHA
            // (PPSSPP gstate.getMaterialAmbientRGBA()).
            v.color[0] = static_cast<uint8_t>(state.material_ambient & 0xFF);
            v.color[1] = static_cast<uint8_t>((state.material_ambient >> 8) & 0xFF);
            v.color[2] = static_cast<uint8_t>((state.material_ambient >> 16) & 0xFF);
            v.color[3] = static_cast<uint8_t>(state.material_alpha & 0xFF);
        }
        for (int k = 0; k < 3; k++) { v.normal[k] = nrm[k]; v.pos[k] = pos[k]; }
    }

    (void)prim_type;  // Used by caller for primitive assembly
}

// ---- Transform ----

// Degenerate-matrix detection. These compensate for
// guest-side broken matrix uploads (open issue, FPU/VFPU
// dataflow family): view arrives all-zero and proj arrives
// NaN/Inf or with a collapsed diagonal. Remove the fallbacks
// once the guest uploads sane matrices.

/// True if every element of the view matrix is zero.
static bool ge_view_matrix_all_zero(const GeState& state) {
    for (int i = 0; i < 12; i++) {
        if (state.view_matrix[i] != 0.0f) return false;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr,
            "[GE] view matrix all-zero -- NDC-direct "
            "fallback engaged\n");
    }
    return true;
}

/// True if the proj matrix is non-finite or has a
/// collapsed X/Y diagonal (column-major indices 0 and 5).
static bool ge_proj_matrix_degenerate(const GeState& state) {
    bool bad = std::fabs(state.proj_matrix[0]) < 1e-6f
            || std::fabs(state.proj_matrix[5]) < 1e-6f;
    for (int i = 0; i < 16 && !bad; i++) {
        if (!std::isfinite(state.proj_matrix[i])) bad = true;
    }
    if (bad) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE] proj matrix degenerate -- NDC-direct "
                "fallback engaged\n");
        }
    }
    return bad;
}

/// Compose model->world->view->proj for one position and return its NDC
/// (post perspective-divide). Mirrors the real transform path below.
static void ge_compose_mvp_ndc(
    const GeState& state, const float pos[3], float ndc[3]
) {
    float wpos[3], vpos[3], clip[4];
    vec3_by_matrix43(state.world_matrix, pos, wpos);
    vec3_by_matrix43(state.view_matrix, wpos, vpos);
    vec3_by_matrix44(state.proj_matrix, vpos, clip);
    if (std::fabs(clip[3]) > 1e-6f) {
        ndc[0] = clip[0] / clip[3];
        ndc[1] = clip[1] / clip[3];
        ndc[2] = clip[2] / clip[3];
    } else {
        ndc[0] = clip[0];
        ndc[1] = clip[1];
        ndc[2] = clip[2];
    }
}

/// Generic collapsed-MVP detector. The real transform inputs (guest-uploaded
/// matrices) are still wrong (FPU/VFPU dataflow family, open issue): they
/// now arrive non-zero but numerically broken, so neither the all-zero nor
/// the diagonal-degenerate gate above fires, yet the composed MVP crushes a
/// whole primitive onto a few-pixel sliver at screen center -- a black screen.
///
/// Detection is GENERIC (no game constants). It transforms the prim's
/// vertices through the real MVP and compares the post-transform NDC footprint
/// against the pre-transform model-space footprint, using two independent
/// signals that must BOTH hold so legitimately-small geometry is never
/// mis-flagged:
///
///   1. ABSOLUTE: the prim's NDC footprint is below a few-pixel "effectively
///      invisible" ceiling (NDC_INVISIBLE_MAX). A correctly-rendered visible
///      element is larger than this.
///   2. RELATIVE: the shrink ratio (NDC extent per unit of model spread) is
///      far below any plausible projection (RATIO_MIN). A genuinely-small but
///      correctly-transformed element keeps a normal ratio (its small NDC
///      matches its small model), so it fails this and is left alone; only a
///      meaningfully-spread prim that the MVP crushes to a sliver fails both.
///
/// A non-finite composed NDC always counts as collapsed. Measured values:
/// broken Patapon title prims read model spread ~28 -> NDC extent ~0.02
/// (ratio ~0.0007); a healthy 1:1 ortho maps 480 model units -> NDC 2.0
/// (ratio ~0.0042) and never trips the absolute ceiling.
static bool ge_mvp_collapses(
    const GeState& state, const std::vector<DecodedVertex>& verts
) {
    if (verts.size() < 2) return false;

    // ~8 px on a 480-wide target (8 * 2/480). A real visible element spans
    // more; a crushed prim is far below this.
    constexpr float NDC_INVISIBLE_MAX = 8.0f * 2.0f / 480.0f;  // ~0.0333
    // Minimum plausible NDC-per-model-unit. A healthy 1:1 ortho is ~0.0042;
    // the broken title is ~0.0007. 0.002 sits ~3x under healthy and ~3x over
    // broken -- a wide guard band either way.
    constexpr float RATIO_MIN = 0.002f;
    // Below this model spread the input is genuinely tiny, so a small NDC is
    // correct (the ratio test would be noisy on a near-zero denominator).
    constexpr float MODEL_SPREAD_MIN = 4.0f;

    float ndc_min[2] = {1e30f, 1e30f};
    float ndc_max[2] = {-1e30f, -1e30f};
    float mdl_min[3] = {1e30f, 1e30f, 1e30f};
    float mdl_max[3] = {-1e30f, -1e30f, -1e30f};
    bool non_finite = false;

    for (const auto& v : verts) {
        float ndc[3];
        ge_compose_mvp_ndc(state, v.pos, ndc);
        for (int k = 0; k < 2; k++) {
            if (!std::isfinite(ndc[k])) { non_finite = true; continue; }
            if (ndc[k] < ndc_min[k]) ndc_min[k] = ndc[k];
            if (ndc[k] > ndc_max[k]) ndc_max[k] = ndc[k];
        }
        for (int k = 0; k < 3; k++) {
            if (v.pos[k] < mdl_min[k]) mdl_min[k] = v.pos[k];
            if (v.pos[k] > mdl_max[k]) mdl_max[k] = v.pos[k];
        }
    }

    float mdl_spread = 0.0f;
    for (int k = 0; k < 3; k++)
        mdl_spread = std::max(mdl_spread, mdl_max[k] - mdl_min[k]);
    float ndc_extent = std::max(ndc_max[0] - ndc_min[0],
                                ndc_max[1] - ndc_min[1]);
    float ratio = (mdl_spread > 1e-6f) ? (ndc_extent / mdl_spread) : 1.0f;

    bool collapsed =
        non_finite
        || (mdl_spread > MODEL_SPREAD_MIN
            && ndc_extent < NDC_INVISIBLE_MAX
            && ratio < RATIO_MIN);
    if (collapsed) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE] composed MVP collapses prim (model spread %.3f -> "
                "NDC extent %.5f, ratio %.5f) -- NDC-direct fallback "
                "engaged\n", mdl_spread, ndc_extent, ratio);
        }
    }
    return collapsed;
}

// ---- Per-vertex lighting ----

// Snapshot the lighting registers from GeState into the pure lighting
// input. Colors pass through as 0xBBGGRR; LIGHTTYPE splits into the
// computation (bits [1:0]) and the type (bits [9:8]).
static void ge_build_lighting_state(
    const GeState& state, GeLightingState& out
) {
    out.ambientColor = state.ambient_color;
    out.ambientAlpha =
        static_cast<uint8_t>(state.ambient_alpha & 0xFFu);
    out.materialEmissive = state.material_emissive;
    out.materialAmbient = state.material_ambient;
    out.materialDiffuse = state.material_diffuse;
    out.materialSpecular = state.material_specular;
    out.materialAlpha =
        static_cast<uint8_t>(state.material_alpha & 0xFFu);
    out.materialSpecularCoef = state.material_specular_coef;
    out.materialUpdate = state.material_update;
    out.lightMode = state.light_mode;
    out.reverseNormal = state.reverse_normal;
    for (int i = 0; i < 4; i++) {
        GeLightParams& l = out.lights[i];
        l.enabled = state.light_enable[i];
        l.computation = state.light_type[i] & 0x3u;
        l.type = (state.light_type[i] >> 8) & 0x3u;
        l.pos[0] = state.light_pos[i][0];
        l.pos[1] = state.light_pos[i][1];
        l.pos[2] = state.light_pos[i][2];
        l.dir[0] = state.light_dir[i][0];
        l.dir[1] = state.light_dir[i][1];
        l.dir[2] = state.light_dir[i][2];
        l.att[0] = state.light_att[i][0];
        l.att[1] = state.light_att[i][1];
        l.att[2] = state.light_att[i][2];
        l.spotExp = state.light_spot_exp[i];
        l.spotCutoff = state.light_spot_cutoff[i];
        l.ambient = state.light_ambient[i];
        l.diffuse = state.light_diffuse[i];
        l.specular = state.light_specular[i];
    }
}

// Light one vertex: world-space normal is the model normal through the
// upper 3x3 of the world matrix (no translation), normalized; the zero
// vector when the vtype carries no normal. Colorless vertices pass null
// so the material registers are used (their decoder-filled color is the
// lighting-OFF path and stays untouched there).
static void ge_light_vertex(
    const GeState& state, const GeLightingState& ls,
    DecodedVertex& v, const float wpos[3]
) {
    float wn[3] = {0.0f, 0.0f, 0.0f};
    if (v.has_normal) {
        wn[0] = v.normal[0] * state.world_matrix[0]
              + v.normal[1] * state.world_matrix[3]
              + v.normal[2] * state.world_matrix[6];
        wn[1] = v.normal[0] * state.world_matrix[1]
              + v.normal[1] * state.world_matrix[4]
              + v.normal[2] * state.world_matrix[7];
        wn[2] = v.normal[0] * state.world_matrix[2]
              + v.normal[1] * state.world_matrix[5]
              + v.normal[2] * state.world_matrix[8];
        const float len = std::sqrt(
            wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
        if (len > 1e-6f && std::isfinite(len)) {
            wn[0] /= len;
            wn[1] /= len;
            wn[2] /= len;
        } else {
            wn[0] = wn[1] = wn[2] = 0.0f;
        }
    }
    const uint8_t* vc = v.has_color ? v.color : nullptr;
    uint8_t lit[4];
    ge_compute_lit_color(ls, wpos, wn, vc, lit);
    v.color[0] = lit[0];
    v.color[1] = lit[1];
    v.color[2] = lit[2];
    v.color[3] = lit[3];
}

void ge_snap_through_positions(std::vector<DecodedVertex>& verts) {
    for (auto& v : verts) {
        v.pos[0] = std::ceil(v.pos[0] - 0.5f);
        v.pos[1] = std::ceil(v.pos[1] - 0.5f);
    }
}

// Game-module degenerate-matrix fallback slot (#47 P5 seam).
static GeDegenerateFallbackFn g_degenerate_fallback = nullptr;

void ge_vertex_set_degenerate_fallback(GeDegenerateFallbackFn fn) {
    g_degenerate_fallback = fn;
}

void ge_transform_vertices(
    std::vector<DecodedVertex>& verts,
    const GeState& state
) {
    bool through = ge_vtype_through(state.vertex_type);

    if (through) {
        // Through-mode: map screen coords to NDC [-1,1]
        // PSP screen is 480x272
        //
        // Through-mode UVs arrive in TEXEL units (e.g. 64.0 for a
        // 128-wide texture), but GL samples in normalized [0,1]
        // coords. PPSSPP normalizes by the texture dimensions
        // (GPU/Common/SoftwareTransformCommon.cpp: uscale /=
        // curTextureWidth; vscale /= curTextureHeight). Without
        // this, every content UV >= 1.0 clamps to the bottom-right
        // texel under GL_CLAMP_TO_EDGE -> textured 2D content
        // discards. tex_size[0] packs log2 dims: log2_w | (log2_h<<8).
        const float texw = static_cast<float>(
            1u << (state.tex_size[0] & 0xFFu));
        const float texh = static_cast<float>(
            1u << ((state.tex_size[0] >> 8) & 0xFFu));
        for (auto& v : verts) {
            v.pos[0] = v.pos[0] / 240.0f - 1.0f;
            v.pos[1] = 1.0f - v.pos[1] / 136.0f;
            v.pos[2] = v.pos[2] / 65535.0f;
            v.w = 1.0f;  // screen-space: already NDC

            // Apply tex scale/offset, then texel -> [0,1] normalize.
            if (v.has_uv) {
                v.uv[0] = v.uv[0] * state.tex_scale_u
                          + state.tex_offset_u;
                v.uv[1] = v.uv[1] * state.tex_scale_v
                          + state.tex_offset_v;
                if (state.texture_enable) {
                    if (texw > 0.0f) v.uv[0] /= texw;
                    if (texh > 0.0f) v.uv[1] /= texh;
                }
            }
        }
    } else {
        // Transform mode: model -> world -> view -> proj
        // using PSP column-major matrix semantics.
        //
        // FALLBACKS: the guest currently uploads broken
        // matrices (view all-zero; proj NaN/Inf or malformed
        // diagonal) -- open issue, FPU/VFPU dataflow family.
        // Remove these workarounds when that is fixed.
        //
        // An all-zero view with a valid proj can never render
        // correctly through the real path (identity-view is a
        // wrong guess against the title's ortho extents), so an
        // all-zero view ALSO routes to the degenerate fallback.
        // The full real path engages automatically once the guest
        // uploads a non-zero view matrix.
        // The all-zero / diagonal-degenerate gates catch the matrices that
        // are *obviously* broken. ge_mvp_collapses additionally catches the
        // subtler case (#27 family): matrices that arrive non-zero and
        // non-singular but compose into an MVP that maps the whole prim onto
        // a sub-pixel cluster -- a silent black screen the static gates miss.
        // Only a game that installed a fallback (its broken-matrix workaround)
        // takes the NDC-direct path. Without one the real transform runs, as on
        // hardware: a game that hides an object by scaling it to zero gets a
        // single clip point (nothing drawn); the old world-space passthrough
        // turned such objects into screen-covering triangles (Patapon 3).
        const bool have_fallback = g_degenerate_fallback != nullptr;
        bool view_zero = have_fallback && ge_view_matrix_all_zero(state);
        bool proj_bad = have_fallback && ge_proj_matrix_degenerate(state);
        bool mvp_collapsed = have_fallback &&
            !view_zero && !proj_bad && ge_mvp_collapses(state, verts);
        bool ndc_direct = view_zero || proj_bad || mvp_collapsed;

        GeLightingState light_state;
        const bool do_lighting = state.lighting_enable;
        if (do_lighting) ge_build_lighting_state(state, light_state);

        for (auto& v : verts) {
            float wpos[3], vpos[3];
            vec3_by_matrix43(state.world_matrix,
                             v.pos, wpos);
            if (do_lighting) ge_light_vertex(state, light_state, v, wpos);
            if (!ndc_direct) {
                vec3_by_matrix43(state.view_matrix,
                                 wpos, vpos);
            }

            if (ndc_direct) {
                // Bypass proj: emit NDC directly from world-space.
                // The mapping is title-tuned and installs from the
                // game module (#47 P5 seam); without one, pass the
                // world-space position through unchanged.
                if (g_degenerate_fallback != nullptr) {
                    g_degenerate_fallback(wpos, v.pos);
                } else {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        std::fprintf(stderr,
                            "[GE] degenerate-matrix fallback: no game "
                            "fallback installed -- world-space "
                            "passthrough\n");
                    }
                    v.pos[0] = wpos[0];
                    v.pos[1] = wpos[1];
                    v.pos[2] = wpos[2];
                }
                v.w = 1.0f;  // positions are already NDC
            } else {
                float clip[4];
                vec3_by_matrix44(state.proj_matrix,
                                 vpos, clip);

                // Clip-space out, NO CPU perspective divide: GL's own
                // divide provides near-plane clipping (a vertex with
                // w <= 0 no longer flips to the opposite screen side and
                // drags a "fan" spike across the frame) and perspective-
                // correct UV interpolation.
                v.pos[0] = clip[0];
                v.pos[1] = clip[1];
                v.pos[2] = clip[2];
                v.w = clip[3];
                // Range culling like the hardware (PPSSPP sets the vertex
                // to NaN so GL drops every primitive that uses it).
                if (ge_vertex_range_culled(state, clip)) {
                    const float nan = std::numeric_limits<float>::quiet_NaN();
                    v.pos[0] = v.pos[1] = v.pos[2] = v.w = nan;
                }
            }

            // Apply tex scale/offset
            if (v.has_uv) {
                v.uv[0] = v.uv[0] * state.tex_scale_u
                          + state.tex_offset_u;
                v.uv[1] = v.uv[1] * state.tex_scale_v
                          + state.tex_offset_v;
            }
        }
    }
}

// ---- Vertex range culling (PPSSPP VertexShaderGenerator.cpp) ----
//
// The GE drops a primitive when one of its transformed vertices lands
// outside the [0, 4096) drawing space after the viewport transform, or has
// w < -1 -- unless the vertex is z-clipped (z < -w), where near-plane
// clipping applies instead. Without it a quad crossing the camera projected
// to a huge screen-filling triangle (the black rectangles in the prologue).
bool ge_vertex_range_culled(const GeState& state, const float clip[4]) {
    const float w = clip[3];
    if (clip[2] < -w) return false;  // z-clipped: clipped, not culled
    if (w < -1.0f) return true;
    if (w == 0.0f) return false;
    const float sx = clip[0] * state.viewport_x_scale / w + state.viewport_x_center;
    const float sy = clip[1] * state.viewport_y_scale / w + state.viewport_y_center;
    return !(sx >= 0.0f && sy >= 0.0f && sx < 4096.0f && sy < 4096.0f);
}

// ---- Rectangle expansion (GE_PRIM_RECTANGLES) ----
//
// Rectangles arrive as 2 diagonal corner vertices and are drawn as two
// screen-aligned triangles (6 vertices). In transform mode the corners are
// in clip space now, so the divide happens here: only the position is
// divided (UVs/colors must stay untouched for correct interpolation), and
// the emitted quad is flat NDC with w = 1. A corner with w <= 0 is behind
// the eye plane -- it has no screen position (dividing would flip it to the
// opposite side of the screen), so the whole rectangle is dropped.
bool ge_expand_rectangle(
    const DecodedVertex& v0,
    const DecodedVertex& v1,
    DecodedVertex out[6]
) {
    if (!(v0.w > 0.0f) || !(v1.w > 0.0f)) return false;

    // Perspective-divided corners (position only).
    DecodedVertex a = v0;  // top-left
    DecodedVertex b = v1;  // bottom-right
    for (int k = 0; k < 3; k++) {
        a.pos[k] = v0.pos[k] / v0.w;
        b.pos[k] = v1.pos[k] / v1.w;
    }
    a.w = 1.0f;
    b.w = 1.0f;

    // 4 corners from the 2 diagonal vertices (same mixing as the
    // historical expansion: z from v0 except the pure-v1 corner).
    DecodedVertex c1 = b;  // top-right: (v1.x, v0.y, v0.z), uv (v1.u, v0.v)
    c1.pos[1] = a.pos[1];
    c1.pos[2] = a.pos[2];
    c1.uv[1] = a.uv[1];
    DecodedVertex c3 = a;  // bottom-left: (v0.x, v1.y, v0.z), uv (v0.u, v1.v)
    c3.pos[1] = b.pos[1];
    c3.uv[1] = b.uv[1];

    // Two triangles: a-c1-b and a-b-c3
    out[0] = a;
    out[1] = c1;
    out[2] = b;
    out[3] = a;
    out[4] = b;
    out[5] = c3;
    return true;
}
