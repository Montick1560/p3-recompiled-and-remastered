// psp_tex_upscale_queue.h: textures that change often are not upscaled; scaling runs on a worker with a byte budget.
#include "psp_tex_upscale_queue.h"

#include <atomic>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    {   // change tracker
        TexChangeTracker t;
        bool often = false;
        for (uint32_t l = 0; l < 100; l++) often |= t.changes_often(0x1000, 7, l);
        CHECK(!often, "same hash every list: not often");
        CHECK(!t.changes_often(0x2000, 1, 0), "first sight");
        CHECK(!t.changes_often(0x2000, 2, 10) && !t.changes_often(0x2000, 3, 20) && !t.changes_often(0x2000, 4, 30),
              "three new hashes: a few variants");
        CHECK(t.changes_often(0x2000, 5, 40), "fourth new hash within 60 lists");
        CHECK(t.changes_often(0x2000, 5, 50), "still often while the changes are recent");
        CHECK(!t.changes_often(0x2000, 5, 200), "quiet for long: not often any more");
        bool far = false;
        for (uint32_t i = 0; i < 6; i++) far |= t.changes_often(0x3000, 900 + i, i * 30);
        CHECK(!far, "new hashes 30 lists apart: never 4 within 60");
        // one texture drawn with a few palettes (unit colours): alternating known hashes is not a change
        bool alt = false;
        for (uint32_t l = 0; l < 200; l++) alt |= t.changes_often(0x4000, 100 + l % 3, l);
        CHECK(!alt, "three palettes in turn: not often");
        // an animation keeps producing new hashes
        bool anim = false;
        for (uint32_t l = 0; l < 40; l++) anim |= t.changes_often(0x5000, 500 + l, l);
        CHECK(anim, "new hash every list: often");
    }
    {   // queue, with a fake scaler that doubles and counts
        std::atomic<int> calls{0};
        bool fail = false;
        TexUpscaleQueue q([&](int f, const uint8_t* px, int w, int h, std::vector<uint8_t>* out) {
            calls++;
            if (fail) return false;
            out->assign(static_cast<size_t>(w * f) * (h * f) * 4, px[0]);
            return true;
        }, 2 * 8 * 8 * 4);  // room for two 8x8 results
        std::vector<uint8_t> src(4 * 4 * 4, 0x42);
        const TexUpscaleImage* img = nullptr;
        const uint64_t e0 = q.ready_epoch();
        CHECK(q.request(1, 2, src.data(), 4, 4, &img) == TexUpscaleQueue::State::Pending, "first request pending");
        q.wait_idle();
        CHECK(q.ready_epoch() > e0, "epoch moves");
        CHECK(q.request(1, 2, src.data(), 4, 4, &img) == TexUpscaleQueue::State::Ready && img && img->w == 8 &&
              img->h == 8 && img->rgba.size() == 8u * 8u * 4u && img->rgba[0] == 0x42, "ready 8x8");
        CHECK(calls == 1, "scaled once");
        q.request(1, 2, src.data(), 4, 4, &img);
        CHECK(calls == 1, "no second scale for the same key");
        fail = true;
        q.request(2, 2, src.data(), 4, 4, &img);
        q.wait_idle();
        CHECK(q.request(2, 2, src.data(), 4, 4, &img) == TexUpscaleQueue::State::Failed, "scaler failure -> Failed");
        fail = false;
        q.request(3, 2, src.data(), 4, 4, &img);
        q.request(4, 2, src.data(), 4, 4, &img);
        q.wait_idle();
        q.request(3, 2, src.data(), 4, 4, &img);
        q.request(4, 2, src.data(), 4, 4, &img);  // over budget: the least recent (1) goes
        CHECK(q.request(1, 2, src.data(), 4, 4, &img) == TexUpscaleQueue::State::Pending, "evicted key scales again");
        q.wait_idle();
        // a poll without the source (cache hit path): no copy; an unknown key just fails, never crashes
        const int before = calls;
        CHECK(q.request(9, 2, nullptr, 4, 4, &img) == TexUpscaleQueue::State::Pending, "poll of unknown key");
        q.wait_idle();
        CHECK(q.request(9, 2, nullptr, 4, 4, &img) == TexUpscaleQueue::State::Failed && calls == before,
              "no source -> Failed, scaler not called");
    }
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_tex_upscale_queue: all passed\n");
    return 0;
}
