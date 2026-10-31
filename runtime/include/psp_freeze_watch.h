#pragma once
// Freeze watchdog. The guest is frozen when it has not flipped a frame for
// limit_s seconds; the watch arms at the first flip so a slow boot never
// counts. A quit or back-to-menu taken while frozen exits with code 13 so the
// launcher can tell a freeze from a normal close (docs/contracts/settings-v1.md).
// FreezeWatch and freeze_exit_code are pure; the host thread lives in
// src/psp_freeze_watch.cpp.
#include <cstdint>

struct FreezeWatch {
    double limit_s = 15.0;
    bool armed = false;
    bool is_frozen = false;
    bool stopped = false;
    uint64_t last_flips = 0;
    double last_change_s = 0;

    // One sample per poll: total guest flips so far, steady time in seconds.
    // Returns +N when a freeze starts (N = seconds without a flip), -N when flips resume
    // after a freeze of N s, 0 otherwise.
    int sample(uint64_t flips, double now_s) {
        if (stopped) return 0;
        if (!armed) {
            if (flips == 0) return 0;
            armed = true;
        } else if (flips == last_flips) {
            if (is_frozen || now_s - last_change_s < limit_s) return 0;
            is_frozen = true;
            return int(now_s - last_change_s);
        }
        const int gap = int(now_s - last_change_s);
        last_flips = flips;
        last_change_s = now_s;
        if (!is_frozen) return 0;
        is_frozen = false;
        return -gap;
    }
    bool frozen() const { return is_frozen; }
    void stop() { stopped = true; }  // later samples return 0; frozen() keeps its value
};

inline int freeze_exit_code(bool frozen, int requested) {
    return frozen && (requested == 0 || requested == 10) ? 13 : requested;
}

void psp_freeze_note_flip();    // ge_present_frame, every guest flip
void psp_freeze_watch_start();  // detached host thread, polls every 1 s
bool psp_freeze_watch_stop();   // returns frozen() at stop time
