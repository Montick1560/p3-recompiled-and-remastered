// psp_particle_nudge.h: draw a 30 Hz guest particle list half a step ahead on the frames in between.
#include "psp_particle_nudge.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

// A fake guest memory: addresses 0x08000000 + offset (the runtime masks with 0x07FFFFFF).
static std::vector<uint8_t> mem(0x10000);
static void w32(uint32_t a, uint32_t v) { std::memcpy(&mem[a & 0x07FFFFFFu], &v, 4); }
static void wf(uint32_t a, float v) { std::memcpy(&mem[a & 0x07FFFFFFu], &v, 4); }
static float rf(uint32_t a) { float v; std::memcpy(&v, &mem[a & 0x07FFFFFFu], 4); return v; }

// Sentinel at `head`, nodes at node[i] (+4 next, +8 data), data at data[i]: pos +24/+28, vel +40/+44.
static void build(uint32_t head, const uint32_t* node, const uint32_t* data, int n) {
    for (int i = 0; i < n; i++) {
        w32(node[i] + 4, i + 1 < n ? node[i + 1] : head);
        w32(node[i] + 8, data[i]);
    }
    w32(head + 4, n ? node[0] : head);
}

int main() {
    const uint32_t head = 0x08000100;
    const uint32_t node[3] = {0x08000200, 0x08000300, 0x08000400};
    const uint32_t data[3] = {0x08001000, 0x08001100, 0x08001200};
    build(head, node, data, 3);
    wf(data[0] + 24, 100.0f); wf(data[0] + 28, 50.0f); wf(data[0] + 40, 6.0f); wf(data[0] + 44, -8.0f);
    wf(data[1] + 24, -3.5f);  wf(data[1] + 28, 7.25f); wf(data[1] + 40, 26.2f); wf(data[1] + 44, -18.6f);
    wf(data[2] + 24, 1.0f);   wf(data[2] + 28, 2.0f);  wf(data[2] + 40, NAN);  wf(data[2] + 44, 1.0f);
    const std::vector<uint8_t> before = mem;

    ParticleNudge undo;
    const int n = psp_particle_nudge(mem.data(), head, 24, 40, 0.5f, 4096, &undo);
    CHECK(n == 2, "moves the two particles with a finite velocity");
    CHECK(rf(data[0] + 24) == 103.0f && rf(data[0] + 28) == 46.0f, "pos += vel / 2");
    CHECK(std::fabs(rf(data[1] + 24) - 9.6f) < 1e-4f && std::fabs(rf(data[1] + 28) + 2.05f) < 1e-4f,
          "second particle too");
    CHECK(rf(data[2] + 24) == 1.0f && rf(data[2] + 28) == 2.0f, "a non-finite velocity is left alone");
    CHECK(rf(data[0] + 40) == 6.0f && rf(data[0] + 44) == -8.0f, "velocity untouched");

    psp_particle_restore(mem.data(), undo);
    CHECK(mem == before, "restore puts every word back bit-exact");

    {   // empty list
        const uint32_t h2 = 0x08002000;
        build(h2, nullptr, nullptr, 0);
        ParticleNudge u2;
        CHECK(psp_particle_nudge(mem.data(), h2, 24, 40, 0.5f, 4096, &u2) == 0, "empty list: nothing");
    }
    {   // a list that never returns to its head (corrupt / freed): stops at max_nodes
        const uint32_t h3 = 0x08003000, loop = 0x08003100, d = 0x08003200;
        w32(h3 + 4, loop);
        w32(loop + 4, loop);  // points at itself
        w32(loop + 8, d);
        wf(d + 24, 0.0f); wf(d + 28, 0.0f); wf(d + 40, 2.0f); wf(d + 44, 2.0f);
        ParticleNudge u3;
        const int m = psp_particle_nudge(mem.data(), h3, 24, 40, 0.5f, 8, &u3);
        CHECK(m == 0 && rf(d + 24) == 0.0f, "a list longer than max_nodes is left alone");
    }
    {   // a null link ends the walk without touching anything
        const uint32_t h4 = 0x08004000;
        w32(h4 + 4, 0);
        ParticleNudge u4;
        CHECK(psp_particle_nudge(mem.data(), h4, 24, 40, 0.5f, 8, &u4) == 0, "null link: nothing");
    }
    if (failures == 0) std::printf("particle_nudge: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
