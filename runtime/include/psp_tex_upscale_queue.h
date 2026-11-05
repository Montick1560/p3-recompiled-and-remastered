#pragma once
// Background xBRZ upscaling for the texture cache (PSPRECOMP_TEX_UPSCALE): TexUpscaleQueue scales on the
// TexrepImageQueue worker (same Pending / Ready / Failed protocol, epoch and byte budget as the HD pack), and
// TexChangeTracker tells which texture addresses change too often to be worth scaling (palette animations).
#include "psp_texrep_queue.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class TexChangeTracker {
public:
    // Records that display list `list` drew `hash` at `addr`. True while the address got a new hash at least twice
    // within the last kWindow lists.
    bool changes_often(uint32_t addr, uint32_t hash, uint32_t list);
    static constexpr uint32_t kWindow = 60;
    static constexpr size_t kMaxAddrs = 4096;

private:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    struct Rec {
        uint32_t hash;
        uint32_t older = kNone, newer = kNone;  // display lists of the last two changes
    };
    std::unordered_map<uint32_t, Rec> recs_;
};

using TexUpscaleImage = TexrepImage;

class TexUpscaleQueue {
public:
    using State = TexrepImageQueue::State;
    using ScaleFn = std::function<bool(int factor, const uint8_t* rgba, int w, int h, std::vector<uint8_t>* out)>;

    TexUpscaleQueue(ScaleFn scale, size_t budget_bytes);

    // Render thread. The first request of `key` copies the source and queues it; *out is set when Ready and stays
    // valid until the next call on this thread.
    State request(uint64_t key, int factor, const uint8_t* rgba, int w, int h, const TexUpscaleImage** out);
    // Render thread, after the upload: drop the CPU copy.
    void release(uint64_t key);
    uint64_t ready_epoch() const { return queue_.ready_epoch(); }
    void wait_idle() { queue_.wait_idle(); }

private:
    struct Source {
        int factor = 0, w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    bool scale_source(const std::string& key, int* w, int* h, std::vector<uint8_t>* rgba);

    ScaleFn scale_;
    std::mutex mu_;
    std::unordered_map<std::string, Source> sources_;  // queued, not yet taken by the worker
    TexrepImageQueue queue_;                            // last: its worker uses the members above
};
