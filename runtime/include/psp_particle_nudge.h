#pragma once
// Moves every particle of a guest particle list by `k` times its per-step
// velocity (pos += k * vel) and remembers the old positions, so a particle
// system whose update is paced to 30 Hz at 60 fps (psp_paced_step.h) can be
// drawn half a step ahead (k = 0.5) on the frames in between and then put
// back bit-exact before its next update. The motion is then 60 Hz at the
// 30 fps speed.
//
// List layout (Patapon 3 weather): a sentinel node at `head`; node +4 = next
// node, node +8 = the particle's data; data + pos_off = x, y floats, data +
// vel_off = vx, vy floats (the step adds vx, vy to x, y). A particle with a
// non-finite position or velocity is left alone. A list that does not return
// to its head within `max_nodes` (freed or being rebuilt) is not touched.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

struct ParticleNudge {
    std::vector<uint32_t> addr;   // guest address of each moved x (y follows at +4)
    std::vector<uint64_t> saved;  // the original x, y words
};

inline int psp_particle_nudge(uint8_t* rdram, uint32_t head, uint32_t pos_off, uint32_t vel_off,
                              float k, int max_nodes, ParticleNudge* undo) {
    auto ptr = [rdram](uint32_t a) { return rdram + (a & 0x07FFFFFFu); };
    auto rd = [&](uint32_t a) { uint32_t v; std::memcpy(&v, ptr(a), 4); return v; };
    undo->addr.clear();
    undo->saved.clear();
    std::vector<uint32_t> datas;
    uint32_t n = rd(head + 4);
    while (n != head) {
        if (n == 0 || static_cast<int>(datas.size()) >= max_nodes) return 0;
        datas.push_back(rd(n + 8));
        n = rd(n + 4);
    }
    for (uint32_t d : datas) {
        float p[2], v[2];
        std::memcpy(p, ptr(d + pos_off), 8);
        std::memcpy(v, ptr(d + vel_off), 8);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(v[0]) || !std::isfinite(v[1]))
            continue;
        uint64_t old;
        std::memcpy(&old, ptr(d + pos_off), 8);
        undo->addr.push_back(d + pos_off);
        undo->saved.push_back(old);
        const float moved[2] = {p[0] + k * v[0], p[1] + k * v[1]};
        std::memcpy(ptr(d + pos_off), moved, 8);
    }
    return static_cast<int>(undo->addr.size());
}

inline void psp_particle_restore(uint8_t* rdram, const ParticleNudge& undo) {
    for (size_t i = 0; i < undo.addr.size(); i++)
        std::memcpy(rdram + (undo.addr[i] & 0x07FFFFFFu), &undo.saved[i], 8);
}
