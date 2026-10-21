#include "psp_cpu.h"
#include "psp_perf.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// Installed by the scheduler (psp_scheduler_init); host threads: 0 / -1.
int (*g_prio_of_current)() = [] { return 0; };
int (*g_id_of_current)() = [] { return -1; };
int psp_current_thread_priority() { return g_prio_of_current(); }
int psp_current_thread_id() { return g_id_of_current(); }

struct Waiter {
    int prio;
    int order;  // -1 = back from a non-blocking HLE call (never lost the CPU), 0 = front (returning
                // from HLE), 1 = back
    uint64_t ticket;
    int id;  // PSP thread id (diagnostics)
    bool operator<(const Waiter& o) const {
        return std::tie(prio, order, ticket) < std::tie(o.prio, o.order, o.ticket);
    }
};

std::mutex g_m;
std::condition_variable g_cv;
bool g_held = false;
int g_holder = -2;
uint64_t g_next_ticket = 0;
std::set<Waiter> g_waiters;
std::atomic<int> g_best_waiting_prio{INT_MAX};
std::atomic<bool> g_closed{false};
std::atomic<int> g_parked{0};
std::atomic<const void*> g_closer{nullptr};
thread_local bool t_held = false;
thread_local char t_token;  // its address identifies this host thread

// PSPRECOMP_CPU_SLOW=<ms> (diagnostics): log every CPU hold and every wait for the CPU longer than
// that, with the threads that held it during the wait ([CPU-SLOW] lines; t = ms since the first
// acquire). Off: one predictable branch per acquire / release.
double slow_ms() {
    static const double ms = [] {
        const char* e = std::getenv("PSPRECOMP_CPU_SLOW");
        return e && e[0] ? std::atof(e) : 0.0;
    }();
    return ms;
}
using SlowClock = std::chrono::steady_clock;
const SlowClock::time_point g_slow_epoch = SlowClock::now();
thread_local SlowClock::time_point t_acquired;
struct Grant {
    int id, prio;
    SlowClock::time_point at;
};
Grant g_grants[64];  // ring of the last CPU grants (under g_m)
uint64_t g_grant_count = 0;
double since_epoch_ms(SlowClock::time_point t) {
    return std::chrono::duration<double, std::milli>(t - g_slow_epoch).count();
}
}  // namespace

// PSPRECOMP_CPU_TRACE=<file> (diagnostics): every CPU-lock event as one line "<us> <ev> <id> <prio> [name]"
// (G grant, L release, LR release keeping a reservation, B blocking wait begins, W waits, HE / HX HLE
// call enter / exit), buffered (1 MiB) and written under the lock's own order. Off: one branch per event.
static FILE* trace_file() {
    static FILE* f = [] {
        const char* e = std::getenv("PSPRECOMP_CPU_TRACE");
        FILE* out = e && e[0] ? std::fopen(e, "w") : nullptr;
        if (out) std::setvbuf(out, nullptr, _IOFBF, 1 << 20);
        return out;
    }();
    return f;
}
static std::mutex g_trace_m;
void psp_cpu_trace(const char* ev, const char* name) {
    FILE* f = trace_file();
    if (f == nullptr) return;
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> l(g_trace_m);
    std::fprintf(f, "%lld %s %d %d%s%s\n", us, ev, psp_current_thread_id(), psp_current_thread_priority(),
                 name ? " " : "", name ? name : "");
    static unsigned n = 0;
    if ((++n & 0x3FFF) == 0) std::fflush(f);  // a killed run keeps all but the last events
}
void psp_cpu_trace_flush() {
    if (FILE* f = trace_file()) {
        std::lock_guard<std::mutex> l(g_trace_m);
        std::fflush(f);
    }
}

