#pragma once
// GE draw batching (psp_ge_draw.cpp): which GE commands must draw the pending batch before
// they apply, and strip/fan -> triangle-list indices. Pure, header-only, no GL.

#include "psp_ge_constants.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/// Commands the renderer consumes only on the CPU at PRIM time (decode, transform, lighting,
/// UV scale/offset, morph, list flow): vertices already carry their effect, so they never
/// split a batch. Checked by grep: none of these GeState fields is read by the draw-time code
/// (psp_ge_draw.cpp after packing, psp_ge_shader.cpp, psp_ge_texture.cpp).
inline bool ge_cmd_cpu_only(uint8_t cmd) {
    switch (cmd) {
    case GE_CMD_NOP: case 0xFF: case GE_CMD_PRIM: case GE_CMD_BOUNDINGBOX:
    case GE_CMD_BASE: case GE_CMD_OFFSETADDR: case GE_CMD_ORIGIN:
    case GE_CMD_VADDR: case GE_CMD_IADDR:
    case GE_CMD_JUMP: case GE_CMD_BJUMP: case GE_CMD_CALL: case GE_CMD_RET:
    case GE_CMD_WORLDMATRIXNUMBER: case GE_CMD_WORLDMATRIXDATA:
    case GE_CMD_VIEWMATRIXNUMBER: case GE_CMD_VIEWMATRIXDATA:
    case GE_CMD_PROJMATRIXNUMBER: case GE_CMD_PROJMATRIXDATA:
    case GE_CMD_TGENMATRIXNUMBER: case GE_CMD_TGENMATRIXDATA:
    case GE_CMD_BONEMATRIXNUMBER: case GE_CMD_BONEMATRIXDATA:
    case GE_CMD_TEXSCALEU: case GE_CMD_TEXSCALEV: case GE_CMD_TEXOFFSETU: case GE_CMD_TEXOFFSETV:
    case GE_CMD_FOG1: case GE_CMD_FOG2:   // the fog factor is baked per vertex
    case GE_CMD_LIGHTINGENABLE: case GE_CMD_LIGHTMODE: case GE_CMD_REVERSENORMAL:
    case GE_CMD_AMBIENTCOLOR: case GE_CMD_AMBIENTALPHA:
        return true;
    default:
        return (cmd >= GE_CMD_MORPHWEIGHT0 && cmd <= GE_CMD_MORPHWEIGHT7) ||
               (cmd >= GE_CMD_LIGHTENABLE0 && cmd <= GE_CMD_LIGHTENABLE3) ||
               (cmd >= GE_CMD_MATERIALUPDATE && cmd <= GE_CMD_MATERIALSPECULARCOEF) ||
               (cmd >= GE_CMD_LIGHTTYPE0 && cmd <= GE_CMD_LSC3);
    }
}

/// True when command `word` (previous word of the same command: `prev`) must draw the pending
/// batch before it applies. Port of PPSSPP's GPUCommonHW.cpp command flags: actions that read
/// or change memory/targets always flush; CPU-only state never; the rest when its word changes.
/// VERTEXTYPE matters at draw time only through its through-mode bit (viewport, fog).
inline bool ge_cmd_flushes(uint8_t cmd, uint32_t prev, uint32_t word) {
    switch (cmd) {
    case GE_CMD_LOADCLUT: case GE_CMD_TRANSFERSTART: case GE_CMD_TEXFLUSH: case GE_CMD_TEXSYNC:
    case GE_CMD_BEZIER: case GE_CMD_SPLINE: case GE_CMD_FINISH: case GE_CMD_END:
    case GE_CMD_SIGNAL: case GE_CMD_CLEARMODE:
        return true;
    case GE_CMD_VERTEXTYPE:
        return ((prev ^ word) & GE_VTYPE_THROUGH) != 0;
    default:
        return !ge_cmd_cpu_only(cmd) && prev != word;
    }
}

/// Append to `out` the triangle-list indices (into a source of `count` vertices) of a
/// GE_PRIM_TRIANGLES / _STRIP / _FAN (any other type: as a list). Strip odd triangles swap their
/// first two vertices (PPSSPP IndexGenerator::AddStrip) so culling sees GL's facing. Incomplete
/// tails are dropped.
inline void ge_triangle_list_indices(int prim_type, size_t count, std::vector<uint32_t>& out) {
    if (count < 3) return;
    for (uint32_t t = 0; t + 2 < count; ++t) {
        switch (prim_type) {
        default:   // GE_PRIM_TRIANGLES, and type 7 (PPSSPP KEEP_PREVIOUS) as the renderer always drew it
            if (t % 3 == 0) out.insert(out.end(), {t, t + 1, t + 2});
            break;
        case GE_PRIM_TRIANGLE_STRIP:
            if (t & 1) out.insert(out.end(), {t + 1, t, t + 2});
            else out.insert(out.end(), {t, t + 1, t + 2});
            break;
        case GE_PRIM_TRIANGLE_FAN:
            out.insert(out.end(), {0u, t + 1, t + 2});
            break;
        }
    }
}
