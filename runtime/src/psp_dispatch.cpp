#include "recomp.h"
#include "psp_overlay.h"
#include "psp_scheduler.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_kernel.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <algorithm>

/// Cached STRICT mode flag — checked once on first call.
static bool g_strict_mode = false;
static bool g_strict_checked = false;

/// Per-address invocation counter for LOOKUP_MISS diagnostics.
/// Logs each unique address on first hit only to reduce noise.
static std::unordered_map<uint32_t, int> g_miss_counts;

/// Last function address from psp_trace_checkpoint (defined below, always recorded).
thread_local uint32_t g_last_func_addr = 0;

/// No-op stub returned when a lookup miss occurs in non-STRICT mode.
/// Sets v0 (ctx->r[2]) to 0 for deterministic return value behavior.
static thread_local uint32_t g_last_miss_addr = 0;

// [V438] ring buffer of recent function entries (poor-man's backtrace).
// Declared here so noop_stub (below) can read it; written by
// psp_trace_checkpoint. Cheap: one array write per checkpoint.
thread_local uint32_t g_func_ring[32] = {0};
thread_local uint32_t g_func_ring_pos = 0;

// [#35] Cross-thread copy of the dispatched-function ring for the debug
// socket's I command. The per-thread g_func_ring above is thread_local and
// unreadable from the socket thread, so psp_trace_checkpoint also appends
// to this shared ring with relaxed atomics (interleaves all game threads;
// ordering across threads is approximate -- diagnostics only).
static std::atomic<uint32_t> g_shared_func_ring[64];
static std::atomic<uint32_t> g_shared_func_ring_pos{0};

// [#35] LOOKUP_MISS counters readable from the debug socket thread.
// The g_miss_counts map below is mutated without a lock from game threads,
// so the socket must NOT iterate it (rehash mid-read). These atomics carry
// the two numbers the I command needs.
static std::atomic<uint32_t> g_miss_unique{0};
static std::atomic<uint64_t> g_miss_total{0};

void psp_dispatch_get_miss_stats(uint32_t* unique_addrs,
                                 uint64_t* total_calls) {
    if (unique_addrs)
        *unique_addrs = g_miss_unique.load(std::memory_order_relaxed);
    if (total_calls)
        *total_calls = g_miss_total.load(std::memory_order_relaxed);
}

int psp_dispatch_get_recent_funcs(uint32_t* out, int max) {
    uint32_t pos = g_shared_func_ring_pos.load(std::memory_order_relaxed);
    int n = 0;
    // Oldest first: walk forward from the slot the next write would claim.
    for (int i = 0; i < 64 && n < max; i++) {
        uint32_t v = g_shared_func_ring[(pos + static_cast<uint32_t>(i)) & 63u]
                         .load(std::memory_order_relaxed);
        if (v) out[n++] = v;
    }
    return n;
}

// Game-module miss handler (#47 P5 seam): everything address-keyed that
// used to live inline here (Patapon's IO-slot dump, [BND_VTABLE_MISS]
// punch list, the 0x438 corrupt-vtable workaround) registers through this
// slot from the game module's register_hooks. nullptr = no handler.
static PspLookupMissHandler g_miss_handler = nullptr;

void psp_dispatch_set_miss_handler(PspLookupMissHandler fn) {
    g_miss_handler = fn;
}

void psp_print_host_backtrace(const char* tag);  // psp_backtrace.cpp

