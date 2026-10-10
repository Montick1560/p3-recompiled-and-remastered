#include "psp_texrep.h"

#include "psp_memory.h"

namespace {
GeClutSnapshot g_clut{};
}  // namespace

void ge_texrep_on_loadclut(const uint8_t* rdram, uint32_t clut_addr, uint32_t loadclut_data) {
    const uint32_t bytes = ge_clut_load_bytes(loadclut_data);
    const uint32_t off = clut_addr & PSP_ADDR_MASK;
    const bool valid = clut_addr != 0 && static_cast<uint64_t>(off) + bytes <= PSP_MEM_SIZE;
    ge_clut_snapshot_load(g_clut, valid ? rdram + off : nullptr, bytes);
}

const GeClutSnapshot& ge_texrep_clut() { return g_clut; }
