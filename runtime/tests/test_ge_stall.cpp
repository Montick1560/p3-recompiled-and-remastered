// psp_ge_stall.h: when a display list must stop at its stall address (PPSSPP GPUCommon parity).
#include "psp_ge_stall.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    CHECK(ge_at_stall(0x49002070, 0x49002070), "same address stalls");
    CHECK(ge_at_stall(0x09002070, 0x49002070), "pc after a JUMP (28 bits) still meets an uncached stall");
    CHECK(ge_at_stall(0x49002070, 0x09002070), "and the other way round");
    CHECK(!ge_at_stall(0x0900206C, 0x49002070), "one command before the stall keeps running");
    CHECK(!ge_at_stall(0x09A6E604, 0x49002070), "a sub-list above the stall address is not a stall");
    CHECK(!ge_at_stall(0x09002070, 0), "stall 0 means no stall");
    CHECK(ge_stall_still_current(0x49002070, 0x49002070), "no update while drawing: stays stalled");
    CHECK(!ge_stall_still_current(0x49002070, 0x49002400), "update while drawing: must run again");

    if (failures == 0) std::printf("ge_stall: all passed\n");
    return failures == 0 ? 0 : 1;
}
