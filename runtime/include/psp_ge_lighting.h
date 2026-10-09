#pragma once
#include <cstdint>

// PSP GE per-vertex lighting, CPU side.
//
// Semantic reference: PPSSPP's GPU/Software/Lighting.cpp (vendored under
// .brief/ for this worktree). The model is the fixed-function pipeline:
// emissive + ambient * materialAmbient, then per enabled light an ambient,
// a diffuse (N.L, optionally powered) and a specular (N.H ^ shininess)
// term, each scaled by distance attenuation and the spot factor.
//
// Simplifications vs PPSSPP (deliberate, documented):
//  - Plain float32 math in 0..1 instead of the GE's float24 product/row-sum
//    emulation (no truncation drift; <1 LSB on the covered cases).
//  - The LIGHTMODE separate-specular path (color1) is folded into primary:
//    specular adds into the returned RGB. No secondary color is produced.
//  - The view direction for the specular half-vector is the constant
//    +Z (0,0,1) instead of PPSSPP's per-vertex viewDir. No diffuse-only
//    test is sensitive to it; specular tests use head-on geometry where
//    both agree.
//  - Zero-contribution gating (PPSSPP's IsLargerThanHalf skips) is omitted:
//    black light/material colors simply contribute zero.
//  - NaN spot-dot/cutoff inputs yield a spot factor of 0 (PPSSPP maps
//    signed NaN/Inf to +-1 first; games never emit those here).
//
// Pure: no GL, no globals. All GE registers arrive already decoded in
// GeLightingState (24-bit colors as 0xBBGGRR like the other material
// registers; float registers as float32).
struct GeLightParams {
    bool enabled = false;
    // Light computation (LIGHTTYPE bits [1:0]): 0 = diffuse, 1 = diffuse +
    // specular (BOTH), 2 = powered diffuse. Other values behave as 0.
    uint32_t computation = 0u;
    // Light type (LIGHTTYPE bits [9:8]): 0 = directional, 1 = point,
    // 2/3 = spot.
    uint32_t type = 0u;
    float pos[3] = {0.0f, 0.0f, 0.0f};  // LX/LY/LZ (directional: direction)
    float dir[3] = {0.0f, 0.0f, 0.0f};  // LDX/LDY/LDZ (spot axis)
    float att[3] = {0.0f, 0.0f, 0.0f};  // LKA/LKB/LKC attenuation
    float spotExp = 0.0f;               // LKS spot exponent
    float spotCutoff = 0.0f;            // LKO spot cutoff (dot threshold)
    uint32_t ambient = 0u;              // LAC 0xBBGGRR
    uint32_t diffuse = 0u;              // LDC 0xBBGGRR
    uint32_t specular = 0u;             // LSC 0xBBGGRR
};

struct GeLightingState {
    uint32_t ambientColor = 0u;       // AMBIENTCOLOR 0xBBGGRR
    uint8_t ambientAlpha = 0xFFu;     // AMBIENTALPHA
    uint32_t materialEmissive = 0u;   // 0xBBGGRR
    uint32_t materialAmbient = 0u;    // 0xBBGGRR
    uint32_t materialDiffuse = 0u;    // 0xBBGGRR
    uint32_t materialSpecular = 0u;   // 0xBBGGRR
    uint8_t materialAlpha = 0xFFu;    // MATERIALALPHA
    float materialSpecularCoef = 0.0f;  // MATERIALSPECULARCOEF (pow exponent)
    uint32_t materialUpdate = 0u;     // MATERIALUPDATE bits: 1 amb 2 diff 4 spec
    uint32_t lightMode = 0u;          // LIGHTMODE (bit 0 separate: noted, folded)
    bool reverseNormal = false;       // REVERSENORMAL
    GeLightParams lights[4];
};

/// Compute the lit primary color for one vertex.
///
/// wpos: world-space position. wnormal: world-space normal; the zero vector
/// means "the vtype carries no normal" (no diffuse/specular, ambient only).
/// vertexColorOrNull: RGBA8 vertex color, or nullptr for colorless vertices
/// (material registers are used, gated by MATERIALUPDATE only when a vertex
/// color exists). outColor: lit RGBA8, clamped to 0..255; alpha is
/// ambientAlpha * material(/vertex)Alpha / 255.
void ge_compute_lit_color(
    const GeLightingState& st,
    const float wpos[3],
    const float wnormal[3],
    const uint8_t* vertexColorOrNull,
    uint8_t outColor[4]
);
