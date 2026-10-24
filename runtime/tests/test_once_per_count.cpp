// psp_once_per_count.h: a per-frame guest routine that reads a counter another routine advances
// runs at most once per advance (60 fps: menu hold-repeat reads the 30 Hz pad hold counter).
#include "psp_once_per_count.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    {   // 60 fps: the counter advances on every second call
        OncePerCount o;
        CHECK(o.due(5), "first call runs");
        CHECK(!o.due(5), "the in-between frame (counter unchanged) is skipped");
        CHECK(o.due(6), "the next advance runs");
        CHECK(!o.due(6), "and its in-between frame is skipped");
    }
    {   // 30 fps: the counter advances on every call, so every call runs
        OncePerCount o;
        for (uint64_t c = 100; c < 110; c++) CHECK(o.due(c), "every call runs when the counter moves");
    }
    {   // the routine was not called for a while (menu closed): the first call after runs once
        OncePerCount o;
        CHECK(o.due(1), "runs");
        CHECK(o.due(40), "a jump of many advances runs once");
        CHECK(!o.due(40), "not twice");
    }
    {   // a counter that starts at 0 still runs on the first call
        OncePerCount o;
        CHECK(o.due(0), "first call at count 0 runs");
        CHECK(!o.due(0), "second call at count 0 is skipped");
    }
    if (failures == 0) std::printf("once_per_count: all tests passed\n");
    return failures ? 1 : 0;
}
