#pragma once
// Shows the main thread's frame loop a change to one value (`latched`) that another thread makes
// only from the frame after the change. On the PSP a higher-priority thread woken by the vblank
// runs before the main thread's frame, so every task of one frame reads the same value; here such
// a thread can run between two tasks of a frame (at any HLE call of the main thread), and a task
// that polled before the change and a task that polled after it disagree within one frame.
// Changes to any other value pass at once: those come from a thread the main thread itself woke
// with that HLE call, which on the PSP runs right away too. view(key, real, frame) returns the
// value to show: the real one for a new key (a new object) or a value other than `latched`, and
// the last one shown until a change to `latched` has stood since an earlier frame. Pure.
#include <cstdint>

struct FrameLatch {
    uint32_t latched = 0;
    uint32_t key = 0;
    uint32_t shown = 0;
    uint64_t since = 0;  // frame the change to `latched` was first seen
    bool pending = false;

    uint32_t view(uint32_t k, uint32_t real, uint64_t frame) {
        if (k != key || real != latched || shown == latched) {
            key = k;
            shown = real;
            pending = false;
            return shown;
        }
        if (!pending) {
            pending = true;
            since = frame;
        }
        if (frame > since) {
            shown = real;
            pending = false;
        }
        return shown;
    }
};
