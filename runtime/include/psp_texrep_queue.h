#pragma once
// Background decoding of texture-replacement images. The render thread calls
// request() on a texture-cache miss: Ready returns the image, Pending means a
// worker thread is decoding it (draw the original meanwhile), Failed is final.
// Images are cached by path under a byte budget (TexrepLru); eviction happens
// only in request()/release() on the render thread -- the worker only inserts,
// so a returned image stays valid until the next call on this thread.
#include "psp_texrep_lru.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct TexrepImage {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // RGBA8 rows, top row first
};

class TexrepImageQueue {
public:
    enum class State { Pending, Ready, Failed };
    using DecodeFn = std::function<bool(const std::string& path, int* w, int* h,
                                        std::vector<uint8_t>* rgba)>;

    TexrepImageQueue(DecodeFn decode, size_t budget_bytes);
    ~TexrepImageQueue();
    TexrepImageQueue(const TexrepImageQueue&) = delete;
    TexrepImageQueue& operator=(const TexrepImageQueue&) = delete;

    /// Render thread. Enqueues `path` the first time; *out is set when Ready.
    State request(const std::string& path, const TexrepImage** out);

    /// Render thread, after the image was copied into a GL texture: drop the
    /// CPU buffer now (a later request of the same file decodes it again).
    void release(const std::string& path);

    /// Incremented each time an image finishes decoding (ready or failed).
    uint64_t ready_epoch() const;

    /// Block until the queue is empty and the worker is idle (tests).
    void wait_idle();

private:
    struct Entry {
        State state = State::Pending;
        std::unique_ptr<TexrepImage> image;
    };
    void worker();
    void evict_locked(const std::string& keep);

    DecodeFn decode_;
    mutable std::mutex mu_;
    std::condition_variable cv_;       // work available / stop
    std::condition_variable idle_cv_;  // queue drained
    std::deque<std::string> queue_;
    std::unordered_map<std::string, Entry> entries_;
    TexrepLru<std::string> lru_;
    uint64_t epoch_ = 0;
    bool busy_ = false;
    bool stop_ = false;
    std::thread thread_;
};
