// psp_half_ahead.h: show a 30 Hz guest state half a step ahead on the frames in between.
#include "psp_half_ahead.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

// A fake guest memory: addresses 0x08000000 + offset (the runtime masks with 0x07FFFFFF).
static std::vector<uint8_t> mem(0x10000);
static void wf(uint32_t a, float v) { std::memcpy(&mem[a & 0x07FFFFFFu], &v, 4); }
static float rf(uint32_t a) { float v; std::memcpy(&v, &mem[a & 0x07FFFFFFu], 4); return v; }
static uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, &mem[a & 0x07FFFFFFu], 4); return v; }

static const uint32_t kBase = 0x08000100;
static const uint32_t kOffs[3] = {192, 196, 200};

// Two runs of the step leave x at a then b (y, z fixed); returns the HalfAhead after both records.
static HalfAhead two_runs(float a, float b) {
    HalfAhead h;
    wf(kBase + 192, a); wf(kBase + 196, 7.0f); wf(kBase + 200, 0.0f);
    psp_half_ahead_record(&h, mem.data(), kBase, kOffs, 3);
    wf(kBase + 192, b);
    psp_half_ahead_record(&h, mem.data(), kBase, kOffs, 3);
    return h;
}

int main() {
    {
        HalfAhead h = two_runs(10.0f, 12.0f);
        const uint32_t cur_bits = r32(kBase + 192);
        CHECK(psp_half_ahead_apply(&h, mem.data(), 50.0f) == 1, "moves the one word that changed");
        CHECK(rf(kBase + 192) == 13.0f, "10 -> 12 shows 13 half a step ahead");
        CHECK(rf(kBase + 196) == 7.0f, "an unchanged word stays");
        psp_half_ahead_restore(h, mem.data());
        CHECK(r32(kBase + 192) == cur_bits, "restore puts 12 back bit-exact");
    }
    {
        HalfAhead h = two_runs(10.0f, 8.0f);
        psp_half_ahead_apply(&h, mem.data(), 50.0f);
        CHECK(rf(kBase + 192) == 7.0f, "10 -> 8 shows 7");
        psp_half_ahead_restore(h, mem.data());
        CHECK(rf(kBase + 192) == 8.0f, "restore 8");
    }
    {
        HalfAhead h = two_runs(0.0f, 300.0f);
        CHECK(psp_half_ahead_apply(&h, mem.data(), 50.0f) == 0, "a jump over max_jump is not slid");
        CHECK(rf(kBase + 192) == 300.0f, "the jump target stays");
    }
    {
        HalfAhead h = two_runs(1.0f, NAN);
        CHECK(psp_half_ahead_apply(&h, mem.data(), 50.0f) == 0, "a non-finite value is left alone");
        CHECK(std::isnan(rf(kBase + 192)), "NaN stays");
    }
    {
        HalfAhead h = two_runs(5.0f, 7.0f);
        wf(kBase + 192, 9.0f);  // someone else moved it since the record
        CHECK(psp_half_ahead_apply(&h, mem.data(), 50.0f) == 0, "a word written since the record is left alone");
        CHECK(rf(kBase + 192) == 9.0f, "the other writer's 9 stays");
        psp_half_ahead_restore(h, mem.data());
        CHECK(rf(kBase + 192) == 9.0f, "restore does not touch it");
    }
    {
        HalfAhead h = two_runs(5.0f, 7.0f);
        psp_half_ahead_apply(&h, mem.data(), 50.0f);
        wf(kBase + 192, 20.0f);  // the guest wrote it while it was shown ahead
        psp_half_ahead_restore(h, mem.data());
        CHECK(rf(kBase + 192) == 20.0f, "restore keeps a value the guest wrote after apply");
    }
    {
        HalfAhead h;
        wf(kBase + 192, 3.0f);
        psp_half_ahead_record(&h, mem.data(), kBase, kOffs, 3);
        CHECK(psp_half_ahead_apply(&h, mem.data(), 50.0f) == 0, "one record only: no step known, nothing moves");
    }
    {
        HalfAhead h = two_runs(10.0f, 12.0f);
        wf(0x08000400 + 192, 50.0f);  // another object recorded into the same HalfAhead
        psp_half_ahead_record(&h, mem.data(), 0x08000400, kOffs, 3);
        CHECK(psp_half_ahead_apply(&h, mem.data(), 50.0f) == 0, "a new base starts over (no previous value)");
    }
    {
        HalfAhead h = two_runs(10.0f, 12.0f);
        psp_half_ahead_apply(&h, mem.data(), 50.0f);
        psp_half_ahead_restore(h, mem.data());
        psp_half_ahead_restore(h, mem.data());
        CHECK(rf(kBase + 192) == 12.0f, "a second restore is harmless");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("half_ahead: all passed\n");
    return 0;
}
