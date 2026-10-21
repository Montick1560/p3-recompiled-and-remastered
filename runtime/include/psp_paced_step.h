#pragma once
// Paces a guest update that runs once per frame for each of several objects
// (a0 = the object, a1 = the frame time) to at most one run per `period`
// vblanks per object. A game written for 30 fps that moves things by a fixed
// step per call runs twice as fast when a patch set makes it render at 60;
// running each object's update on every second vblank keeps the 30 fps step
// rate. A skipped call's frame time is banked and handed to the object's next
// run, so timers that use the frame time still count real time. When the game
// slows down to `period` vblanks per frame on its own, every call runs and the
// frame time is left alone. Each object has its own pacer: a frame that
// crosses a vblank between two objects does not make the later one skip.
// Pure; the caller passes the current vblank index (psp_vblank_clock.h).
#include "psp_vblank_pacer.h"

#include <cstdint>

struct PacedStepTable {
    static constexpr int kSlots = 16;
    // Cap on banked time (ticks of the caller's unit): a skipped call banks
    // one 60 fps frame, so anything above two 30 fps frames (4800 ticks/s in
    // Patapon 3) means the object stopped being run and the bank is stale.
    static constexpr uint32_t kMaxBanked = 320;
    // An object not run for longer than this (paused game, mission over, a new
    // object at a freed one's address) starts fresh: no banked time, not skipped.
    static constexpr int64_t kStaleVblanks = 30;

    struct Slot {
        uint32_t obj = 0;
        int64_t used = -1;
        VblankPacer pacer;
        uint32_t banked = 0;
        bool skipped = false;  // the last step() for this object returned false
    };
    Slot slots[kSlots];

    // True: run the update now with *dt (this call's frame time plus the
    // banked one). False: skip it; *dt has been banked for the next run.
    bool step(uint32_t obj, int64_t vcount, int64_t period, uint32_t* dt) {
        Slot* s = nullptr;
        Slot* lru = &slots[0];
        for (Slot& c : slots) {
            if (c.used >= 0 && c.obj == obj) { s = &c; break; }
            if (c.used < lru->used) lru = &c;
        }
        if (!s) {  // new object: fresh slot
            s = lru;
            *s = Slot{};
            s->obj = obj;
        } else if (vcount - s->used > kStaleVblanks) {
            s->banked = 0;
        }
        s->used = vcount;
        s->skipped = !s->pacer.due(vcount, period);
        if (s->skipped) {
            s->banked += *dt;
            if (s->banked > kMaxBanked) s->banked = kMaxBanked;
            return false;
        }
        *dt += s->banked;
        s->banked = 0;
        return true;
    }

    // Whether the object's last update was skipped: its draw then shows the
    // state of the previous run (a draw can show it half a step ahead).
    bool skipped(uint32_t obj) const {
        for (const Slot& c : slots)
            if (c.used >= 0 && c.obj == obj) return c.skipped;
        return false;
    }
};
