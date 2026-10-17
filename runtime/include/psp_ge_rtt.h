#pragma once
#include <cstdint>

// Render-to-texture helpers, pure (no GL). PPSSPP references:
// GPU/Common/TextureCacheCommon.cpp MatchFramebuffer (colour channel, non-CLUT formats).

// A GE render target as the texture path sees it (one per FBO pool slot).
struct GeRttTarget {
    bool valid;       // GL holds content newer than guest RAM (gl_written && !cpu_newer)
    uint32_t key;     // VRAM offset of the buffer: addr & 0x001FFFF0
    uint32_t stride;  // FRAMEBUFWIDTH & 0x7FC, in pixels
    uint32_t fb_fmt;  // FRAMEBUFPIXFORMAT: 0=5650 1=5551 2=4444 3=8888
};

struct GeRttHit {
    int index;  // matching target, -1 = sample guest RAM as usual
    int x, y;   // texel origin of the texture inside the target
};

// Texture formats 0..3 sample a render target when the texel size equals the pixel size, the
// strides agree (unless the texture is one row high) and the origin passes PPSSPP's sub-area
// rules: x inside the 480-pixel window, y < 512, y + texHeight/4 inside 272 lines when y > 0,
// y <= 32 for buffers above 0x04110000. CLUT and DXT never match (depal is not implemented).
// The nearest target at or below tex_addr wins. Any VRAM alias (0x04/0x44, +2 MB) works.
GeRttHit ge_rtt_find(const GeRttTarget* targets, int count, uint32_t tex_addr, int tex_fmt,
                     uint32_t tex_bufw, int tex_h);

// First valid target whose 272-line VRAM window [key, key + stride*bpp*272) overlaps the byte
// span [addr, addr + bytes); -1 if none or addr is not VRAM. Any format (detector use).
int ge_rtt_overlap(const GeRttTarget* targets, int count, uint32_t addr, uint32_t bytes);

// Bytes a texture spans in memory: bufw (at least 1) x h texels at the format's bit depth
// (16/16/16/32, CLUT 4/8/16/32, DXT1 4, DXT3/5 8). 0 for an unknown format or h <= 0.
uint32_t ge_rtt_tex_bytes(int tex_fmt, uint32_t tex_bufw, int tex_h);

// GL blit of the w x h texel window at PSP (x, y) of a 480x272 target rendered at `scale`
// (bottom-left GL origin) into a scratch texture whose row 0 is PSP row y. The window is
// clipped to the target; dst starts at (0, 0). empty = nothing of the window is inside.
struct GeRttBlit { bool empty; int src_x0, src_y0, src_x1, src_y1, dst_w, dst_h; };
GeRttBlit ge_rtt_blit_rect(int x, int y, int w, int h, int scale);
