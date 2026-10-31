// Freeze watchdog host thread (psp_freeze_watch.h). Polls the guest flip count
// once a second and logs [FREEZE] lines; the launcher reads the exit code.
#include "psp_freeze_watch.h"
#include "psp_debug_socket.h"
#include "psp_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

static std::atomic<uint64_t> g_flips{0};
static std::mutex g_mutex;  // guards g_watch (its stop flag included)
static FreezeWatch g_watch;

void psp_freeze_note_flip() { g_flips.fetch_add(1, std::memory_order_relaxed); }

static double steady_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void log_threads() {
    static PspDebugThreadInfo threads[64];  // only the poll thread uses it
    const int n = psp_scheduler_snapshot(threads, 64);
    for (int i = 0; i < n; i++) {
        const PspDebugThreadInfo& t = threads[i];
        std::fprintf(stderr, "[FREEZE]   thread %d %s %s %s last_func=0x%08X\n", t.id, t.name,
                     t.status, t.wait_reason[0] ? t.wait_reason : "-", t.last_func);
    }
    std::fflush(stderr);
}

void psp_freeze_watch_start() {
    const char* e = std::getenv("PSPRECOMP_FREEZE_SECONDS");
    const long s = e ? std::strtol(e, nullptr, 10) : 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_watch.limit_s = s >= 2 && s <= 600 ? double(s) : 15.0;
    }
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int n;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_watch.stopped) return;
                n = g_watch.sample(g_flips.load(std::memory_order_relaxed), steady_seconds());
            }
            if (n > 0) {
                std::fprintf(stderr, "[FREEZE] no new frame for %d s\n", n);
                std::fflush(stderr);  // before the snapshot: a deadlocked scheduler mutex blocks it
                log_threads();
            } else if (n < 0) {
                std::fprintf(stderr, "[FREEZE] frames again after %d s\n", -n);
                std::fflush(stderr);
            }
        }
    }).detach();
}

bool psp_freeze_watch_stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_watch.stop();
    return g_watch.frozen();
}
