#pragma once
// A CPU-side RGBA8 image (texture patching, button prompts).
#include <cstdint>
#include <vector>

struct RgbaImage {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // RGBA8, top row first
};
