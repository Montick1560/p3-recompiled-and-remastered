#pragma once
#include <cstdint>

// OpenGL-free sampler choice for a GE draw (PPSSPP default-settings semantics).
struct GeSampler {
    bool min_linear;  // GL_LINEAR instead of GL_NEAREST for minification
    bool mag_linear;  // GL_LINEAR instead of GL_NEAREST for magnification
    bool clamp_s;     // GL_CLAMP_TO_EDGE instead of GL_REPEAT on U
    bool clamp_t;     // GL_CLAMP_TO_EDGE instead of GL_REPEAT on V
};

// Filter override (env PSPRECOMP_TEX_FILTER): Auto follows the game.
enum class GeFilterOverride { Auto, Nearest, Linear };

GeSampler ge_sampler_from_regs(uint32_t tex_filter, uint32_t tex_wrap, GeFilterOverride ov);

// Parses PSPRECOMP_TEX_FILTER values: nullptr/""/"auto" -> Auto, "nearest" -> Nearest,
// "linear" -> Linear; anything else -> Auto.
GeFilterOverride ge_filter_override_parse(const char* s);
