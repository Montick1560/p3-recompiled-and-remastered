#include "psp_tex_upscale_queue.h"

bool TexChangeTracker::changes_often(uint32_t addr, uint32_t hash, uint32_t list) {
    // shortcut: past kMaxAddrs addresses the history restarts; an LRU if scaling ever churns because of it.
    if (recs_.size() >= kMaxAddrs && recs_.find(addr) == recs_.end()) recs_.clear();
    auto [it, fresh] = recs_.try_emplace(addr, Rec{hash});
    Rec& r = it->second;
    if (fresh) return false;
    if (r.hash != hash) {
        r.hash = hash;
        r.older = r.newer;
        r.newer = list;
    }
    return r.older != kNone && list - r.older <= kWindow;
}

TexUpscaleQueue::TexUpscaleQueue(ScaleFn scale, size_t budget_bytes)
    : scale_(std::move(scale)),
      queue_([this](const std::string& key, int* w, int* h, std::vector<uint8_t>* rgba) {
          return scale_source(key, w, h, rgba);
      }, budget_bytes) {}

TexUpscaleQueue::State TexUpscaleQueue::request(uint64_t key, int factor, const uint8_t* rgba, int w, int h,
                                                const TexUpscaleImage** out) {
    const std::string k = std::to_string(key);
    // Held across queue_.request so the worker cannot take a new key before its source is stored.
    std::lock_guard<std::mutex> lk(mu_);
    const State s = queue_.request(k, out);
    if (s == State::Pending) {
        if (sources_.find(k) == sources_.end())
            sources_[k] = Source{factor, w, h, std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(w) * h * 4)};
    } else {
        sources_.erase(k);
    }
    return s;
}

void TexUpscaleQueue::release(uint64_t key) {
    queue_.release(std::to_string(key));
}

bool TexUpscaleQueue::scale_source(const std::string& key, int* w, int* h, std::vector<uint8_t>* rgba) {
    Source src;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sources_.find(key);
        if (it == sources_.end()) return false;
        src = std::move(it->second);
        sources_.erase(it);
    }
    if (!scale_(src.factor, src.rgba.data(), src.w, src.h, rgba)) return false;
    *w = src.w * src.factor;
    *h = src.h * src.factor;
    return true;
}
