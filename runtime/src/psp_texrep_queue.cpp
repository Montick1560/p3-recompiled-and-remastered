#include "psp_texrep_queue.h"

TexrepImageQueue::TexrepImageQueue(DecodeFn decode, size_t budget_bytes)
    : decode_(std::move(decode)), lru_(budget_bytes), thread_([this] { worker(); }) {}

TexrepImageQueue::~TexrepImageQueue() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    thread_.join();
}

TexrepImageQueue::State TexrepImageQueue::request(const std::string& path, const TexrepImage** out) {
    *out = nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(path);
    if (it == entries_.end()) {
        entries_[path];
        queue_.push_back(path);
        cv_.notify_one();
        return State::Pending;
    }
    Entry& e = it->second;
    if (e.state == State::Ready) {
        lru_.touch(path);
        evict_locked(path);
        *out = e.image.get();
    }
    return e.state;
}

void TexrepImageQueue::release(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(path);
    if (it == entries_.end() || it->second.state != State::Ready) return;
    lru_.remove(path);
    entries_.erase(it);
}

uint64_t TexrepImageQueue::ready_epoch() const {
    std::lock_guard<std::mutex> lk(mu_);
    return epoch_;
}

void TexrepImageQueue::wait_idle() {
    std::unique_lock<std::mutex> lk(mu_);
    idle_cv_.wait(lk, [this] { return queue_.empty() && !busy_; });
}

void TexrepImageQueue::evict_locked(const std::string& keep) {
    for (const std::string& victim : lru_.trim({keep})) {
        entries_.erase(victim);  // a later request decodes it again
    }
}

void TexrepImageQueue::worker() {
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            path = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        auto img = std::make_unique<TexrepImage>();
        const bool ok = decode_(path, &img->w, &img->h, &img->rgba);
        {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = false;
            auto it = entries_.find(path);
            if (it != entries_.end()) {
                if (ok) {
                    // Insert only: the render thread trims the budget, so an
                    // image handed out by request() stays valid until its next
                    // call on the render thread.
                    lru_.put(path, img->rgba.size());
                    it->second.image = std::move(img);
                    it->second.state = State::Ready;
                } else {
                    it->second.state = State::Failed;
                }
            }
            epoch_++;
            if (queue_.empty()) idle_cv_.notify_all();
        }
    }
}
