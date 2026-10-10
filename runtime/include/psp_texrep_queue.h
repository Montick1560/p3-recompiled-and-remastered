#pragma once
// Background decoding of texture-replacement images. The render thread calls
// request() on a texture-cache miss: Ready returns the image, Pending means a
// worker thread is decoding it (draw the original meanwhile), Failed is final.
// Images are cached by path with an LRU byte budget; eviction happens only in
// request(), on the render thread, so a returned image stays valid until the
// next request().
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

    /// Incremented each time an image finishes decoding (ready or failed).
    uint64_t ready_epoch() const;

    /// Block until the queue is empty and the worker is idle (tests).
    void wait_idle();

private:
    struct Entry {
        State state = State::Pending;
        std::unique_ptr<TexrepImage> image;
        uint64_t last_use = 0;
    };
    void worker();
    void evict_locked(const std::string& keep);

    DecodeFn decode_;
    size_t budget_;
    mutable std::mutex mu_;
    std::condition_variable cv_;       // work available / stop
    std::condition_variable idle_cv_;  // queue drained
    std::deque<std::string> queue_;
    std::unordered_map<std::string, Entry> entries_;
    size_t bytes_ = 0;
    uint64_t clock_ = 0;
    uint64_t epoch_ = 0;
    bool busy_ = false;
    bool stop_ = false;
    std::thread thread_;
};