static void noop_stub(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = g_last_miss_addr;
    int& c = g_miss_counts[addr];
    if (c <= 5) {
        std::fprintf(stderr,
            "[LOOKUP_MISS_CTX] addr=0x%08X caller=0x%08X a0=0x%08X a1=0x%08X sp=0x%08X"
            " r16=0x%08X r17=0x%08X r21=0x%08X\n",
            addr,
            g_last_func_addr,
            static_cast<uint32_t>(ctx->r[4]),
            static_cast<uint32_t>(ctx->r[5]),
            static_cast<uint32_t>(ctx->r[29]),
            static_cast<uint32_t>(ctx->r[16]),
            static_cast<uint32_t>(ctx->r[17]),
            static_cast<uint32_t>(ctx->r[21]));
        if (c == 1) {
            // Which functions led here (oldest first; shared across threads).
            uint32_t ring[64];
            int n = psp_dispatch_get_recent_funcs(ring, 64);
            std::fprintf(stderr, "[LOOKUP_MISS_RECENT] addr=0x%08X ra=0x%08X:", addr,
                         static_cast<uint32_t>(ctx->r[31]));
            for (int i = n > 32 ? n - 32 : 0; i < n; i++) {
                std::fprintf(stderr, " %08X", ring[i]);
            }
            std::fprintf(stderr, "\n");
            char tag[40];
            std::snprintf(tag, sizeof(tag), "miss %08X", addr);
            psp_print_host_backtrace(tag);  // the real caller (build/tools/bt.py)
        }
    }

    // Game-module miss handler (#47 P5 seam) — may log address-keyed
    // diagnostics and apply title-specific workarounds before the generic
    // deterministic return value below.
    if (g_miss_handler != nullptr) {
        g_miss_handler(rdram, ctx, addr, c);
    }

    ctx->r[2] = 0;
}
static bool g_miss_atexit_registered = false;

/// Dump all LOOKUP_MISS addresses sorted by count at program exit.
/// Output format is suitable for feeding back into Ghidra analysis
/// as "force function start" hints for future dispatch table expansion.
static void psp_dump_lookup_misses() {
    if (g_miss_counts.empty()) return;

    // Sort by count descending
    std::vector<std::pair<uint32_t, int>> sorted(
        g_miss_counts.begin(), g_miss_counts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

    int total_misses = 0;
    for (const auto& [addr, count] : sorted) {
        total_misses += count;
    }

    std::fprintf(stderr,
        "\n[LOOKUP_MISS_SUMMARY] %zu unique addresses, "
        "%d total calls\n",
        sorted.size(),
        total_misses);

    for (const auto& [addr, count] : sorted) {
        std::fprintf(stderr,
            "  0x%08X (count=%d)\n", addr, count);
    }

    std::fprintf(stderr,
        "[LOOKUP_MISS_SUMMARY] END (%zu addresses)\n",
        sorted.size());
}

// [#47 P1] Probe mode: psp_dispatch_probe_lookup() resolves an address
// WITHOUT engaging the miss machinery — no log, no counters, no STRICT
// abort — and yields nullptr when the address is absent. Used at boot
// (single-threaded, right after psp_init_dispatch_table()) to resolve the
// guest functions that runtime hooks wrap, replacing the former link-time
// `extern FUN_*` references into the generated output. Not thread-safe by
// design; boot-time only.
static bool g_probe_lookup = false;

FuncPtr psp_dispatch_probe_lookup(uint32_t vaddr) {
    g_probe_lookup = true;
    FuncPtr fn = RECOMP_LOOKUP(vaddr);
    g_probe_lookup = false;
    return fn;
}

// PSPRECOMP_OVL_ARGS=hex[,hex...]: like PSPRECOMP_FUNC_ARGS, for code in an
// overlay bank (resolved per call, so it cannot be wrapped at boot). Logs
// a0-a3, the calling thread's last functions and v0 for the first 200 calls.
static constexpr int OVL_ARGS_MAX = 8;
static uint32_t g_ovl_args_addr[OVL_ARGS_MAX];
static FuncPtr g_ovl_args_real[OVL_ARGS_MAX];
static std::atomic<int> g_ovl_args_calls[OVL_ARGS_MAX];
static int g_ovl_args_count = -1;


static void ovl_args_call(int slot, uint8_t* rdram, recomp_context* ctx) {
    const bool log = g_ovl_args_calls[slot].fetch_add(1, std::memory_order_relaxed) < 200;
    const uint32_t addr = g_ovl_args_addr[slot];
    if (log) {
        std::fprintf(stderr, "[OVL-ARGS-IN] 0x%08X(a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X) path:",
                     addr, (uint32_t)ctx->r[4], (uint32_t)ctx->r[5], (uint32_t)ctx->r[6],
                     (uint32_t)ctx->r[7]);
        for (uint32_t k = 12; k >= 1; k--) {
            const uint32_t f = g_func_ring[(g_func_ring_pos - k) & 31u];
            if (f) std::fprintf(stderr, " %08X", f);
        }
        std::fprintf(stderr, "\n");
        psp_print_host_backtrace("ovl-args");
    }
    g_ovl_args_real[slot](rdram, ctx);
    if (log) {
        std::fprintf(stderr, "[OVL-ARGS-OUT] 0x%08X -> v0=0x%08X\n", addr, (uint32_t)ctx->r[2]);
    }
}

#define OVL_ARGS_WRAPPER(N) \
    static void ovl_args_wrapper_##N(uint8_t* rdram, recomp_context* ctx) { \
        ovl_args_call(N, rdram, ctx); \
    }
