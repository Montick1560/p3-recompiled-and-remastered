#pragma once
// Background xBRZ upscaling for the texture cache (PSPRECOMP_TEX_UPSCALE): TexUpscaleQueue scales on the
// TexrepImageQueue worker (same Pending / Ready / Failed protocol, epoch and byte budget as the HD pack), and
// TexChangeTracker tells which texture addresses change too often to be worth scaling (palette animations).
#include "psp_texrep_queue.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class TexChangeTracker {
public:
    // Records that display list `list` drew `hash` at `addr`. True while the address got kChanges new hashes (ones
    // not among its last kRecent) within kWindow lists: an animation, not one texture drawn with a few palettes.
    bool changes_often(uint32_t addr, uint32_t hash, uint32_t list);
    static constexpr uint32_t kWindow = 60;
    static constexpr size_t kMaxAddrs = 4096;
    static constexpr size_t kRecent = 8;
    static constexpr size_t kChanges = 4;

private:
    struct Rec {
        std::array<uint32_t, kRecent> recent{};  // last distinct hashes, ring
        size_t count = 0, next = 0;
        std::array<uint32_t, kChanges> changes{};  // display lists of the last new hashes, ring
        size_t n_changes = 0, next_change = 0;
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
    // valid until the next call on this thread. rgba == nullptr only polls (a key queued without a source fails).
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
