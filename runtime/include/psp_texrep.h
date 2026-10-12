#pragma once
// Texture replacement from a PPSSPP-format pack (PSPRECOMP_TEXTURES=<dir>).
// Render (GE) thread only. Spec: docs/superpowers/specs/2026-10-10-m3c-hd-textures-design.md
#include "psp_ge.h"
#include "psp_texrep_hash.h"
#include "psp_texrep_pack.h"

#include <cstddef>
#include <cstdint>
#include <string>

/// GE_CMD_LOADCLUT: snapshot the palette bytes the replacement key hashes.
void ge_texrep_on_loadclut(const uint8_t* rdram, uint32_t clut_addr, uint32_t loadclut_data);

/// The current CLUT snapshot.
const GeClutSnapshot& ge_texrep_clut();

/// Read PSPRECOMP_TEXTURES (pack folder), PSPRECOMP_TEXTURES_DUMP and
/// PSPRECOMP_TEXTURES_BUDGET_MB; load <dir>/textures.ini plus the root
/// listing. Logs one [TEXREP] line (silent when the env is unset).
void ge_texrep_init();

/// Byte budget for replacement texture memory (decoded PNGs and GL textures),
/// from PSPRECOMP_TEXTURES_BUDGET_MB, default 256 MiB. Valid after
/// ge_texrep_init() (also when no pack is loaded).
size_t ge_texrep_budget_bytes();

/// True when a pack is loaded.
bool ge_texrep_enabled();

/// PPSSPP replacement key of the live level-0 texture. draw_max_v = largest V
/// (texels) the current through-mode draw samples, 0 if unknown.
GeTexrepKey ge_texrep_key(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v);

/// Key + pack lookup; under PSPRECOMP_TEXTURES_DUMP logs each distinct key
/// once: "[TEXREP] key=<24 hex> WxH fmt=N hit|miss|ignored".
TexrepFind ge_texrep_lookup(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v,
                            GeTexrepKey* key_out);

/// A pack image for the live texture: RGBA8 rows, top row first.
struct GeTexrepImage {
    int w = 0, h = 0;
    const uint8_t* rgba = nullptr;  // owned by the texrep image cache
    TexrepFilter filter = TexrepFilter::None;
    std::string path;  // pack file it came from (for ge_texrep_image_uploaded)
};

enum class GeTexrepResult {
    None,     // no replacement (not in the pack, ignored, or the file failed)
    Pending,  // in the pack, decoding on the worker thread: draw the original
    Ready,    // `out` filled (pointer valid until the next call)
};

/// On a texture-cache miss (or a pending entry's retry): look the live texture
/// up in the pack. PNGs decode on a worker thread and stay in a CPU cache
/// (ge_texrep_budget_bytes(), least recently used evicted); a file that fails
/// to load is logged once and the original texture is used.
GeTexrepResult ge_texrep_replacement(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v,
                                     GeTexrepImage* out);

/// Render thread, right after `img` was copied into a GL texture: the CPU
/// buffer is no longer needed and the budget drops it now (a later request of
/// the same file decodes it again from disk).
void ge_texrep_image_uploaded(const GeTexrepImage& img);

/// Changes whenever a pack image finishes decoding: pending cache entries
/// retry only then.
uint64_t ge_texrep_epoch();