namespace {
void slow_note_release() {  // the holder, before giving the CPU up
    if (slow_ms() <= 0.0) return;
    const auto now = SlowClock::now();
    const double held = std::chrono::duration<double, std::milli>(now - t_acquired).count();
    if (held > slow_ms()) {
        std::fprintf(stderr, "[CPU-SLOW] t=%.1f hold id=%d prio=%d %.2f ms\n", since_epoch_ms(now),
                     psp_current_thread_id(), psp_current_thread_priority(), held);
    }
}

// A thread inside a non-blocking HLE call keeps its place: on the PSP a
// syscall that does not block never lets a lower-priority thread run. The
// caller leaves a reservation with its priority; only strictly better threads
// may take the CPU until it blocks for real (psp_cpu_block_begin) or the
// reservation ages out (a long host-side call counts as blocking).
struct Reservation {
    const void* owner;
    int prio;
    int id;  // PSP thread id (diagnostics)
    std::chrono::steady_clock::time_point since;
};
std::vector<Reservation> g_res;
constexpr auto kReservationTtl = std::chrono::milliseconds(2);

bool reserved_against_locked(int prio, const void* me) {
    const auto now = std::chrono::steady_clock::now();
    for (const Reservation& r : g_res) {
        if (r.owner != me && r.prio <= prio && now - r.since < kReservationTtl) return true;
    }
    return false;
}

// Drops `me`'s reservation: 0 = none, 1 = one past its TTL (the call counted as blocking), 2 = a live one
// (the call did not block: on the PSP the caller never lost the CPU).
int drop_reservation_age_locked(const void* me) {
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < g_res.size(); i++) {
        if (g_res[i].owner == me) {
            const bool live = now - g_res[i].since < kReservationTtl;
            g_res.erase(g_res.begin() + static_cast<std::ptrdiff_t>(i));
            return live ? 2 : 1;
        }
    }
    return 0;
}

