#pragma once
#include "psp_ge_constants.h"
#include <cstdint>
#include <cstring>

/// Maximum call stack depth for display list CALL/RET commands.
static constexpr int GE_CALL_STACK_SIZE = 32;

/// Maximum number of GE commands processed per display list (safety valve).
static constexpr int GE_MAX_COMMANDS = 4 * 1024 * 1024;

/// GE state machine -- tracks all state-setting commands for rendering.
/// Updated by the command decoder; read by vertex, texture, and shader code.
/// GE relative address (PPSSPP GPUStateCache::getRelativeAddress) used by
/// VADDR, IADDR, JUMP, BJUMP and CALL: the 24-bit command data extended with
/// BASE bits 16-19 as address bits 24-27, plus the OFFSETADDR / ORIGIN
/// offset, wrapped to 28 bits. `base_data` is the raw BASE command data.
inline uint32_t ge_relative_address(uint32_t base_data, uint32_t offset,
                                    uint32_t data) {
    uint32_t base_extended = ((base_data & 0x000F0000u) << 8) |
                             (data & 0x00FFFFFFu);
    return (offset + base_extended) & 0x0FFFFFFFu;
}

struct GeState {
    // -- Address state --
    uint32_t base_addr;       // GE_CMD_BASE raw data (bits 16-19 = address bits 24-27)
    uint32_t vertex_addr;     // GE_CMD_VADDR: vertex buffer address
    uint32_t index_addr;      // GE_CMD_IADDR: index buffer address
    uint32_t offset_addr;     // GE_CMD_OFFSETADDR / ORIGIN

    // -- Vertex format --
    uint32_t vertex_type;     // GE_CMD_VERTEXTYPE: raw VTYPE bitfield

    // -- Matrices --
    float world_matrix[12];   // 4x3 column-major (world transform)
    float view_matrix[12];    // 4x3 column-major (view transform)
    float proj_matrix[16];    // 4x4 column-major (projection)
    float tgen_matrix[12];    // 4x3 column-major (texgen)
    int world_mtx_num;        // Current write index for WORLDMATRIXDATA
    int view_mtx_num;         // Current write index for VIEWMATRIXDATA
    int proj_mtx_num;         // Current write index for PROJMATRIXDATA
    int tgen_mtx_num;         // Current write index for TGENMATRIXDATA

    // -- Texture state --
    uint32_t tex_addr[8];     // GE_CMD_TEXADDR0-7: mipmap addresses
    uint32_t tex_bufw[8];     // GE_CMD_TEXBUFWIDTH0-7: buffer widths
    uint32_t tex_size[8];     // GE_CMD_TEXSIZE0-7: log2_w | (log2_h << 8)
    uint32_t tex_format;      // GE_CMD_TEXFORMAT: GETextureFormat value
    uint32_t tex_mode;        // GE_CMD_TEXMODE: bit 0 = swizzle
    uint32_t tex_func;        // GE_CMD_TEXFUNC: GeTexFunc value (bits 0-2)
    bool tex_alpha;           // GE_CMD_TEXFUNC bit 8 (TCC): texture alpha used (RGBA)
    bool tex_color_double;    // GE_CMD_TEXFUNC bit 16: fragment RGB doubled
    uint32_t tex_func_raw;    // GE_CMD_TEXFUNC data as written (diagnostics)
    uint32_t tex_filter;      // GE_CMD_TEXFILTER
    uint32_t tex_wrap;        // GE_CMD_TEXWRAP
    uint32_t tex_env_color;   // GE_CMD_TEXENVCOLOR
    bool texture_enable;      // GE_CMD_TEXTUREMAPENABLE

    // -- CLUT state --
    uint32_t clut_addr;       // GE_CMD_CLUTADDR (low 24 bits)
    uint32_t clut_addr_upper; // GE_CMD_CLUTADDRUPPER (high bits)
    uint32_t clut_format;     // GE_CMD_CLUTFORMAT: palette format + shift

    // -- Viewport / screen --
    float viewport_x_scale, viewport_y_scale, viewport_z_scale;
    float viewport_x_center, viewport_y_center, viewport_z_center;
    float tex_scale_u, tex_scale_v;
    float tex_offset_u, tex_offset_v;
    uint32_t offset_x, offset_y;    // GE_CMD_OFFSETX/Y (1/16 subpixel)
    uint32_t region1, region2;       // GE_CMD_REGION1/2
    uint32_t scissor1, scissor2;     // GE_CMD_SCISSOR1/2
    uint32_t min_z, max_z;           // GE_CMD_MINZ/MAXZ: depth range (u16)

    // -- Framebuffer --
    uint32_t framebuf_ptr;    // GE_CMD_FRAMEBUFPTR
    uint32_t framebuf_width;  // GE_CMD_FRAMEBUFWIDTH
    uint32_t framebuf_format; // GE_CMD_FRAMEBUFPIXFORMAT

    // -- Render state --
    bool alpha_blend_enable;
    uint32_t blend_mode;      // GE_CMD_BLENDMODE: src|dst|op packed
    uint32_t blend_fix_a;     // GE_CMD_BLENDFIXEDA
    uint32_t blend_fix_b;     // GE_CMD_BLENDFIXEDB

