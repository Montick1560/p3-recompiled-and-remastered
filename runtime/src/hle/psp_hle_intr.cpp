#include "psp_cpu.h"
#include "hle/psp_hle_intr.h"
#include "psp_vblank_clock.h"

#include <atomic>
#include <chrono>
#include <cstdio>

PspSubIntrTable& psp_subintr_table() {
    static auto* table = new PspSubIntrTable();  // leaked: used until exit
    return *table;
}

int64_t psp_display_elapsed_us() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

void psp_intr_dispatch_vblank(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<int64_t> last_frame{-1};

    const int64_t frame = psp_vblank_index(psp_display_elapsed_us());
    int64_t prev = last_frame.load();
    if (frame <= prev || !last_frame.compare_exchange_strong(prev, frame)) {
        return;
    }

    for (const PspSubIntrHandler& h :
         psp_subintr_table().enabled(PSP_INTR_VBLANK)) {
        FuncPtr fn = RECOMP_LOOKUP(h.handler);
        if (!fn) {
            static int miss_log_count = 0;
            if (miss_log_count++ < 8) {
                std::fprintf(stderr,
                    "[HLE] vblank subintr %d: LOOKUP_MISS handler=0x%08X\n",
                    h.sub, h.handler);
            }
            continue;
        }
        // Interrupt handlers run on the waiting thread's stack, below its
        // frame; the whole register file is restored afterwards so the
        // interrupted HLE call returns as if nothing ran.
        const recomp_context saved = *ctx;
        ctx->r[4] = h.sub;
        ctx->r[5] = static_cast<int32_t>(h.arg);
        ctx->r[29] = static_cast<int32_t>(
            (static_cast<uint32_t>(saved.r[29]) - 0x40u) & ~0xFu);
        ctx->r[31] = 0;
        { PspCpuAcquireScope cpu; fn(rdram, ctx); }  // guest code needs the CPU
        *ctx = saved;
    }
}
