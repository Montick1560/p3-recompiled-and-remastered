#include "psp_ge_sampler.h"

#include <cstring>

GeSampler ge_sampler_from_regs(uint32_t tex_filter, uint32_t tex_wrap, GeFilterOverride ov) {
    GeSampler s{};
    s.min_linear = (tex_filter & 1u) != 0;
    s.mag_linear = ((tex_filter >> 8) & 1u) != 0;
    s.clamp_s = (tex_wrap & 1u) != 0;
    s.clamp_t = ((tex_wrap >> 8) & 1u) != 0;

    if (ov == GeFilterOverride::Nearest) {
        s.min_linear = false;
        s.mag_linear = false;
    } else if (ov == GeFilterOverride::Linear) {
        s.min_linear = true;
        s.mag_linear = true;
    }
    return s;
}

GeFilterOverride ge_filter_override_parse(const char* s) {
    if (s == nullptr || s[0] == '\0') {
        return GeFilterOverride::Auto;
    }
    if (std::strcmp(s, "nearest") == 0) {
        return GeFilterOverride::Nearest;
    }
    if (std::strcmp(s, "linear") == 0) {
        return GeFilterOverride::Linear;
    }
    return GeFilterOverride::Auto;
}
