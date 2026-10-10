#pragma once
// Texture replacement from a PPSSPP-format pack (PSPRECOMP_TEXTURES=<dir>).
// Render (GE) thread only. Spec: docs/superpowers/specs/2026-10-10-m3c-hd-textures-design.md
#include "psp_texrep_hash.h"

#include <cstdint>

/// GE_CMD_LOADCLUT: snapshot the palette bytes the replacement key hashes.
void ge_texrep_on_loadclut(const uint8_t* rdram, uint32_t clut_addr, uint32_t loadclut_data);

/// The current CLUT snapshot.
const GeClutSnapshot& ge_texrep_clut();
