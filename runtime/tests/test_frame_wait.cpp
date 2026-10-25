// psp_frame_wait.h: a frame-counted wait written for 30 fps keeps its length when the script runs more often.
#include "psp_frame_wait.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    CHECK(psp_frame_wait_left(14, 2) == 29, "N = 15 at two runs per frame: 30 runs");
    CHECK(psp_frame_wait_left(0, 2) == 0, "N = 1 stays one run: a poll-every-frame loop sees one-vblank flags");
    CHECK(psp_frame_wait_left(1, 2) == 3, "N = 2 at two runs per frame: 4 runs");
    CHECK(psp_frame_wait_left(59, 2) == 119, "N = 60 at two runs per frame: 120 runs");
    CHECK(psp_frame_wait_left(14, 1) == 14, "one run per frame (30 fps): unchanged");
    CHECK(psp_frame_wait_left(-1, 2) == -1, "a negative count is left alone: never invent a wait");
    if (failures) return 1;
    std::printf("frame_wait: all tests passed\n");
    return 0;
}