OVL_ARGS_WRAPPER(0) OVL_ARGS_WRAPPER(1) OVL_ARGS_WRAPPER(2) OVL_ARGS_WRAPPER(3)
OVL_ARGS_WRAPPER(4) OVL_ARGS_WRAPPER(5) OVL_ARGS_WRAPPER(6) OVL_ARGS_WRAPPER(7)
#undef OVL_ARGS_WRAPPER

static FuncPtr ovl_args_intercept(uint32_t vaddr, FuncPtr fn) {
    static const FuncPtr wrappers[OVL_ARGS_MAX] = {
        ovl_args_wrapper_0, ovl_args_wrapper_1, ovl_args_wrapper_2, ovl_args_wrapper_3,
        ovl_args_wrapper_4, ovl_args_wrapper_5, ovl_args_wrapper_6, ovl_args_wrapper_7};
    if (g_ovl_args_count < 0) {
        g_ovl_args_count = 0;
        const char* env = std::getenv("PSPRECOMP_OVL_ARGS");
        while (env && *env && g_ovl_args_count < OVL_ARGS_MAX) {
            char* end = nullptr;
            const uint32_t a = static_cast<uint32_t>(std::strtoul(env, &end, 16));
            if (end == env) break;
            g_ovl_args_addr[g_ovl_args_count++] = a;
            env = (*end == ',') ? end + 1 : end;
        }
    }
    for (int i = 0; i < g_ovl_args_count; i++) {
        if (g_ovl_args_addr[i] == vaddr) {
            g_ovl_args_real[i] = fn;  // the bank loaded now (overlays share the window)
            return wrappers[i];
        }
    }
    return fn;
}

/// Called by RECOMP_LOOKUP (in dispatch.cpp) when an address is not in the
/// dispatch table. Logs each unique miss on first hit and returns a noop
/// stub or aborts if PSPRECOMP_STRICT=1.
FuncPtr psp_on_lookup_miss(uint32_t vaddr) {
    // Probe mode (#47 P1): report "absent" to psp_dispatch_probe_lookup
    // without any side effects.
    if (g_probe_lookup) {
        return nullptr;
    }

    // Code overlays (M2): an address inside an overlay window belongs to the
    // bank of the overlay currently loaded there. This is the hot path for
    // overlay code (every call inside an overlay lands here), so it runs
    // before the miss bookkeeping below.
    {
        FuncPtr fn = nullptr;
        const PspOverlayStatus st = psp_overlay_resolve(psp_overlay_rdram(), vaddr, &fn);
        static const bool trace = [] {
            const char* e = std::getenv("PSPRECOMP_OVERLAY_TRACE");
            return e && e[0] == '1';
        }();
        static std::atomic<int> traced{0};
        if (trace && st != PspOverlayStatus::NotInWindow && traced.fetch_add(1) < 64) {
            std::fprintf(stderr, "[OVERLAY] resolve 0x%08X -> status %d (rdram=%p)\n",
                         vaddr, static_cast<int>(st), static_cast<void*>(psp_overlay_rdram()));
        }
        switch (st) {
        case PspOverlayStatus::Found:
            return ovl_args_intercept(vaddr, fn);
        case PspOverlayStatus::HashMismatch:
            std::fprintf(stderr,
                "[OVERLAY] FATAL: the overlay loaded at the window of 0x%08X does "
                "not match the code it was recompiled from (did the game files "
                "change?). Rebuild with: games/patapon3/scripts/p3.sh all\n",
                vaddr);
            std::fflush(stderr);
            std::abort();
        default:
            break;  // not an overlay address / no bank: ordinary miss
        }
    }

    // One-time check for STRICT mode environment variable
    if (!g_strict_checked) {
        const char* env = std::getenv("PSPRECOMP_STRICT");
        g_strict_mode = (env && env[0] == '1');
        g_strict_checked = true;
    }

    // Register atexit handler on first miss
    if (!g_miss_atexit_registered) {
        g_miss_atexit_registered = true;
        std::atexit(psp_dump_lookup_misses);
    }

    // Per-address miss counting -- log on first hit only
    int& count = g_miss_counts[vaddr];
    count++;
    g_miss_total.fetch_add(1, std::memory_order_relaxed);
    if (count == 1) {
        g_miss_unique.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
            "[LOOKUP_MISS] addr=0x%08X (first hit)\n", vaddr);
    }

    // Store for noop_stub context logging
    g_last_miss_addr = vaddr;

    // STRICT mode: abort on any lookup miss (RUNTIME-11)
    if (g_strict_mode) {
        std::fprintf(stderr,
            "STRICT: aborting on LOOKUP_MISS 0x%08X\n", vaddr);
        std::abort();
    }

    return noop_stub;
}

