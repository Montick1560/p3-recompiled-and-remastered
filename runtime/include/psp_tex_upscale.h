#pragma once
// Optional xBRZ upscaling of the game's own textures (PSPRECOMP_TEX_UPSCALE = xbrz2 | xbrz3 | xbrz4), for players
// without the HD pack. Pure: no GL, safe on a worker thread.
#include <cstdint>
#include <vector>

constexpr int kTexUpscaleMaxSide = 2048;  // a texture whose scaled side would pass this is not scaled (VRAM)

// "xbrz2" / "xbrz3" / "xbrz4" -> 2 / 3 / 4; anything else (null, "", "off") -> 0 = off.
int tex_upscale_factor_parse(const char* env);

// RGBA8 rows (top first) in, w*factor x h*factor RGBA8 rows out. False, out untouched, when factor is not 2..4,
// w or h <= 0, or a scaled side would exceed kTexUpscaleMaxSide.
bool tex_upscale_xbrz(int factor, const uint8_t* rgba, int w, int h, std::vector<uint8_t>* out);
