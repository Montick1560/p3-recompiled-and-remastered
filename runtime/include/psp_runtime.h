#pragma once
#include <atomic>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

/// Global shutdown flag — all threads and the event loop check this.
/// Set by signal handler (SIGTERM/SIGINT) or when all game threads exit.
extern std::atomic<bool> g_should_exit;

/// Count of alive game threads — event loop uses this to detect quiescence.
extern std::atomic<int> g_alive_threads;

/// Main thread ID — set once in main() before any GL init.
/// Used by GL_THREAD_CHECK to assert GL calls happen on the main thread.
extern std::thread::id g_main_thread_id;

/// Host input: PSP button mask driven by SDL keyboard events
/// (set/cleared in psp_event_loop.cpp; ORed in by the sceCtrl HLE).
extern std::atomic<uint32_t> g_host_buttons;

/// Host game controllers (psp_event_loop.cpp, SDL GameController API):
/// combined PSP button mask of every pad, and the analog stick of the most
/// recently moved pad (x low byte, y high byte; 0x8080 = centred).
extern std::atomic<uint32_t> g_pad_buttons;
extern std::atomic<uint16_t> g_pad_analog;

/// Mouse wheel as D-pad UP/DOWN (psp_wheel.h): the timed press the event loop
/// publishes every iteration; ORed in by the sceCtrl HLE.
extern std::atomic<uint32_t> g_wheel_buttons;

/// Debug-socket injected button mask + expiry (steady_clock milliseconds).
/// Set by the `B <hexmask> <ms>` debug-socket command; the sceCtrl HLE ORs
/// the mask in only while now < deadline (one-shot timed overlay).
extern std::atomic<uint32_t> g_injected_buttons;
extern std::atomic<int64_t>  g_injected_buttons_deadline_ms;

/// [#35] LOOKUP_MISS counters for the debug socket I command
/// (implemented in psp_dispatch.cpp; relaxed-atomic, any thread).
void psp_dispatch_get_miss_stats(uint32_t* unique_addrs,
                                 uint64_t* total_calls);

/// [#35] Recent dispatched-function ring (cross-thread copy of the
/// PC-TRACE ring in psp_dispatch.cpp), oldest first. Returns count.
int psp_dispatch_get_recent_funcs(uint32_t* out, int max);
/// Debug socket K: PSP thread <tid> prints its guest call stack at its next function entry.
void psp_dispatch_request_backtrace(int tid);
/// Debug socket `A`: restart the per-function log budgets of
/// PSPRECOMP_FUNC_WATCH / _FUNC_ARGS / _OVL_ARGS (log the next calls again).
void psp_dispatch_trace_rearm();
/// Debug socket `C`: write "<addr> <entries>" per guest function entered since
/// the last call (PSPRECOMP_FUNC_CENSUS=1) and reset the counts. Returns the
/// number of lines, or -1 when the census is off or the file cannot be opened.
int psp_dispatch_census_dump(const char* path);

/// Debug-only GL thread safety check.
/// In debug builds: aborts if current thread is not the main thread.
/// In release builds: no-op.
#ifndef NDEBUG
#define GL_THREAD_CHECK() do { \
    if (std::this_thread::get_id() != g_main_thread_id) { \
        std::fprintf(stderr, \
            "FATAL: GL call from non-main thread at %s:%d\n", \
            __FILE__, __LINE__); \
        std::abort(); \
    } \
} while(0)
#else
#define GL_THREAD_CHECK() ((void)0)
#endif

/// Initialize SDL2 window and OpenGL 3.3 core context.
/// Must be called from main thread. Returns 0 on success.
/// Implemented in Plan 04.
int psp_runtime_init_sdl();

/// Clean shutdown: signal threads, drain render queue, join threads.
/// Implemented in Plan 05.
void psp_runtime_shutdown();

/// Install signal handlers for SIGTERM and SIGINT that set g_should_exit.
/// Implemented in Plan 05.
void psp_install_signal_handlers();
