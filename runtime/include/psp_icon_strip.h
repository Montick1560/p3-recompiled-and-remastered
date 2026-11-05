#pragma once
// An icon strip: one PNG holding `frames` equal frames side by side (the Patapon 3 ping signal icon), decoded on
// the CPU; the overlay uploads it on the GL thread (psp_world_tag_set_icon).
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "psp_texrep_png.h"

struct IconStrip {
    int w = 0, h = 0, frames = 0;
    std::vector<uint8_t> rgba;  // w x h RGBA8 rows
};

// False (and `out` untouched) when the file is missing, not a PNG, or its width is not `frames` equal frames.
// `path` is UTF-8 (the exe's code page is UTF-8, res/utf8.manifest).
inline bool psp_icon_strip_load(const std::string& path, int frames, IconStrip* out) {
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    IconStrip s;
    if (frames <= 0 || !ge_texrep_decode_png(bytes.data(), bytes.size(), &s.w, &s.h, &s.rgba)) return false;
    if (s.w % frames != 0) return false;
    s.frames = frames;
    *out = std::move(s);
    return true;
}
