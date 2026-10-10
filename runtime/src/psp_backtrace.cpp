// Host call-stack dump for guest diagnostics. Recompiled guest functions are
// host functions, so the host stack is the guest call stack; the recompiled
// code never writes a usable $ra. Prints raw return addresses relative to the
// image base; build/tools/bt.py symbolizes them with llvm-nm.
#include <cstdio>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void psp_print_host_backtrace(const char* tag) {
#ifdef _WIN32
    void* frames[48];
    const USHORT n = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "[BT] %s:", tag);
    for (USHORT i = 0; i < n; i++) {
        std::fprintf(stderr, " +%llx",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - base));
    }
    std::fprintf(stderr, "\n");
#else
    (void)tag;
#endif
}

// Store watch (recomp.h MEM_W_WRITE): PSPRECOMP_STORE_WATCH=<hex value>.
#include <atomic>
#include <cstdlib>
#include <cstring>

uint32_t g_psp_store_watch = [] {
    const char* e = std::getenv("PSPRECOMP_STORE_WATCH");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
}();

// PSPRECOMP_STORE_WATCH_RANGE=<lo>-<hi> (hex) keeps only stores into [lo, hi);
// a backtrace is printed for word +4 of 16-byte-aligned blocks (object vtable
// slots), the address alone for the rest.
void psp_store_watch_hit(uint32_t addr, uint32_t val) {
    static const uint32_t lo_hi[2] = {
        [] { const char* e = std::getenv("PSPRECOMP_STORE_WATCH_RANGE");
             return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u; }(),
        [] { const char* e = std::getenv("PSPRECOMP_STORE_WATCH_RANGE");
             const char* d = e ? std::strchr(e, '-') : nullptr;
             return d ? static_cast<uint32_t>(std::strtoul(d + 1, nullptr, 16)) : 0xFFFFFFFFu; }(),
    };
    if (addr < lo_hi[0] || addr >= lo_hi[1]) return;
    static std::atomic<int> hits{0};
    const int n = hits.fetch_add(1);
    if (n >= 5000) return;
    std::fprintf(stderr, "[STORE-WATCH] store of 0x%08X to 0x%08X\n", val, addr);
    if ((addr & 0xF) == 4 && n < 400) psp_print_host_backtrace("store-watch");
}