    bool depth_test_enable;
    uint32_t depth_func;      // GE_CMD_ZTEST
    bool depth_write_disable; // GE_CMD_ZWRITEDISABLE

    bool alpha_test_enable;
    uint32_t alpha_test;      // GE_CMD_ALPHATEST: func|ref|mask packed

    bool stencil_test_enable;
    uint32_t stencil_test;    // GE_CMD_STENCILTEST
    uint32_t mask_rgb;        // GE_CMD_MASKRGB (1 bits = keep framebuffer)
    uint32_t mask_alpha;      // GE_CMD_MASKALPHA
    uint32_t stencil_op;      // GE_CMD_STENCILOP

    bool cull_enable;
    uint32_t cull_face;       // GE_CMD_CULL: 0=CW, 1=CCW

    bool clear_mode;          // GE_CMD_CLEARMODE bit 0
    uint32_t clear_flags;     // GE_CMD_CLEARMODE bits [10:8]

    // -- Lighting (per-vertex, psp_ge_lighting.h) --
    bool lighting_enable;
    uint32_t shade_mode;      // 0=flat, 1=gouraud
    uint32_t ambient_color;   // GE_CMD_AMBIENTCOLOR (0xBBGGRR)
    uint32_t ambient_alpha;   // GE_CMD_AMBIENTALPHA
    uint32_t material_emissive;
    uint32_t material_ambient;
    uint32_t material_alpha;  // GE_CMD_MATERIALALPHA (alpha of colorless verts)
    uint32_t material_diffuse;
    uint32_t material_specular;     // GE_CMD_MATERIALSPECULAR (0xBBGGRR)
    float material_specular_coef;   // GE_CMD_MATERIALSPECULARCOEF (pow exponent)
    uint32_t material_update;       // GE_CMD_MATERIALUPDATE bits 1/2/4 amb/diff/spec
    uint32_t light_mode;            // GE_CMD_LIGHTMODE (bit 0 separate-specular)
    bool reverse_normal;            // GE_CMD_REVERSENORMAL
    bool light_enable[4];           // GE_CMD_LIGHTENABLE0-3
    uint32_t light_type[4];         // GE_CMD_LIGHTTYPE0-3 raw (comp [1:0], type [9:8])
    float light_pos[4][3];          // GE_CMD_LX/LY/LZ0-3
    float light_dir[4][3];          // GE_CMD_LDX/LDY/LDZ0-3 (spot axis)
    float light_att[4][3];          // GE_CMD_LKA/LKB/LKC0-3 attenuation
    float light_spot_exp[4];        // GE_CMD_LKS0-3 spot exponent
    float light_spot_cutoff[4];     // GE_CMD_LKO0-3 spot cutoff
    uint32_t light_ambient[4];      // GE_CMD_LAC0-3 (0xBBGGRR)
    uint32_t light_diffuse[4];      // GE_CMD_LDC0-3 (0xBBGGRR)
    uint32_t light_specular[4];     // GE_CMD_LSC0-3 (0xBBGGRR)

    // -- Fog --
    bool fog_enable;
    uint32_t fog_color;

    // -- Log-once tracking --
    bool cmd_warned[256];     // One flag per command ID

    /// Zero-initialize all fields.
    void reset() { std::memset(this, 0, sizeof(*this)); }
};

/// Initialize the GE subsystem. Call once before any display list processing.
void ge_init();

/// Register the GE signal callback (called from sceGeSetCallback HLE).
/// Fired when a display list encounters a SIGNAL GE command.
/// signal_func: PSP virtual address of the signal handler function.
/// signal_arg:  passed as a1 to the handler.
/// cb_uid:      the UID returned to the game (passed as a0).
void ge_set_signal_callback(
    uint8_t* rdram, uint32_t signal_func,
    uint32_t signal_arg, int cb_uid);

/// Register the GE finish callback (called from sceGeSetCallback HLE).
/// Fired when a display list executes a FINISH GE command.
/// finish_func: PSP virtual address of the finish handler function.
///              Receives a0 = FINISH data & 0xffff, a1 = finish_arg.
void ge_set_finish_callback(
    uint32_t finish_func, uint32_t finish_arg);

/// Shutdown the GE subsystem. Call during cleanup.
void ge_shutdown();

/// Process a display list starting at list_addr in rdram.
/// Result from processing a display list.
struct GeListResult {
    uint32_t stopped_pc = 0;  // PC where processing stopped
    bool completed = false;    // True if END was reached
    int prim_count = 0;        // Number of PRIM commands processed
};

/// Reads 32-bit command words, dispatches via switch, updates GeState.
/// Returns when END command is reached or stall_addr is hit (if nonzero).
GeListResult ge_process_display_list(
    uint8_t* rdram,
    uint32_t list_addr,
    uint32_t stall_addr
);

/// Read-only access to the current GE state (for vertex/texture/shader).
const GeState& ge_get_state();

/// Mutable access to GE state (for ge.cpp internal + draw wiring).
GeState& ge_get_state_mut();
