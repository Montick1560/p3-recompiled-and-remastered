#pragma once
// RGBA8 helpers for drawing button prompts into a texture (pure, no GL), and the embedded icon set
// (Kenney Input Prompts 1.5, CC0: runtime/assets/button_prompts, built into the exe by
// runtime/cmake/embed_files.cmake).
#include "psp_rgba_image.h"

#include <cstdint>
#include <string>
#include <vector>

// The decoded 64x64 icon `name` (a psp_prompts.h icon name; "@arrows_all" / "@arrows_vertical" /
// "@arrows_up" are arrow-key keycaps drawn here), nullptr when unknown. Decoded once and kept; safe
// from any thread.
const RgbaImage* prompt_icon_image(const std::string& name);

// Resize to w x h on premultiplied alpha: box filter when shrinking, bilinear when growing.
RgbaImage rgba_resample(const RgbaImage& src, int w, int h);

// Transparent pixels (a < 32) not 4-connected to the image border become opaque (r, g, b): the
// symbol a Kenney icon cuts out of its shape gets the dark interior of the game's own glyphs.
void rgba_fill_holes(RgbaImage& img, uint8_t r, uint8_t g, uint8_t b);

// Neutral pixels (max - min channel < 24) get rgb scaled by gray / 255; coloured ones are kept.
void rgba_tint_neutral(RgbaImage& img, uint8_t gray);

// Clear dst's rectangle (x, y, w, h) to transparent and draw `icons` inside it: one icon centred, or
// two side by side with a slash (`slash_rgba`, 0xAABBGGRR) between them. Each icon is cropped to its
// alpha bounding box and scaled to fit, aspect kept. Nothing outside the rectangle is written.
void rgba_draw_cell(RgbaImage& dst, int x, int y, int w, int h, const std::vector<const RgbaImage*>& icons,
                    uint32_t slash_rgba);
