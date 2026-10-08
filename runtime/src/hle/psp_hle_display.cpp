#include "hle/psp_hle.h"
#include "hle/psp_hle_intr.h"
#include "psp_vblank_clock.h"
#include "psp_render_queue.h"
#include "psp_scheduler.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdio>
#include <thread>
#include <chrono>

// ---- State ----
static uint32_t g_fb_addr = 0;
static uint32_t g_fb_stride = 512;
static uint32_t g_fb_format = 3;  // 8888

// ---- HLE Functions ----

static void hle_sceDisplaySetFrameBuf(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t fb_addr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t stride = static_cast<uint32_t>(ctx->r[5]);
    uint32_t format = static_cast<uint32_t>(ctx->r[6]);
    uint32_t sync_mode = static_cast<uint32_t>(ctx->r[7]);

    (void)sync_mode;

    g_fb_addr = fb_addr;
    g_fb_stride = stride;
    g_fb_format = format;

    std::fprintf(stderr,
        "[HLE] sceDisplaySetFrameBuf(0x%08X, %u, %u)\n",
        fb_addr, stride, format);

    // Post a FramePresent render request to the render queue
    RenderRequest req;
    req.type = RenderRequestType::FramePresent;
    req.rdram = rdram;
    req.fb_addr = fb_addr;
    req.fb_stride = stride;
    req.fb_format = format;
    render_queue_post(req);

    ctx->r[2] = SCE_OK;
}

static void hle_sceDisplayWaitVblankStart(
    uint8_t* rdram, recomp_context* ctx
) {
    sched_yield_point();

    // Block until the next vblank boundary of the 59.94 Hz display clock
    // (not a fixed period after the call: the frame's own work time is
    // part of the frame, as on hardware).
    std::this_thread::sleep_for(std::chrono::microseconds(
        psp_us_until_next_vblank(psp_display_elapsed_us())));
    // Vblank interrupt: games hang per-frame work (e.g. waking a loader
    // thread) on PSP_VBLANK_INT sub-interrupt handlers.
    psp_intr_dispatch_vblank(rdram, ctx);

    ctx->r[2] = SCE_OK;
}

static void hle_sceDisplayWaitVblankStartCB(
    uint8_t* rdram, recomp_context* ctx
) {
    hle_sceDisplayWaitVblankStart(rdram, ctx);
}

static void hle_sceDisplayWaitVblank(
    uint8_t* rdram, recomp_context* ctx
) {
    hle_sceDisplayWaitVblankStart(rdram, ctx);
}

static void hle_sceDisplayGetFrameBuf(
    uint8_t* rdram, recomp_context* ctx
) {
    uint32_t fb_addr_ptr = static_cast<uint32_t>(ctx->r[4]);
    uint32_t stride_ptr = static_cast<uint32_t>(ctx->r[5]);
    uint32_t format_ptr = static_cast<uint32_t>(ctx->r[6]);

    if (fb_addr_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, fb_addr_ptr, g_fb_addr);
    }
    if (stride_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, stride_ptr, g_fb_stride);
    }
    if (format_ptr != 0) {
        psp_mem_write<uint32_t>(rdram, format_ptr, g_fb_format);
    }

    ctx->r[2] = SCE_OK;
}

static void hle_sceDisplayGetVcount(
    uint8_t* rdram, recomp_context* ctx
) {
    ctx->r[2] = static_cast<int32_t>(
        psp_vblank_index(psp_display_elapsed_us()));
    (void)rdram;
}

static void hle_sceDisplaySetMode(
    uint8_t* rdram, recomp_context* ctx
) {
    std::fprintf(stderr, "[HLE] sceDisplaySetMode(%d, %d, %d)\n",
        (int)ctx->r[4], (int)ctx->r[5], (int)ctx->r[6]);
    ctx->r[2] = SCE_OK;
    (void)rdram;
}

static void hle_sceDisplayGetFramePerSec(
    uint8_t* rdram, recomp_context* ctx
) {
    // The MIPS ABI returns floats in $f0 (callers convert from there).
    // v0 also carries the bit pattern, as before, for any caller that
    // reads the integer register.
    union { float f; int32_t i; } u;
    u.f = 59.94f;
    ctx->f[0] = u.f;
    ctx->r[2] = u.i;
    (void)rdram;
}

// ---- Registration ----

void psp_hle_register_display() {
    psp_hle_register("sceDisplaySetFrameBuf",
                      hle_sceDisplaySetFrameBuf);
    psp_hle_register("sceDisplayWaitVblankStart",
                      hle_sceDisplayWaitVblankStart);
    psp_hle_register("sceDisplayWaitVblankStartCB",
                      hle_sceDisplayWaitVblankStartCB);
    psp_hle_register("sceDisplayWaitVblank",
                      hle_sceDisplayWaitVblank);
    psp_hle_register("sceDisplayGetFrameBuf",
                      hle_sceDisplayGetFrameBuf);
    psp_hle_register("sceDisplayGetVcount",
                      hle_sceDisplayGetVcount);
    psp_hle_register("sceDisplaySetMode",
                      hle_sceDisplaySetMode);
    psp_hle_register("sceDisplayGetFramePerSec",
                      hle_sceDisplayGetFramePerSec);
}