// Forward declaration for atexit registration
static void psp_dump_pc_trace();

/// PC tracing checkpoint — called at every generated function entry.
/// When PSPRECOMP_PC_TRACE=1, logs function address with frequency counting.
/// No-op when env var is absent or not "1" (single branch cost).
static bool g_pc_trace = false;
static bool g_pc_trace_checked = false;
static std::unordered_map<uint32_t, int> g_func_counts;
static std::mutex g_pc_trace_mutex;
static int g_total_entries = 0;

// Previous dispatched-function address, kept per thread so game-module
// diagnostics (games/<name>/hooks_dispatch.cpp) can report caller chains.
thread_local uint32_t g_prev_func_addr = 0;

// PSPRECOMP_FUNC_WATCH=<hex>[,<hex>...]: log every entry of the listed
// guest functions with the caller (previous checkpoint), first 200 hits each.
static constexpr int FUNC_WATCH_MAX = 16;
static uint32_t g_func_watch[FUNC_WATCH_MAX];
static std::atomic<int> g_func_watch_hits[FUNC_WATCH_MAX];
static int g_func_watch_count = 0;

static void func_watch_parse() {
    const char* env = std::getenv("PSPRECOMP_FUNC_WATCH");
    while (env && *env && g_func_watch_count < FUNC_WATCH_MAX) {
        char* end = nullptr;
        uint32_t a = static_cast<uint32_t>(std::strtoul(env, &end, 16));
        if (end == env) break;
        g_func_watch[g_func_watch_count++] = a;
        env = (*end == ',') ? end + 1 : end;
    }
}

static std::atomic<int> g_bt_request{-1};  // debug socket K <tid>

void psp_dispatch_request_backtrace(int tid) {
    g_bt_request.store(tid, std::memory_order_relaxed);
}

