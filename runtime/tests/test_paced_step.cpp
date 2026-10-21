// psp_paced_step.h: per-object once-per-frame updates paced to one run per N vblanks, with the
// frame time of the skipped calls handed to the next run (60 fps weather).
#include "psp_paced_step.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    {   // 60 fps: called every vblank with 80 ticks -> runs every second vblank with 160
        PacedStepTable t;
        uint32_t dt = 80;
        CHECK(t.step(0x1000, 0, 2, &dt) && dt == 80, "first call runs with its own frame time");
        dt = 80;
        CHECK(!t.step(0x1000, 1, 2, &dt), "next vblank is skipped");
        dt = 80;
        CHECK(t.step(0x1000, 2, 2, &dt) && dt == 160, "the run after a skip gets both frame times");
        dt = 80;
        CHECK(!t.step(0x1000, 3, 2, &dt), "and the one after is skipped again");
        dt = 80;
        CHECK(t.step(0x1000, 4, 2, &dt) && dt == 160, "steady 30 Hz with 160 ticks");
    }
    {   // the game slows to two vblanks per frame on its own: every call runs, frame time untouched
        PacedStepTable t;
        for (int64_t vb = 10; vb < 20; vb += 2) {
            uint32_t dt = 160;
            CHECK(t.step(0x2000, vb, 2, &dt) && dt == 160, "slow frames all run unchanged");
        }
    }
    {   // two objects updated in the same frames are paced independently, same cadence
        PacedStepTable t;
        uint32_t a = 80, b = 80;
        CHECK(t.step(0xA000, 0, 2, &a) && t.step(0xB000, 0, 2, &b), "both run on vblank 0");
        a = b = 80;
        CHECK(!t.step(0xA000, 1, 2, &a) && !t.step(0xB000, 1, 2, &b), "both skip vblank 1");
        a = b = 80;
        CHECK(t.step(0xA000, 2, 2, &a) && a == 160 && t.step(0xB000, 2, 2, &b) && b == 160,
              "both run on vblank 2 with their own banked time");
    }
    {   // a frame that crosses a vblank between two objects: the late object keeps its own rhythm
        PacedStepTable t;
        uint32_t dt = 80;
        t.step(0xA000, 0, 2, &dt);
        dt = 80;
        CHECK(t.step(0xB000, 1, 2, &dt), "late object runs on its first call");
        dt = 80;
        CHECK(!t.step(0xB000, 2, 2, &dt), "then skips");
        dt = 80;
        CHECK(t.step(0xB000, 3, 2, &dt) && dt == 160, "and runs every second vblank of its own");
    }
    {   // more objects than slots: the least recently used slot is recycled and starts fresh
        PacedStepTable t;
        for (uint32_t i = 0; i < PacedStepTable::kSlots; i++) {
            uint32_t dt = 80;
            t.step(0x100 * (i + 1), 0, 2, &dt);
            dt = 80;
            t.step(0x100 * (i + 1), 1, 2, &dt);  // every object banks 80
        }
        uint32_t dt = 80;
        CHECK(t.step(0xFFFF00, 1, 2, &dt) && dt == 80, "a new object runs at once without old banked time");
        dt = 80;
        CHECK(t.step(0x200, 2, 2, &dt) && dt == 160, "the other objects keep their slots");
    }
    {   // the update stops being called (pause, mission over: a new object can reuse the
        // address) and comes back much later: it runs at once and starts fresh
        PacedStepTable t;
        uint32_t dt = 80;
        t.step(0x3000, 0, 2, &dt);
        dt = 80;
        t.step(0x3000, 1, 2, &dt);  // banks 80, skipped
        CHECK(t.skipped(0x3000), "skipped before the gap");
        dt = 80;
        CHECK(t.step(0x3000, 1 + PacedStepTable::kStaleVblanks + 1, 2, &dt) && dt == 80,
              "after a long gap: runs at once without the old banked time");
        CHECK(!t.skipped(0x3000), "and is not marked skipped");
        dt = 80;
        t.step(0x3100, 0, 2, &dt);
        dt = 80;
        t.step(0x3100, 1, 2, &dt);  // banks 80
        dt = 80;
        CHECK(t.step(0x3100, 1 + PacedStepTable::kStaleVblanks, 2, &dt) && dt == 160,
              "a gap up to the limit keeps the banked time");
    }
    {   // the banked time never runs away (a stuck skip): it is capped
        PacedStepTable t;
        uint32_t dt = 80;
        t.step(0x4000, 0, 2, &dt);
        for (int i = 0; i < 100; i++) {
            dt = 80;
            t.step(0x4000, 1, 2, &dt);  // same vblank: always skipped
        }
        dt = 80;
        CHECK(t.step(0x4000, 2, 2, &dt) && dt <= PacedStepTable::kMaxBanked + 80, "banked time is capped");
    }
    {   // the draw asks whether the object's last update was skipped (draw it half a step ahead)
        PacedStepTable t;
        CHECK(!t.skipped(0x5000), "unknown object: not skipped");
        uint32_t dt = 80;
        t.step(0x5000, 0, 2, &dt);
        CHECK(!t.skipped(0x5000), "ran: not skipped");
        dt = 80;
        t.step(0x5000, 1, 2, &dt);
        CHECK(t.skipped(0x5000), "skipped: skipped");
        CHECK(!t.skipped(0x6000), "another object is not affected");
        dt = 80;
        t.step(0x5000, 2, 2, &dt);
        CHECK(!t.skipped(0x5000), "ran again: not skipped");
    }
    if (failures == 0) std::printf("paced_step: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
