// psp_ge_fog.h -- GE fog, the PPSSPP hardware-renderer way. A transformed vertex gets
// f = (view z + FOG1) * FOG2 (VertexShaderGenerator.cpp:904); the fragment mixes
// rgb = mix(FOGCOLOR, rgb, clamp(f, 0, 1)) after the texture function (FragmentShaderGenerator.cpp:866).
// Through-mode draws are never fogged (ShaderId.cpp:326).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// GE float24: the register's 24 bits are the top 24 bits of an IEEE float (PPSSPP getFloat24).
inline float ge_float24(uint32_t data) {
    const uint32_t bits = (data & 0xFFFFFFu) << 8;
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// FOG1 / FOG2 as the shader uses them: the PSP ignores inf/NaN, PPSSPP maps them to +-65535
// keeping the sign (ShaderUniforms.cpp:255 UpdateFogCoef).
inline float ge_fog_coef(uint32_t data) {
    const float f = ge_float24(data);
    if (std::isfinite(f)) return f;
    return std::signbit(f) ? -65535.0f : 65535.0f;
}

inline bool ge_fog_active(bool fog_enable, bool through) { return fog_enable && !through; }

// Unclamped; 1 = no fog, <= 0 = only the fog colour. The shader clamps.
inline float ge_fog_factor(float view_z, float fog1, float fog2) { return (view_z + fog1) * fog2; }

// FOGCOLOR is 0xBBGGRR.
inline void ge_fog_color_rgb(uint32_t fogcolor, float out[3]) {
    out[0] = static_cast<float>(fogcolor & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((fogcolor >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((fogcolor >> 16) & 0xFFu) / 255.0f;
}

// CPU twin of the fragment-shader line, for tests.
inline void ge_fog_mix(const float fog_rgb[3], float factor, float rgb[3]) {
    const float t = factor < 0.0f ? 0.0f : (factor > 1.0f ? 1.0f : factor);
    for (int i = 0; i < 3; ++i) rgb[i] = fog_rgb[i] + (rgb[i] - fog_rgb[i]) * t;
}