bool drop_reservation_locked(const void* me) {
    for (size_t i = 0; i < g_res.size(); i++) {
        if (g_res[i].owner == me) {
            g_res.erase(g_res.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("PSPRECOMP_NO_CPU_LOCK");
        return !(e && e[0] == '1');
    }();
    return on;
}

void publish_best_locked() {
    g_best_waiting_prio.store(g_waiters.empty() ? INT_MAX : g_waiters.begin()->prio,
                              std::memory_order_relaxed);
}

// Never returns: the thread stays blocked until the process exits. The closer exits instead
// (parking it would hang the shutdown it is running).
[[noreturn]] void park_forever() {
    if (g_closer.load() == &t_token) {
        std::fprintf(stderr, "[CPU] the closing thread asked for the CPU after close: exiting now\n");
        std::fflush(nullptr);
        std::_Exit(0);
    }
    g_parked.fetch_add(1);
    g_cv.notify_all();
    std::mutex m;
    std::unique_lock<std::mutex> l(m);
    std::condition_variable never;
    for (;;) never.wait(l);
}

}  // namespace

void psp_cpu_set_thread_info(int (*priority)(), int (*id)()) {
    g_prio_of_current = priority;
    g_id_of_current = id;
}

// Why a waiter cannot take the CPU now (under g_m): 'H' held by g_holder, 'Q' a better waiter queued
// ahead, 'R' a reservation; the thread id responsible. ('-', -9) = nothing (about to run).
static std::pair<char, int> blocker_locked(int prio, uint64_t ticket, const void* me) {
    if (g_held) return {'H', g_holder};
    if (!g_waiters.empty() && g_waiters.begin()->ticket != ticket) {
        // The head is ahead of us: 'r' = it is itself held back by a reservation, 'Q' = it could run
        // (waking up).
        const Waiter& h = *g_waiters.begin();
        const auto t = std::chrono::steady_clock::now();
        for (const Reservation& r : g_res) {
            if (r.prio <= h.prio && t - r.since < kReservationTtl) return {'r', r.id};
        }
        return {'Q', h.id};
    }
    const auto now = std::chrono::steady_clock::now();
    for (const Reservation& r : g_res) {
        if (r.owner != me && r.prio <= prio && now - r.since < kReservationTtl) return {'R', r.id};
    }
    return {'-', -9};
}

void psp_cpu_acquire(bool front) {
    if (g_closed.load(std::memory_order_acquire)) {
        psp_cpu_release();
        park_forever();
    }
    if (!enabled() || t_held) return;
    const int prio = psp_current_thread_priority();
    std::unique_lock<std::mutex> lock(g_m);
    // Our own HLE call is over. A live reservation means it did not block: we never lost the CPU, so we
    // go on ahead of every equal-priority waiter (one may have queued meanwhile, held back by the
    // reservation: waiting for it to wake up instead cost a timer tick per hand-off).
    const int dropped = drop_reservation_age_locked(&t_token);
    const Waiter me{prio, dropped == 2 ? -1 : (front ? 0 : 1), g_next_ticket++, psp_current_thread_id()};
    g_waiters.insert(me);
    publish_best_locked();
    auto ready = [&] {
        return !g_held && g_waiters.begin()->ticket == me.ticket &&
               !reserved_against_locked(prio, &t_token);
    };
    // A waiter our reservation held back may be able to run now (we wait behind a better one).
    if (dropped != 0 && !ready()) g_cv.notify_all();
    // Reservations age out without a notification: poll while any exists.
    // A long wait means the owner never reached an HLE call or a loop
    // back-edge: report it once per wait (deadlock diagnostics).
    const auto start = std::chrono::steady_clock::now();
    bool reported = false;
    bool traced_wait = false;
    // PSPRECOMP_CPU_SLOW: time blocked per (reason, thread), sampled at every wake-up of this wait.
    std::vector<std::tuple<char, int, double>> why;
    auto last_t = start;
    std::pair<char, int> last_why = slow_ms() > 0.0 ? blocker_locked(prio, me.ticket, &t_token)
                                                     : std::pair<char, int>{'-', -9};
    auto note_why = [&] {
        if (slow_ms() <= 0.0) return;
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last_t).count();
        bool found = false;
        for (auto& w : why) {
            if (std::get<0>(w) == last_why.first && std::get<1>(w) == last_why.second) {
                std::get<2>(w) += ms;
                found = true;
            }
        }
        if (!found) why.emplace_back(last_why.first, last_why.second, ms);
        last_t = now;
        last_why = blocker_locked(prio, me.ticket, &t_token);
    };
    for (;;) {
        if (g_closed.load(std::memory_order_acquire)) {
            g_waiters.erase(me);
            publish_best_locked();
            lock.unlock();
            park_forever();
        }
        if (ready()) break;
        if (!traced_wait) {
            traced_wait = true;
            psp_cpu_trace("W", nullptr);
        }
        g_cv.wait_for(lock, g_res.empty() ? std::chrono::milliseconds(100)
                                          : std::chrono::milliseconds(1));
        note_why();
        if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            reported = true;
            std::fprintf(stderr, "[CPU] thread %d (prio %d) waited 5 s for the CPU held by thread %d\n",
                         psp_current_thread_id(), prio, g_holder);
        }
    }
    if (slow_ms() > 0.0) {
        const auto now = SlowClock::now();
        const double waited = std::chrono::duration<double, std::milli>(now - start).count();
        if (waited > slow_ms()) {
            char who[512];
            int n = 0;
            for (uint64_t i = g_grant_count > 64 ? g_grant_count - 64 : 0; i < g_grant_count && n < 480; i++) {
                const Grant& g = g_grants[i % 64];
                if (g.at >= start) n += std::snprintf(who + n, sizeof(who) - n, " %d/p%d@%.1f", g.id, g.prio, since_epoch_ms(g.at));
            }
            who[n] = '\0';
            char br[256];
            int b = 0;
            for (const auto& w : why) {
                if (std::get<2>(w) >= 0.05 && b < 230) {
                    b += std::snprintf(br + b, sizeof(br) - b, " %c%d:%.1f", std::get<0>(w), std::get<1>(w), std::get<2>(w));
                }
            }
            br[b] = '\0';
            std::fprintf(stderr, "[CPU-WHY] id=%d prio=%d %.2f ms:%s\n", psp_current_thread_id(), prio, waited, br);
            std::fprintf(stderr, "[CPU-SLOW] t=%.1f wait id=%d prio=%d %s %.2f ms; granted meanwhile:%s\n",
                         since_epoch_ms(now), psp_current_thread_id(), prio, front ? "front" : "back", waited, who);
        }
        t_acquired = now;
        g_grants[g_grant_count++ % 64] = {psp_current_thread_id(), prio, now};
    }
    if (perf_enabled()) {
        g_perf.cpu_wait_ns.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count()),
            std::memory_order_relaxed);
    }
    g_waiters.erase(me);
    publish_best_locked();
    psp_cpu_trace("G", nullptr);
    g_held = true;
    g_holder = psp_current_thread_id();
    t_held = true;
}

