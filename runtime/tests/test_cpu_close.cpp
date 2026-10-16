// psp_cpu: closing the guest CPU at shutdown parks every thread that would run guest code again.
#include "psp_cpu.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static bool wait_until(const std::atomic<int>& v, int want, int ms) {
    for (int i = 0; i < ms; i++) {
        if (v.load() >= want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return v.load() >= want;
}
static bool wait_parked(int want, int ms) {
    for (int i = 0; i < ms; i++) {
        if (psp_cpu_parked() >= want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return psp_cpu_parked() >= want;
}

static int run_closer() {
    // The closing thread must never park: asking for the CPU after close exits the process.
    psp_cpu_close();
    psp_cpu_acquire();
    std::printf("FAIL: closer came back from acquire\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--closer") == 0) return run_closer();
    const bool nolock = argc > 1 && std::strcmp(argv[1], "--nolock") == 0;

    if (!nolock) {  // before close: normal hand-over
        std::atomic<int> got{0};
        std::thread a([&] { psp_cpu_acquire(); got = 1; psp_cpu_release(); });
        a.join();
        CHECK(got.load() == 1, "acquire works before close");
        CHECK(psp_cpu_wait_released(std::chrono::milliseconds(100)), "free after release");
    }

    // With the lock on: a holder running guest code and a waiter coming back from an HLE call.
    // With PSPRECOMP_NO_CPU_LOCK=1 nobody holds or waits (acquire returns at once before close),
    // so only the late thread below is checked.
    std::atomic<int> holder_has{0}, holder_go{0}, ran_after{0}, waiter_started{0};
    std::thread holder, waiter;
    if (!nolock) {
        holder = std::thread([&] {
            psp_cpu_acquire();
            holder_has = 1;
            while (!holder_go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            psp_cpu_yield_if_contended();  // loop back-edge after close: parks
            ran_after = 1;
        });
        CHECK(wait_until(holder_has, 1, 1000), "holder took the CPU");
        waiter = std::thread([&] {
            waiter_started = 1;
            psp_cpu_acquire(/*front=*/true);  // blocked behind the holder
            ran_after = 1;
        });
        CHECK(wait_until(waiter_started, 1, 1000), "waiter started");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    psp_cpu_close();
    psp_cpu_close();  // idempotent
    holder_go = 1;
    const int expect = nolock ? 0 : 2;
    CHECK(wait_parked(expect, 2000), "holder and waiter parked");
    CHECK(psp_cpu_wait_released(std::chrono::milliseconds(500)), "nobody holds the CPU after parking");
    CHECK(ran_after.load() == 0, "no thread ran guest code after close");

    std::thread late([&] { psp_cpu_acquire(); ran_after = 1; });  // asks after close
    CHECK(wait_parked(expect + 1, 2000), "late thread parked");
    CHECK(ran_after.load() == 0, "late thread did not run");

    if (holder.joinable()) holder.detach();
    if (waiter.joinable()) waiter.detach();
    late.detach();
    if (failures == 0) std::printf("cpu_close%s: all tests passed\n", nolock ? " (no lock)" : "");
    std::fflush(nullptr);
    std::_Exit(failures == 0 ? 0 : 1);  // parked threads never return; do not run destructors
}
