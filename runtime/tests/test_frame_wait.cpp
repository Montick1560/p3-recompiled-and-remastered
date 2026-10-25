// psp_frame_wait.h: a frame-counted wait written for 30 fps keeps its length when the script runs more often, and
// ends on a frame whose pad words hold the presses.
#include "psp_frame_wait.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    CHECK(psp_frame_wait_left(14, 2, true) == 29, "N = 15 started on a pad frame: 30 runs, ends on a pad frame");
    CHECK(psp_frame_wait_left(14, 2, false) == 28, "N = 15 started between pad frames: 29 runs, ends on a pad frame");
    CHECK(psp_frame_wait_left(0, 2, true) == 1,
          "N = 1 on a pad frame: 2 runs (a countdown made of wait-1 loops keeps its 30 fps pace)");
    CHECK(psp_frame_wait_left(0, 2, false) == 0,
          "N = 1 between pad frames: 1 run, so a 'check; wait 1' poll moves onto the pad frames");
    CHECK(psp_frame_wait_left(1, 2, true) == 3, "N = 2 at two runs per frame: 4 runs");
    CHECK(psp_frame_wait_left(59, 2, true) == 119, "N = 60 at two runs per frame: 120 runs");
    CHECK(psp_frame_wait_left(14, 1, false) == 14, "one run per frame (30 fps): unchanged");
    CHECK(psp_frame_wait_left(-1, 2, false) == -1, "a negative count is left alone: never invent a wait");
    if (failures) return 1;
    std::printf("frame_wait: all tests passed\n");
    return 0;
}
