// Guest callback runner: one host thread runs queued guest calls with the
// guest CPU held, on a carved guest stack, like GE finish callbacks.
#include "psp_cpu.h"
#include "psp_vfpu.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_kernel.h"
#include "net/guest_callbacks.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr uint32_t kScratchSize = 64 * 1024;

GuestCallQueue& queue() {
    static auto* q = new GuestCallQueue();  // leaked: used until exit
    return *q;
}

void run(uint8_t* rdram, uint32_t stack_sp) {
    // Calls run one at a time, so one scratch buffer serves them all: data
    // is copied in right before its call and is not touched until the next.
    const uint32_t scratch = psp_alloc_kernel_memory(kScratchSize);
    GuestCall c{};
    for (;;) {
        if (!queue().pop(&c, 1000)) continue;
        FuncPtr fn = RECOMP_LOOKUP(c.fn);
        if (!fn) {
            std::fprintf(stderr, "[ADHOC] callback 0x%08X: LOOKUP_MISS\n", c.fn);
            continue;
        }
        if (!c.buf.empty()) {
            if (scratch == 0 || c.buf.size() > kScratchSize) {
                std::fprintf(stderr, "[ADHOC] callback 0x%08X: %zu-byte buffer dropped\n", c.fn, c.buf.size());
                continue;
            }
            std::memcpy(rdram + (scratch & 0x07FFFFFFU), c.buf.data(), c.buf.size());
        }
        uint32_t args[8];
        guest_call_resolve(c, scratch, args);
        recomp_context ctx{};
        vfpu_init_context(&ctx);
        for (int i = 0; i < 8; i++) ctx.r[4 + i] = static_cast<int32_t>(args[i]);
        ctx.r[28] = static_cast<int32_t>(psp_get_boot_module_gp());
        ctx.r[29] = static_cast<int32_t>(stack_sp);
        ctx.r[31] = 0;
        PspCpuAcquireScope cpu;
        fn(rdram, &ctx);
    }
}

}  // namespace

void guest_callbacks_post(uint8_t* rdram, GuestCall call) {
    static std::once_flag started;
    std::call_once(started, [rdram] {
        const uint32_t sp = psp_alloc_stack(0x4000).sp;
        std::fprintf(stderr, "[ADHOC] callback runner started (sp=0x%08X)\n", sp);
        std::thread(run, rdram, sp).detach();
    });
    queue().push(std::move(call));
}
