// psp_freeze_watch.h: a freeze is no guest frame flip for limit_s seconds, counted from the
// first flip; closing the game while frozen turns quit/back-to-menu into exit code 13.
#include "psp_freeze_watch.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    {   // not_armed_before_first_flip: slow boots never count
        FreezeWatch w;
        CHECK(w.sample(0, 0) == 0, "no flip yet at t=0");
        CHECK(w.sample(0, 100) == 0, "no flip yet at t=100");
        CHECK(!w.frozen(), "not frozen before the first flip");
    }
    {   // freeze_after_limit, then recovery_reports_length
        FreezeWatch w;
        CHECK(w.sample(1, 0) == 0, "first flip arms");
        CHECK(w.sample(1, 14.9) == 0, "14.9 s is under the limit");
        CHECK(w.sample(1, 15.0) == 15, "15 s starts a freeze");
        CHECK(w.frozen(), "frozen at the limit");
        CHECK(w.sample(1, 20) == 0, "the freeze is reported once");
        CHECK(w.sample(2, 32.4) == -32, "recovery reports the freeze length");
        CHECK(!w.frozen(), "not frozen after recovery");
    }
    {   // flips_keep_it_alive
        FreezeWatch w;
        bool quiet = true;
        for (int t = 0; t <= 100; t += 10) quiet &= w.sample(uint64_t(t / 10 + 1), t) == 0;
        CHECK(quiet, "flips every 10 s never report");
        CHECK(!w.frozen(), "and never freeze");
    }
    {   // stopped_watch_never_freezes
        FreezeWatch w;
        w.sample(1, 0);
        w.stop();
        CHECK(w.sample(1, 60) == 0, "a stopped watch reports nothing");
        CHECK(!w.frozen(), "a stopped watch does not freeze");
    }
    {   // frozen at stop time stays frozen
        FreezeWatch w;
        w.sample(1, 0);
        w.sample(1, 15);
        w.stop();
        CHECK(w.frozen(), "frozen() keeps its value at stop time");
        CHECK(w.sample(2, 20) == 0 && w.frozen(), "even when flips resume after stop");
    }
    {   // exit_code_only_changes_when_frozen
        CHECK(freeze_exit_code(false, 0) == 0, "quit stays 0");
        CHECK(freeze_exit_code(false, 10) == 10, "menu stays 10");
        CHECK(freeze_exit_code(true, 0) == 13, "frozen quit is 13");
        CHECK(freeze_exit_code(true, 10) == 13, "frozen menu is 13");
        CHECK(freeze_exit_code(true, 12) == 12, "beta ended stays 12");
        CHECK(freeze_exit_code(true, 11) == 11, "mismatch stays 11");
    }
    if (failures == 0) std::printf("freeze_watch: all tests passed\n");
    return failures ? 1 : 0;
}
