#include "psp_ge_lighting.h"

#include <cmath>
#include <cstdint>

// Decode a 24-bit GE color (0xBBGGRR) to float 0..1 channels.
static void ge_light_rgb(uint32_t bbgrr, float out[3]) {
    out[0] = static_cast<float>(bbgrr & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((bbgrr >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((bbgrr >> 16) & 0xFFu) / 255.0f;
}

static float ge_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float ge_length(const float v[3]) {
    return std::sqrt(ge_dot(v, v));
}

static uint8_t ge_clamp_u8(float v) {
    const long r = std::lround(v * 255.0f);
    if (r < 0) return 0;
    if (r > 255) return 255;
    return static_cast<uint8_t>(r);
}

void ge_compute_lit_color(
    const GeLightingState& st,
    const float wpos[3],
    const float wnormal[3],
    const uint8_t* vertexColorOrNull,
    uint8_t outColor[4]
) {
    const bool hasColor = (vertexColorOrNull != nullptr);
    const uint32_t update = hasColor ? (st.materialUpdate & 7u) : 0u;

    // Material channels: vertex color wins when its MATERIALUPDATE bit is
    // set (PPSSPP Lighting::ComputeState colorForAmbient/Diffuse/Specular).
    float matAmb[3], matDiff[3], matSpec[3];
    float vertexF[3] = {0.0f, 0.0f, 0.0f};
    if (hasColor) {
        vertexF[0] = static_cast<float>(vertexColorOrNull[0]) / 255.0f;
        vertexF[1] = static_cast<float>(vertexColorOrNull[1]) / 255.0f;
        vertexF[2] = static_cast<float>(vertexColorOrNull[2]) / 255.0f;
    }
    if ((update & 1u) != 0u) {
        matAmb[0] = vertexF[0];
        matAmb[1] = vertexF[1];
        matAmb[2] = vertexF[2];
    } else {
        ge_light_rgb(st.materialAmbient, matAmb);
    }
    if ((update & 2u) != 0u) {
        matDiff[0] = vertexF[0];
        matDiff[1] = vertexF[1];
        matDiff[2] = vertexF[2];
    } else {
        ge_light_rgb(st.materialDiffuse, matDiff);
    }
    if ((update & 4u) != 0u) {
        matSpec[0] = vertexF[0];
        matSpec[1] = vertexF[1];
        matSpec[2] = vertexF[2];
    } else {
        ge_light_rgb(st.materialSpecular, matSpec);
    }

    // World-space normal with REVERSENORMAL; a zero vector (vtype without
    // normals) stays zero and disables diffuse/specular below.
    float n[3] = {wnormal[0], wnormal[1], wnormal[2]};
    if (st.reverseNormal) {
        n[0] = -n[0];
        n[1] = -n[1];
        n[2] = -n[2];
    }
    const float nLen = ge_length(n);
    const bool hasNormal = (nLen > 1e-6f) && std::isfinite(nLen);
    if (hasNormal) {
        n[0] /= nLen;
        n[1] /= nLen;
        n[2] /= nLen;
    } else {
        n[0] = n[1] = n[2] = 0.0f;
    }

    const float specExp =
        (st.materialSpecularCoef > 0.0f
         && std::isfinite(st.materialSpecularCoef))
        ? st.materialSpecularCoef
        : 0.0f;

    // Base: emissive + ambient * materialAmbient.
    float emissive[3], baseAmb[3];
    ge_light_rgb(st.materialEmissive, emissive);
    ge_light_rgb(st.ambientColor, baseAmb);
    float out[3] = {
        emissive[0] + baseAmb[0] * matAmb[0],
        emissive[1] + baseAmb[1] * matAmb[1],
        emissive[2] + baseAmb[2] * matAmb[2],
    };

    // View direction for the specular half-vector (see header: +Z).
    const float viewDir[3] = {0.0f, 0.0f, 1.0f};

    for (int li = 0; li < 4; li++) {
        const GeLightParams& l = st.lights[li];
        if (!l.enabled) continue;

        const bool directional = (l.type == 0u);
        const bool spot = (l.type >= 2u);

        // L: direction from the vertex to the light, + attenuation/spot.
        float L[3];
        float att = 1.0f;
        float spotScale = 1.0f;
        if (directional) {
            // A zero direction stays zero: no diffuse from this light.
            const float len = ge_length(l.pos);
            if (len > 1e-6f && std::isfinite(len)) {
                L[0] = l.pos[0] / len;
                L[1] = l.pos[1] / len;
                L[2] = l.pos[2] / len;
            } else {
                L[0] = L[1] = L[2] = 0.0f;
            }
        } else {
            float vec[3] = {
                l.pos[0] - wpos[0],
                l.pos[1] - wpos[1],
                l.pos[2] - wpos[2],
            };
            float d = ge_length(vec);
            if (!std::isfinite(d)) {
                continue;  // Degenerate light position: no contribution.
            }
            if (d < 1e-6f) {
                L[0] = 0.0f;
                L[1] = 0.0f;
                L[2] = 1.0f;
                d = 0.0f;
            } else {
                L[0] = vec[0] / d;
                L[1] = vec[1] / d;
                L[2] = vec[2] / d;
            }
            const float den =
                l.att[0] + l.att[1] * d + l.att[2] * d * d;
            if (!(den > 0.0f) || !std::isfinite(den)) {
                att = 0.0f;
            } else {
                att = 1.0f / den;
                if (att > 1.0f) att = 1.0f;
            }
        }

        if (spot) {
            const float sLen = ge_length(l.dir);
            float raw = 0.0f;
            if (sLen > 1e-6f && std::isfinite(sLen)) {
                raw = (l.dir[0] * L[0] + l.dir[1] * L[1]
                       + l.dir[2] * L[2]) / sLen;
            }
            const float sExp =
                (l.spotExp > 0.0f && std::isfinite(l.spotExp))
                ? l.spotExp
                : 0.0f;
            if (!std::isfinite(raw)) {
                spotScale = 0.0f;
            } else if (raw >= l.spotCutoff) {
                // pow(raw, 0) == 1: a zero exponent means no falloff.
                if (raw <= 0.0f && sExp > 0.0f) {
                    spotScale = 0.0f;
                } else {
                    spotScale =
                        std::pow(raw > 0.0f ? raw : 0.0f, sExp);
                }
                if (!std::isfinite(spotScale)) spotScale = 0.0f;
            } else {
                spotScale = 0.0f;
            }
        }

        const float attSpot = att * spotScale;
        if (attSpot <= 0.0f) {
            // Still contributes nothing, but keep the ambient skip cheap:
            // all three scales below multiply by attSpot.
        }

        // Ambient: lightAmbient * materialAmbient.
        {
            float lac[3];
            ge_light_rgb(l.ambient, lac);
            out[0] += lac[0] * matAmb[0] * attSpot;
            out[1] += lac[1] * matAmb[1] * attSpot;
            out[2] += lac[2] * matAmb[2] * attSpot;
        }

        if (!hasNormal) continue;
        const float ndl = ge_dot(n, L);

        // Diffuse: powered when the computation is ONLYPOWDIFFUSE (2).
        if (l.computation == 2u) {
            if (ndl > 0.0f) {
                const float df = std::pow(ndl, specExp);
                if (std::isfinite(df) && df > 0.0f) {
                    float ldc[3];
                    ge_light_rgb(l.diffuse, ldc);
                    const float s = df * attSpot;
                    out[0] += ldc[0] * matDiff[0] * s;
                    out[1] += ldc[1] * matDiff[1] * s;
                    out[2] += ldc[2] * matDiff[2] * s;
                }
            }
        } else if (ndl > 0.0f) {
            float ldc[3];
            ge_light_rgb(l.diffuse, ldc);
            const float s = ndl * attSpot;
            out[0] += ldc[0] * matDiff[0] * s;
            out[1] += ldc[1] * matDiff[1] * s;
            out[2] += ldc[2] * matDiff[2] * s;
        }

        // Specular: only for the BOTH computation (1); folded into primary
        // (LIGHTMODE separate-specular is noted, not split).
        if (l.computation == 1u && ndl >= 0.0f) {
            float h[3] = {
                L[0] + viewDir[0],
                L[1] + viewDir[1],
                L[2] + viewDir[2],
            };
            const float hLen = ge_length(h);
            if (hLen > 1e-6f && std::isfinite(hLen)) {
                h[0] /= hLen;
                h[1] /= hLen;
                h[2] /= hLen;
            } else {
                h[0] = 0.0f;
                h[1] = 0.0f;
                h[2] = 1.0f;
            }
            const float ndh = ge_dot(n, h);
            if (ndh > 0.0f) {
                const float sf = std::pow(ndh, specExp);
                if (std::isfinite(sf) && sf > 0.0f) {
                    float lsc[3];
                    ge_light_rgb(l.specular, lsc);
                    const float s = sf * attSpot;
                    out[0] += lsc[0] * matSpec[0] * s;
                    out[1] += lsc[1] * matSpec[1] * s;
                    out[2] += lsc[2] * matSpec[2] * s;
                }
            }
        }
    }

    outColor[0] = ge_clamp_u8(out[0]);
    outColor[1] = ge_clamp_u8(out[1]);
    outColor[2] = ge_clamp_u8(out[2]);

    // Alpha = ambientAlpha * material(/vertex)Alpha / 255. The vertex alpha
    // feeds the material slot exactly when MATERIALUPDATE takes ambient
    // from the vertex color (bit 0), mirroring the RGB selection above.
    const uint32_t matA =
        ((update & 1u) != 0u)
        ? static_cast<uint32_t>(vertexColorOrNull[3])
        : static_cast<uint32_t>(st.materialAlpha & 0xFFu);
    const uint32_t ambA = static_cast<uint32_t>(st.ambientAlpha & 0xFFu);
    outColor[3] = static_cast<uint8_t>((ambA * matA + 127u) / 255u);
}
