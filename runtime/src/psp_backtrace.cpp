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
