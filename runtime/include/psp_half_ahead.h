#pragma once
// Shows a guest state that a 30 Hz step moves (psp_paced_step.h at 60 fps)
// half a step ahead on the frames in between, then puts it back bit-exact:
// the motion is 60 Hz at the 30 fps speed. For a few float words of one
// object (a transform: position, rotation, scale), not a particle list
// (psp_particle_nudge.h, which knows each particle's velocity).
//
// record() after each run of the step keeps the words' values after the
// last two runs (prev, cur). apply() writes cur + (cur - prev) / 2 into guest
// memory for the frame in between; restore() puts cur back. apply() leaves a
// word alone when the step is unknown (one record only, or a new object at
// `base`), when |cur - prev| > max_jump (a teleport, a loop restart or a new
// state is never slid across the screen), when either value is not finite,
// and when the word no longer equals cur (someone else wrote it since the
// record). restore() only puts back a word that still holds what apply()
// wrote. Pure; at most kMaxWords words per object.
#include <cmath>
#include <cstdint>
#include <cstring>

struct HalfAhead {
    static constexpr int kMaxWords = 16;
    uint32_t base = 0;
    int n = 0;
    int records = 0;  // runs recorded for this base (a step needs two)
    uint32_t offs[kMaxWords] = {};
    uint32_t prev[kMaxWords] = {}, cur[kMaxWords] = {};  // raw float bits
    uint32_t shown[kMaxWords] = {};                      // what apply() wrote
    bool moved[kMaxWords] = {};
};

inline void psp_half_ahead_record(HalfAhead* h, const uint8_t* rdram, uint32_t base, const uint32_t* offs, int n) {
    if (n > HalfAhead::kMaxWords) n = HalfAhead::kMaxWords;
    bool same = h->base == base && h->n == n && h->records > 0;
    for (int i = 0; same && i < n; i++) same = h->offs[i] == offs[i];
    if (!same) {
        h->base = base;
        h->n = n;
        h->records = 0;
        std::memcpy(h->offs, offs, sizeof(uint32_t) * n);
    }
    for (int i = 0; i < n; i++) {
        h->prev[i] = h->cur[i];
        std::memcpy(&h->cur[i], rdram + ((base + offs[i]) & 0x07FFFFFFu), 4);
        h->moved[i] = false;
    }
    h->records++;
}

inline int psp_half_ahead_apply(HalfAhead* h, uint8_t* rdram, float max_jump) {
    int moved = 0;
    for (int i = 0; i < h->n; i++) {
        h->moved[i] = false;
        if (h->records < 2) continue;
        uint8_t* p = rdram + ((h->base + h->offs[i]) & 0x07FFFFFFu);
        uint32_t now;
        std::memcpy(&now, p, 4);
        if (now != h->cur[i] || h->prev[i] == h->cur[i]) continue;
        float a, b;
        std::memcpy(&a, &h->prev[i], 4);
        std::memcpy(&b, &h->cur[i], 4);
        if (!std::isfinite(a) || !std::isfinite(b) || std::fabs(b - a) > max_jump) continue;
        const float ahead = b + (b - a) * 0.5f;
        std::memcpy(&h->shown[i], &ahead, 4);
        std::memcpy(p, &ahead, 4);
        h->moved[i] = true;
        moved++;
    }
    return moved;
}

inline void psp_half_ahead_restore(const HalfAhead& h, uint8_t* rdram) {
    for (int i = 0; i < h.n; i++) {
        if (!h.moved[i]) continue;
        uint8_t* p = rdram + ((h.base + h.offs[i]) & 0x07FFFFFFu);
        uint32_t now;
        std::memcpy(&now, p, 4);
        if (now == h.shown[i]) std::memcpy(p, &h.cur[i], 4);
    }
}
