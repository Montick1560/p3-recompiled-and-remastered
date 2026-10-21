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

// Hardware write watch (Windows x64 debug registers): PSPRECOMP_HW_WATCH=<hex
// guest address>. Every write to that guest word, by recompiled code or by an
// HLE call (memcpy, IO reads, result pointers), prints
// "[HW-WATCH] guest 0x... = 0x... rip +off" plus a [BT] line. A watcher thread
// arms DR0/DR7 on every host thread of the process (re-scanned every 100 ms, so
// guest threads started later are covered). Finds the writer of a corrupted
// word when the value is 0 or too common for PSPRECOMP_STORE_WATCH.
#ifdef _WIN32
#include <tlhelp32.h>
#include <set>
#include <thread>
#include <chrono>

static std::atomic<uintptr_t> g_hw_watch_host{0};
static std::atomic<uint32_t> g_hw_watch_guest{0};
static std::atomic<int> g_hw_watch_hits{0};
static std::atomic<uint32_t> g_hw_watch_gen{0};  // bumped by a retarget: re-arm every thread
static uint8_t* g_hw_watch_rdram = nullptr;

static LONG CALLBACK hw_watch_handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    if ((c->Dr6 & 1) == 0) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    if (g_hw_watch_hits.fetch_add(1) < 400) {
        const uint32_t v = *reinterpret_cast<volatile uint32_t*>(g_hw_watch_host.load());
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::fprintf(stderr, "[HW-WATCH] guest 0x%08X = 0x%08X rip +%llx\n", g_hw_watch_guest.load(), v,
                     static_cast<unsigned long long>(
                         reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) - base));
        psp_print_host_backtrace("hw-watch");
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool hw_watch_arm(DWORD tid) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) return false;
    bool ok = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &c)) {
            c.Dr0 = g_hw_watch_host.load();
            c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);  // L0, write, 4 bytes
            ok = SetThreadContext(h, &c) != 0;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return ok;
}

static void hw_watch_set(uint32_t guest) {
    g_hw_watch_guest = guest & ~3u;
    g_hw_watch_host = reinterpret_cast<uintptr_t>(g_hw_watch_rdram) + ((guest & ~3u) & 0x07FFFFFFu);
    g_hw_watch_hits = 0;
    g_hw_watch_gen.fetch_add(1);
    std::fprintf(stderr, "[HW-WATCH] watching writes to guest 0x%08X\n", guest & ~3u);
}

static void hw_watch_thread_start() {
    AddVectoredExceptionHandler(1, hw_watch_handler);
    std::thread([] {
        const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        std::set<DWORD> armed;
        uint32_t gen = g_hw_watch_gen.load();
        for (;;) {
            if (g_hw_watch_gen.load() != gen) {  // retargeted: every thread again
                gen = g_hw_watch_gen.load();
                armed.clear();
            }
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                std::vector<DWORD> fresh;
                for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID == pid && te.th32ThreadID != self &&
                        !armed.count(te.th32ThreadID)) {
                        fresh.push_back(te.th32ThreadID);
                    }
                }
                CloseHandle(snap);
                for (DWORD t : fresh) {
                    if (hw_watch_arm(t)) armed.insert(t);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

void psp_hw_watch_start(uint8_t* rdram) {
    g_hw_watch_rdram = rdram;
    const char* e = std::getenv("PSPRECOMP_HW_WATCH");
    if (!e || !rdram) return;
    hw_watch_set(static_cast<uint32_t>(std::strtoul(e, nullptr, 16)));
    hw_watch_thread_start();
}

// Debug socket `H <hex>`: watch another guest word from now on (starts the
// watcher if PSPRECOMP_HW_WATCH was unset). Threads are re-armed within 100 ms.
bool psp_hw_watch_retarget(uint32_t guest) {
    if (!g_hw_watch_rdram || guest < 0x08000000u || guest >= 0x0A000000u) return false;
    static std::atomic<bool> started{false};
    const bool was_on = g_hw_watch_host.load() != 0;
    hw_watch_set(guest);
    if (!was_on && !started.exchange(true)) hw_watch_thread_start();
    return true;
}
#else
void psp_hw_watch_start(uint8_t*) {}
bool psp_hw_watch_retarget(uint32_t) { return false; }
#endif
