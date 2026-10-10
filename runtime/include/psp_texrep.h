#pragma once
// Texture replacement from a PPSSPP-format pack (PSPRECOMP_TEXTURES=<dir>).
// Render (GE) thread only. Spec: docs/superpowers/specs/2026-10-10-m3c-hd-textures-design.md
#include "psp_ge.h"
#include "psp_texrep_hash.h"
#include "psp_texrep_pack.h"

#include <cstdint>

/// GE_CMD_LOADCLUT: snapshot the palette bytes the replacement key hashes.
void ge_texrep_on_loadclut(const uint8_t* rdram, uint32_t clut_addr, uint32_t loadclut_data);

/// The current CLUT snapshot.
const GeClutSnapshot& ge_texrep_clut();

/// Read PSPRECOMP_TEXTURES (pack folder) and PSPRECOMP_TEXTURES_DUMP; load
/// <dir>/textures.ini plus the root listing. Logs one [TEXREP] line (silent
/// when the env is unset).
void ge_texrep_init();

/// True when a pack is loaded.
bool ge_texrep_enabled();

/// PPSSPP replacement key of the live level-0 texture. draw_max_v = largest V
/// (texels) the current through-mode draw samples, 0 if unknown.
GeTexrepKey ge_texrep_key(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v);

/// Key + pack lookup; under PSPRECOMP_TEXTURES_DUMP logs each distinct key
/// once: "[TEXREP] key=<24 hex> WxH fmt=N hit|miss|ignored".
TexrepFind ge_texrep_lookup(const uint8_t* rdram, const GeState& s, uint16_t draw_max_v,
                            GeTexrepKey* key_out);
