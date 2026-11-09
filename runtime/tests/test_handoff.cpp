// psp_handoff.h: the game tells the launcher it is leaving and keeps its window until the launcher is ready, within a limit.
#include "psp_handoff.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

using namespace std::chrono;

static long long finish_ms(milliseconds limit, bool* active) {
    const auto t0 = steady_clock::now();
    *active = psp_handoff_finish(limit);
    return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

static void no_launcher_means_today_path() {
    bool active = true;
    CHECK(!psp_handoff_begin());
    CHECK(finish_ms(milliseconds(500), &active) < 100);
    CHECK(!active);
}

#ifdef _WIN32
#include <windows.h>

static void events_missing_means_today_path() {
    _putenv_s("PSPRECOMP_HANDOFF", "p3-test-handoff-nobody");
    bool active = true;
    CHECK(!psp_handoff_begin());
    CHECK(finish_ms(milliseconds(500), &active) < 100);
    CHECK(!active);
}

static void ready_ends_the_wait_and_no_ready_hits_the_limit() {
    _putenv_s("PSPRECOMP_HANDOFF", "p3-test-handoff");
    HANDLE leaving = CreateEventA(nullptr, TRUE, FALSE, "Local\\p3-test-handoff-leaving");
    HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, "Local\\p3-test-handoff-ready");
    CHECK(leaving && ready);

    bool active = false;
    CHECK(WaitForSingleObject(leaving, 0) == WAIT_TIMEOUT);
    CHECK(psp_handoff_begin());
    CHECK(WaitForSingleObject(leaving, 0) == WAIT_OBJECT_0);
    const long long waited = finish_ms(milliseconds(150), &active);
    CHECK(active);
    CHECK(waited >= 130 && waited < 1000);

    CHECK(psp_handoff_begin());
    SetEvent(ready);
    CHECK(finish_ms(milliseconds(5000), &active) < 1000);
    CHECK(active);

    CHECK(finish_ms(milliseconds(500), &active) < 100);  // finished: nothing is left to wait for
    CHECK(!active);
    CloseHandle(leaving);
    CloseHandle(ready);
}
#endif

int main() {
    no_launcher_means_today_path();
#ifdef _WIN32
    events_missing_means_today_path();
    ready_ends_the_wait_and_no_ready_hits_the_limit();
#endif
    if (g_failed) return 1;
    std::printf("handoff tests passed\n");
    return 0;
}
