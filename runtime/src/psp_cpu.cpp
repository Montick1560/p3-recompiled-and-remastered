#include "psp_cpu.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

namespace {

// Installed by the scheduler (psp_scheduler_init); host threads: 0 / -1.
int (*g_prio_of_current)() = [] { return 0; };
int (*g_id_of_current)() = [] { return -1; };
int psp_current_thread_priority() { return g_prio_of_current(); }
int psp_current_thread_id() { return g_id_of_current(); }

struct Waiter {
    int prio;
    int order;  // 0 = front (returning from HLE), 1 = back
    uint64_t ticket;
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
thread_local bool t_held = false;
thread_local char t_token;  // its address identifies this host thread

// A thread inside a non-blocking HLE call keeps its place: on the PSP a
// syscall that does not block never lets a lower-priority thread run. The
// caller leaves a reservation with its priority; only strictly better threads
// may take the CPU until it blocks for real (psp_cpu_block_begin) or the
// reservation ages out (a long host-side call counts as blocking).
struct Reservation {
    const void* owner;
    int prio;
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

}  // namespace

void psp_cpu_set_thread_info(int (*priority)(), int (*id)()) {
    g_prio_of_current = priority;
    g_id_of_current = id;
}

void psp_cpu_acquire(bool front) {
    if (!enabled() || t_held) return;
    const int prio = psp_current_thread_priority();
    std::unique_lock<std::mutex> lock(g_m);
    drop_reservation_locked(&t_token);  // our own HLE call is over
    const Waiter me{prio, front ? 0 : 1, g_next_ticket++};
    g_waiters.insert(me);
    publish_best_locked();
    auto ready = [&] {
        return !g_held && g_waiters.begin()->ticket == me.ticket &&
               !reserved_against_locked(prio, &t_token);
    };
    // Reservations age out without a notification: poll while any exists.
    // A long wait means the owner never reached an HLE call or a loop
    // back-edge: report it once per wait (deadlock diagnostics).
    const auto start = std::chrono::steady_clock::now();
    bool reported = false;
    while (!ready()) {
        g_cv.wait_for(lock, g_res.empty() ? std::chrono::milliseconds(100)
                                          : std::chrono::milliseconds(1));
        if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            reported = true;
            std::fprintf(stderr, "[CPU] thread %d (prio %d) waited 5 s for the CPU held by thread %d\n",
                         psp_current_thread_id(), prio, g_holder);
        }
    }
    g_waiters.erase(me);
    publish_best_locked();
    g_held = true;
    g_holder = psp_current_thread_id();
    t_held = true;
}

void psp_cpu_release() {
    if (!enabled() || !t_held) return;
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
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_held = false;
        g_holder = -2;
        t_held = false;
        drop_reservation_locked(&t_token);
        g_res.push_back({&t_token, psp_current_thread_priority(), std::chrono::steady_clock::now()});
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
    if (dropped) g_cv.notify_all();
}

bool psp_cpu_held() {
    return t_held;
}

void psp_cpu_yield_if_contended() {
    if (!t_held) return;
    if (g_best_waiting_prio.load(std::memory_order_relaxed) > psp_current_thread_priority()) {
        return;  // nobody waiting, or only worse-priority threads
    }
    psp_cpu_release();
    psp_cpu_acquire(/*front=*/false);
}