void psp_trace_checkpoint(uint32_t addr) {
    if (!g_pc_trace_checked) {
        const char* env = std::getenv("PSPRECOMP_PC_TRACE");
        g_pc_trace = (env && env[0] == '1');
        func_watch_parse();
        g_pc_trace_checked = true;
        if (g_pc_trace) {
            std::atexit(psp_dump_pc_trace);
        }
    }
    for (int i = 0; i < g_func_watch_count; i++) {
        if (g_func_watch[i] == addr &&
            g_func_watch_hits[i].fetch_add(1, std::memory_order_relaxed) < 200) {
            std::fprintf(stderr, "[FUNC-WATCH] 0x%08X from 0x%08X; this thread's path:",
                         addr, g_last_func_addr);
            for (uint32_t k = 16; k >= 1; k--) {
                const uint32_t f = g_func_ring[(g_func_ring_pos - k) & 31u];
                if (f) std::fprintf(stderr, " %08X", f);
            }
            std::fprintf(stderr, "\n");
        }
    }
    g_prev_func_addr = g_last_func_addr;
    g_last_func_addr = addr;
    // Per-thread last entry for the debug socket I command (the shared ring
    // below interleaves every thread).
    static thread_local PspThread* const self = psp_get_current_thread();
    if (self) self->last_func.store(addr, std::memory_order_relaxed);
    // Debug socket K <tid>: the thread dumps its guest call stack here.
    if (self && g_bt_request.load(std::memory_order_relaxed) == self->id) {
        g_bt_request.store(-1, std::memory_order_relaxed);
        char tag[32];
        std::snprintf(tag, sizeof(tag), "thread %d at %08X", self->id, addr);
        psp_print_host_backtrace(tag);
    }
    g_func_ring[(g_func_ring_pos++) & 31u] = addr;
    // [#35] shared (cross-thread) ring for the debug socket I command.
    // One relaxed fetch_add + store per function entry; measured noise is
    // acceptable for a diagnostics-first runtime.
    g_shared_func_ring[g_shared_func_ring_pos.fetch_add(
        1, std::memory_order_relaxed) & 63u]
        .store(addr, std::memory_order_relaxed);

    // (The PSPRECOMP_SPLEAK shadow-stack sp-leak detector that hung here was
    // a Patapon-tuned investigation probe — deleted in #47 Phase 5;
    // re-creatable from games/patapon/ if that hunt ever reopens.)
    if (!g_pc_trace) return;
    g_total_entries++;

    std::lock_guard<std::mutex> lock(g_pc_trace_mutex);
    int& c = g_func_counts[addr];
    c++;
    if (c <= 3 || c % 100000 == 0) {
        std::fprintf(stderr,
            "[PC-TRACE] func=0x%08X count=%d\n", addr, c);
    }
}

/// Dump the most-called functions sorted by count.
/// Called from a timer thread after a few seconds.
static void psp_dump_pc_trace() {
    std::lock_guard<std::mutex> lock(g_pc_trace_mutex);
    std::fprintf(stderr,
        "\n[PC-TRACE] === DUMP (total entries=%d, last=0x%08X) ===\n",
        g_total_entries, g_last_func_addr);

    // Sort by count descending
    std::vector<std::pair<uint32_t, int>> sorted(
        g_func_counts.begin(), g_func_counts.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    int shown = 0;
    for (const auto& [addr, count] : sorted) {
        std::fprintf(stderr,
            "[PC-TRACE]   0x%08X  calls=%d\n", addr, count);
        if (++shown >= 20) break;
    }
    std::fprintf(stderr, "[PC-TRACE] === END DUMP ===\n");
}

// ---- PSPRECOMP_FUNC_ARGS=<hex>[,<hex>...] (debug) ----
// Wraps up to 8 guest functions in the dispatch table: every call through
// RECOMP_LOOKUP logs a0-a3 on entry and v0 on return (first 400 calls each).
// Calls the emitter resolved to direct C++ calls bypass the wrapper.
static constexpr int FUNC_ARGS_MAX = 8;
static uint32_t g_func_args_addr[FUNC_ARGS_MAX];
static FuncPtr g_func_args_orig[FUNC_ARGS_MAX];
static std::atomic<int> g_func_args_calls[FUNC_ARGS_MAX];

void psp_dispatch_trace_rearm() {
    for (auto& c : g_func_args_calls) c.store(0, std::memory_order_relaxed);
    for (auto& c : g_func_watch_hits) c.store(0, std::memory_order_relaxed);
    for (auto& c : g_ovl_args_calls) c.store(0, std::memory_order_relaxed);
    std::fprintf(stderr, "[TRACE] function watch/args budgets re-armed\n");
}

static void func_args_call(int slot, uint8_t* rdram, recomp_context* ctx) {
    // PSPRECOMP_FUNC_ARGS_MAX=<n>: per-function log budget (default 400).
    static const int budget = [] {
        const char* e = std::getenv("PSPRECOMP_FUNC_ARGS_MAX");
        return e ? static_cast<int>(std::strtol(e, nullptr, 0)) : 400;
    }();
    const bool log = g_func_args_calls[slot].fetch_add(1, std::memory_order_relaxed) < budget;
    const uint32_t a[4] = {(uint32_t)ctx->r[4], (uint32_t)ctx->r[5],
                           (uint32_t)ctx->r[6], (uint32_t)ctx->r[7]};
    const uint32_t ra = (uint32_t)ctx->r[31];
    if (log) {
        // Entry line too: a call that never returns (a hang) still shows up.
        std::fprintf(stderr,
                     "[FUNC-ARGS-IN] 0x%08X(a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X) ra=0x%08X\n",
                     g_func_args_addr[slot], a[0], a[1], a[2], a[3], ra);
    }
    g_func_args_orig[slot](rdram, ctx);
    if (log) {
        std::fprintf(stderr,
                     "[FUNC-ARGS] 0x%08X(a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X) ra=0x%08X "
                     "-> v0=0x%08X\n",
                     g_func_args_addr[slot], a[0], a[1], a[2], a[3], ra, (uint32_t)ctx->r[2]);
        // PSPRECOMP_FUNC_ARGS_DUMP=<bytes>: also hex-dump guest memory at v0
        // (e.g. the block a lookup function returns, before it is reused).
        static const uint32_t dump = [] {
            const char* e = std::getenv("PSPRECOMP_FUNC_ARGS_DUMP");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) & ~3u : 0u;
        }();
        const uint32_t v0 = (uint32_t)ctx->r[2];
        for (uint32_t off = 0; dump && v0 && off < dump; off += 16) {
            std::fprintf(stderr, "[FUNC-ARGS]   %08X:", v0 + off);
            for (uint32_t k = off; k < off + 16 && k < dump; k += 4) {
                std::fprintf(stderr, " %08X", psp_mem_read<uint32_t>(rdram, v0 + k));
            }
            std::fprintf(stderr, "\n");
        }
    }
}

