// psp_cpu: the guest CPU lock under contention. One holder at a time, everyone makes progress, the best
// priority goes first, a reservation keeps a non-blocking HLE caller's place, and a hand-off between two
// threads stays fast while many worse-priority threads wait (they must not be woken on every release).
#include "psp_cpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

thread_local int t_prio = 32;
thread_local int t_id = -1;
static int prio_of() { return t_prio; }
static int id_of() { return t_id; }

using Clock = std::chrono::steady_clock;

// Many threads of mixed priorities take the CPU through every path (plain, reserved HLE return, a blocking
// HLE, back-edge yields): never two holders, and all of them finish.
static void exclusion_and_progress() {
    std::atomic<int> inside{0}, overlaps{0}, done{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; i++) {
        ts.emplace_back([&, i] {
            t_prio = 16 + (i % 3) * 8;
            t_id = 100 + i;
            for (int n = 0; n < 1500; n++) {
                psp_cpu_acquire();
                if (inside.fetch_add(1) != 0) overlaps++;
                for (volatile int k = 0; k < 50; k++) {}
                inside.fetch_sub(1);
                switch (n % 4) {
                case 0: psp_cpu_release(); break;
                case 1: psp_cpu_release_reserved(); psp_cpu_acquire(true); psp_cpu_release(); break;
                case 2: psp_cpu_release_reserved(); psp_cpu_block_begin(); psp_cpu_acquire(true); psp_cpu_release(); break;
                default: psp_cpu_yield_if_contended(); psp_cpu_release(); break;
                }
            }
            done++;
        });
    }
    const auto t0 = Clock::now();
    while (done.load() < 8 && Clock::now() - t0 < std::chrono::seconds(20)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(done.load() == 8, "every thread finishes (no lost wake-up)");
    for (auto& t : ts) t.join();
    CHECK(overlaps.load() == 0, "never two holders");
}

// A holder keeps the CPU while a worse and then a better thread queue: on release the better one runs first.
static void best_priority_first() {
    std::atomic<int> order{0}, low_at{-1}, high_at{-1}, queued{0};
    t_prio = 32;
    psp_cpu_acquire();
    std::thread low([&] { t_prio = 40; t_id = 2; queued++; psp_cpu_acquire(); low_at = order++; psp_cpu_release(); });
    while (queued.load() < 1) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::thread high([&] { t_prio = 10; t_id = 3; queued++; psp_cpu_acquire(); high_at = order++; psp_cpu_release(); });
    while (queued.load() < 2) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    psp_cpu_release();
    low.join();
    high.join();
    CHECK(high_at.load() == 0 && low_at.load() == 1, "the better priority goes first");
}

// A thread in a non-blocking HLE call (reservation) keeps its place against an equal-priority waiter, and
// the waiter gets the CPU as soon as the caller blocks.
static void reservation_keeps_place() {
    std::atomic<int> waiter_got{0}, queued{0};
    t_prio = 20;
    t_id = 1;
    psp_cpu_acquire();
    std::thread w([&] { t_prio = 20; t_id = 2; queued++; psp_cpu_acquire(); waiter_got = 1; psp_cpu_release(); });
    while (queued.load() < 1) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    psp_cpu_release_reserved();  // a non-blocking HLE call begins
    for (const auto until = Clock::now() + std::chrono::microseconds(300); Clock::now() < until;) {}
    CHECK(waiter_got.load() == 0, "an equal-priority waiter does not take a reserved place");
    const auto t0 = Clock::now();
    psp_cpu_block_begin();  // the call blocks for real: the waiter runs now
    while (!waiter_got.load() && Clock::now() - t0 < std::chrono::seconds(1)) std::this_thread::yield();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::printf("waiter ran %.3f ms after the reservation's owner blocked\n", ms);
    CHECK(waiter_got.load() == 1 && ms < 50.0, "the waiter runs once the owner blocks");
    w.join();
}

// Two equal-priority threads hand the CPU to each other (like the game's two audio threads) while eight
// worse-priority threads wait for it. Returns the mean hand-off time in microseconds.
static double handoff_us() {
    std::atomic<bool> stop{false};
    std::atomic<int> turn{0}, background_in{0};
    constexpr int kRounds = 2000;
    std::vector<std::thread> bg;
    for (int i = 0; i < 8; i++) {
        bg.emplace_back([&, i] {
            t_prio = 40;
            t_id = 50 + i;
            background_in++;
            while (!stop.load()) {
                psp_cpu_acquire();
                psp_cpu_release();
            }
        });
    }
    while (background_in.load() < 8) std::this_thread::yield();
    double total_us = 0;
    std::atomic<long long> passed_ns{0};
    auto player = [&](int me) {
        t_prio = 16;
        t_id = 10 + me;
        for (int n = 0; n < kRounds; n++) {
            psp_cpu_acquire();
            while (turn.load() != me) {  // not my turn: let the other one run
                psp_cpu_release();
                psp_cpu_acquire();
            }
            turn = 1 - me;
            psp_cpu_release();
        }
    };
    const auto t0 = Clock::now();
    std::thread a(player, 0), b(player, 1);
    a.join();
    b.join();
    total_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    stop = true;
    for (auto& t : bg) t.join();
    (void)passed_ns;
    return total_us / (2.0 * kRounds);
}

// A thread back from a short non-blocking HLE call never lost the CPU (PSP): it goes on at once, ahead of an
// equal-priority thread that queued during the call (like the game's two audio threads: the PCM thread
// returns from sceKernelSignalSema while the ATRAC thread waits), and without waiting for that thread
// to wake up.
static void reservation_owner_returns_first() {
    std::atomic<int> order{0}, a_at{-1}, b_at{-1}, b_queued{0};
    double worst_ms = 0;
    for (int round = 0; round < 20; round++) {
        order = 0;
        a_at = b_at = -1;
        b_queued = 0;
        t_prio = 16;
        t_id = 1;
        psp_cpu_acquire();
        psp_cpu_release_reserved();  // A enters a non-blocking HLE call
        std::thread b([&] {
            t_prio = 16;
            t_id = 2;
            b_queued = 1;
            psp_cpu_acquire(true);  // B is back from its own HLE call: blocked by A's reservation
            b_at = order++;
            psp_cpu_release();
        });
        while (!b_queued.load()) std::this_thread::yield();
        for (const auto until = Clock::now() + std::chrono::microseconds(200); Clock::now() < until;) {}
        const auto t0 = Clock::now();
        psp_cpu_acquire(true);  // A is back: the call did not block
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        worst_ms = std::max(worst_ms, ms);
        a_at = order++;
        psp_cpu_release();
        b.join();
        if (a_at.load() != 0 || b_at.load() != 1) {
            CHECK(false, "the reservation's owner goes on before an equal-priority waiter");
            break;
        }
    }
    std::printf("owner back from a non-blocking call: worst re-acquire %.3f ms\n", worst_ms);
    CHECK(worst_ms < 0.3, "the owner re-acquires at once (no OS wake-up of another thread)");
}

int main() {
    psp_cpu_set_thread_info(prio_of, id_of);
    exclusion_and_progress();
    best_priority_first();
    reservation_keeps_place();
    reservation_owner_returns_first();
    const double us = handoff_us();
    std::printf("hand-off between two threads with 8 worse-priority waiters: %.1f us mean\n", us);
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
