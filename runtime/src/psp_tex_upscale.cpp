#include "psp_tex_upscale.h"

#include <cstring>

#include "xbrz.h"

int tex_upscale_factor_parse(const char* env) {
    if (env == nullptr) return 0;
    if (std::strcmp(env, "xbrz2") == 0) return 2;
    if (std::strcmp(env, "xbrz3") == 0) return 3;
    if (std::strcmp(env, "xbrz4") == 0) return 4;
    return 0;
}

bool tex_upscale_xbrz(int factor, const uint8_t* rgba, int w, int h, std::vector<uint8_t>* out) {
    if (factor < 2 || factor > 4 || w <= 0 || h <= 0) return false;
    if (w * factor > kTexUpscaleMaxSide || h * factor > kTexUpscaleMaxSide) return false;
    // xBRZ works on 0xAARRGGBB words.
    std::vector<uint32_t> src(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < src.size(); i++) {
        const uint8_t* p = rgba + i * 4;
        src[i] = uint32_t(p[3]) << 24 | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
    }
    std::vector<uint32_t> dst(src.size() * factor * factor);
    xbrz::scale(static_cast<size_t>(factor), src.data(), dst.data(), w, h, xbrz::ColorFormat::ARGB);
    out->resize(dst.size() * 4);
    for (size_t i = 0; i < dst.size(); i++) {
        uint8_t* p = out->data() + i * 4;
        p[0] = uint8_t(dst[i] >> 16);
        p[1] = uint8_t(dst[i] >> 8);
        p[2] = uint8_t(dst[i]);
        p[3] = uint8_t(dst[i] >> 24);
    }
    return true;
}