#define FUNC_ARGS_WRAPPER(N) \
    static void func_args_wrapper_##N(uint8_t* rdram, recomp_context* ctx) { \
        func_args_call(N, rdram, ctx); \
    }
FUNC_ARGS_WRAPPER(0) FUNC_ARGS_WRAPPER(1) FUNC_ARGS_WRAPPER(2) FUNC_ARGS_WRAPPER(3)
FUNC_ARGS_WRAPPER(4) FUNC_ARGS_WRAPPER(5) FUNC_ARGS_WRAPPER(6) FUNC_ARGS_WRAPPER(7)
#undef FUNC_ARGS_WRAPPER

void psp_func_args_install() {
    static const FuncPtr wrappers[FUNC_ARGS_MAX] = {
        func_args_wrapper_0, func_args_wrapper_1, func_args_wrapper_2, func_args_wrapper_3,
        func_args_wrapper_4, func_args_wrapper_5, func_args_wrapper_6, func_args_wrapper_7};
    const char* env = std::getenv("PSPRECOMP_FUNC_ARGS");
    int n = 0;
    while (env && *env && n < FUNC_ARGS_MAX) {
        char* end = nullptr;
        uint32_t addr = static_cast<uint32_t>(std::strtoul(env, &end, 16));
        if (end == env) break;
        FuncPtr orig = RECOMP_LOOKUP(addr);
        // Overlay-window code is not resolvable at boot (nothing loaded): the
        // lookup yields the miss stub, and wrapping it would shadow the bank.
        if (orig == noop_stub) {
            std::fprintf(stderr, "[FUNC-ARGS] 0x%08X not resolvable at boot (overlay?), skipped\n", addr);
            orig = nullptr;
        }
        if (orig) {
            g_func_args_addr[n] = addr;
            g_func_args_orig[n] = orig;
            psp_dispatch_register(addr, wrappers[n]);
            std::fprintf(stderr, "[FUNC-ARGS] watching 0x%08X\n", addr);
            n++;
        }
        env = (*end == ',') ? end + 1 : end;
    }
}
