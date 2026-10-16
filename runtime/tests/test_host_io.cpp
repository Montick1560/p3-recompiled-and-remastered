// psp_host_io: shutdown waits for host file writes in flight (a half-written save is worse
// than a slow exit).
#include "psp_host_io.h"

#include <chrono>
#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    using ms = std::chrono::milliseconds;
    CHECK(psp_host_writes_in_flight() == 0, "starts idle");
    CHECK(psp_host_writes_wait_idle(ms(0)), "idle wait returns at once");
    {
        PspHostWriteScope a;
        PspHostWriteScope b;
        CHECK(psp_host_writes_in_flight() == 2, "nested scopes count");
        CHECK(!psp_host_writes_wait_idle(ms(30)), "times out while a write is in flight");
    }
    CHECK(psp_host_writes_in_flight() == 0, "scopes end");
    std::thread w([] {
        PspHostWriteScope s;
        std::this_thread::sleep_for(ms(200));
    });
    std::this_thread::sleep_for(ms(20));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(psp_host_writes_wait_idle(ms(2000)), "waits for the writer");
    const auto waited = std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - t0).count();
    CHECK(waited >= 120 && waited < 1500, "returned when the writer finished");
    w.join();
    if (failures == 0) std::printf("host_io: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
