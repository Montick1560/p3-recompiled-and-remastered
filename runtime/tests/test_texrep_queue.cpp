// TexrepImageQueue: replacement PNGs decode on a worker thread; the render
// thread polls request() and draws the original texture while Pending.
#include "psp_texrep_queue.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static std::atomic<int> g_calls{0};

// Fake decoder: "bad*" fails; otherwise a w x 1 image whose width is the path
// length and whose bytes are the first character.
static bool fake_decode(const std::string& path, int* w, int* h, std::vector<uint8_t>* rgba) {
    g_calls++;
    if (path.rfind("bad", 0) == 0) return false;
    *w = static_cast<int>(path.size());
    *h = 1;
    rgba->assign(static_cast<size_t>(*w) * 4, static_cast<uint8_t>(path[0]));
    return true;
}

static void pending_then_ready() {
    g_calls = 0;
    TexrepImageQueue q(fake_decode, 1u << 20);
    const TexrepImage* img = nullptr;
    const uint64_t e0 = q.ready_epoch();
    CHECK(q.request("abcd", &img) == TexrepImageQueue::State::Pending, "first request is pending");
    CHECK(q.request("abcd", &img) == TexrepImageQueue::State::Pending || img != nullptr,
          "second request while decoding does not enqueue again");
    q.wait_idle();
    CHECK(q.ready_epoch() > e0, "epoch bumps when an image is ready");
    CHECK(q.request("abcd", &img) == TexrepImageQueue::State::Ready, "ready after decode");
    CHECK(img && img->w == 4 && img->h == 1 && img->rgba.size() == 16 && img->rgba[0] == 'a',
          "decoded image returned");
    CHECK(g_calls == 1, "decoded exactly once");
}

static void failure_is_sticky() {
    g_calls = 0;
    TexrepImageQueue q(fake_decode, 1u << 20);
    const TexrepImage* img = nullptr;
    q.request("bad.png", &img);
    q.wait_idle();
    CHECK(q.request("bad.png", &img) == TexrepImageQueue::State::Failed, "failed decode reported");
    CHECK(img == nullptr, "no image for a failure");
    q.request("bad.png", &img);
    q.wait_idle();
    CHECK(g_calls == 1, "a failed file is not retried");
}

static void lru_budget() {
    // Each image is path-length * 4 bytes: "xxxxxxxx" = 32 bytes; budget 40
    // keeps one, so asking for B evicts A.
    TexrepImageQueue q(fake_decode, 40);
    const TexrepImage* img = nullptr;
    q.request("aaaaaaaa", &img);
    q.wait_idle();
    CHECK(q.request("aaaaaaaa", &img) == TexrepImageQueue::State::Ready, "A ready");
    q.request("bbbbbbbb", &img);
    q.wait_idle();
    CHECK(q.request("bbbbbbbb", &img) == TexrepImageQueue::State::Ready, "B ready");
    CHECK(img && img->rgba[0] == 'b', "B returned (and kept)");
    CHECK(q.request("aaaaaaaa", &img) == TexrepImageQueue::State::Pending, "A was evicted, decodes again");
    q.wait_idle();
}

static void destructor_with_work_pending() {
    TexrepImageQueue q(fake_decode, 1u << 20);
    const TexrepImage* img = nullptr;
    for (int i = 0; i < 50; i++) q.request("p" + std::to_string(i), &img);
    // Leaving scope must join the worker without hanging.
}

int main() {
    pending_then_ready();
    failure_is_sticky();
    lru_budget();
    destructor_with_work_pending();
    if (failures == 0) std::printf("test_texrep_queue: all passed\n");
    return failures == 0 ? 0 : 1;
}