void psp_cpu_release() {
    if (!enabled() || !t_held) return;
    slow_note_release();
    psp_cpu_trace("L", nullptr);
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_held = false;
        g_holder = -2;
        t_held = false;
    }
    g_cv.notify_all();
}

void psp_cpu_release_reserved() {
    if (!enabled() || !t_held) return;
    slow_note_release();
    psp_cpu_trace("LR", nullptr);
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_held = false;
        g_holder = -2;
        t_held = false;
        drop_reservation_locked(&t_token);
        g_res.push_back({&t_token, psp_current_thread_priority(), psp_current_thread_id(), std::chrono::steady_clock::now()});
    }
    g_cv.notify_all();
}

void psp_cpu_block_begin() {
    if (!enabled()) return;
    bool dropped;
    {
        std::lock_guard<std::mutex> lock(g_m);
        dropped = drop_reservation_locked(&t_token);
    }
    if (dropped) {
        psp_cpu_trace("B", nullptr);
        g_cv.notify_all();
    }
}

bool psp_cpu_held() {
    return t_held;
}

void psp_cpu_yield_if_contended() {
    if (!t_held) return;
    if (g_closed.load(std::memory_order_relaxed)) {
        psp_cpu_release();
        park_forever();
    }
    if (g_best_waiting_prio.load(std::memory_order_relaxed) > psp_current_thread_priority()) {
        return;  // nobody waiting, or only worse-priority threads
    }
    psp_cpu_release();
    psp_cpu_acquire(/*front=*/false);
}

int psp_cpu_describe(char* buf, int size) {
    std::lock_guard<std::mutex> lock(g_m);
    const auto now = std::chrono::steady_clock::now();
    int n = std::snprintf(buf, size, "held=%d holder=%d waiters=[", g_held ? 1 : 0, g_holder);
    for (const Waiter& w : g_waiters) {
        if (n >= size) break;
        n += std::snprintf(buf + n, size - n, "(id %d prio %d %s)", w.id, w.prio, w.order > 0 ? "back" : (w.order < 0 ? "own" : "front"));
    }
    if (n < size) n += std::snprintf(buf + n, size - n, "] reservations=[");
    for (const Reservation& r : g_res) {
        if (n >= size) break;
        const long long us = std::chrono::duration_cast<std::chrono::microseconds>(now - r.since).count();
        n += std::snprintf(buf + n, size - n, "(id %d prio %d age %lldus)", r.id, r.prio, us);
    }
    if (n < size) n += std::snprintf(buf + n, size - n, "]");
    return n;
}

void psp_cpu_close() {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_closer.store(&t_token);
        g_closed.store(true, std::memory_order_release);
    }
    g_cv.notify_all();
}

bool psp_cpu_wait_released(std::chrono::milliseconds limit) {
    std::unique_lock<std::mutex> lock(g_m);
    return g_cv.wait_for(lock, limit, [] { return !g_held; });
}

int psp_cpu_parked() {
    return g_parked.load();
}
