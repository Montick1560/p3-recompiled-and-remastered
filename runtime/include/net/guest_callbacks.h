#pragma once
// Runs guest callback functions (ad hoc handlers) that the network layer
// raises from host threads. Calls are queued and executed in FIFO order by
// one runtime thread that takes the guest CPU like any guest code, on its
// own guest stack (the way GE and interrupt callbacks run).
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

#include <vector>

struct GuestCall {
    uint32_t fn = 0;
    uint32_t a[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // a0..a3, t0..t3
    // Optional data the call points at: the runner copies it into a guest
    // scratch buffer right before the call (so it stays valid until the
    // handler returns) and adds the buffer address to every argument whose
    // bit is set in buf_args.
    std::vector<uint8_t> buf;
    uint8_t buf_args = 0;
};

// Thread-safe FIFO; push never blocks, so a callback that triggers new
// events (re-entering the network HLE) cannot deadlock the runner.
class GuestCallQueue {
public:
    void push(const GuestCall& c) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            q_.push_back(c);
        }
        cv_.notify_one();
    }
    // Next call, false after timeout_ms or once stopped.
    bool pop(GuestCall* out, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this] { return stopped_ || !q_.empty(); });
        if (stopped_ || q_.empty()) return false;
        *out = q_.front();
        q_.pop_front();
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopped_ = true;
        }
        cv_.notify_all();
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return q_.size();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<GuestCall> q_;
    bool stopped_ = false;
};

// Queue a call. The runner starts on the first post.
void guest_callbacks_post(uint8_t* rdram, GuestCall call);
inline void guest_callbacks_post(uint8_t* rdram, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    GuestCall c;
    c.fn = fn;
    c.a[0] = a0;
    c.a[1] = a1;
    c.a[2] = a2;
    guest_callbacks_post(rdram, std::move(c));
}

// Arguments after the buffer is placed at guest address buf_addr (pure).
inline void guest_call_resolve(const GuestCall& c, uint32_t buf_addr, uint32_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = c.a[i] + (((c.buf_args >> i) & 1) ? buf_addr : 0);
}
