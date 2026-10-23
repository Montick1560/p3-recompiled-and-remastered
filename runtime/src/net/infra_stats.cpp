// Network event log of infrastructure mode (see net/infra_stats.h).
#include "net/infra_stats.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace infra::stats {

namespace {

constexpr size_t kWriteChunk = 64 * 1024;
constexpr size_t kMaxPending = 16 * 1024 * 1024;

// Leaked on purpose (like infra::client()): the stats thread may still run when the process exits.
struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::string pending;  // rows not yet written
    FILE* file = nullptr;
    std::thread* writer = nullptr;
    bool stop = false;
    size_t dropped = 0;
    std::function<void()> hook;
    std::atomic<bool> on{false};
    std::once_flag env_once;
};

State& st() {
    static State* s = new State;
    return *s;
}

const char* ev_name(Ev ev) {
    switch (ev) {
    case Ev::TxCall: return "tx_call";
    case Ev::TxDone: return "tx_done";
    case Ev::Rx: return "rx";
    case Ev::Pop: return "pop";
    case Ev::Drop: return "drop";
    }
    return "?";
}

void read_env() {
    std::call_once(st().env_once, [] {
        const char* p = std::getenv("PSPRECOMP_INFRA_STATS");
        if (p && p[0] && !st().on) open(p);
    });
}

// The stats thread: writes what record() queued, at least once a second.
void write_loop() {
    State& s = st();
    std::unique_lock<std::mutex> lock(s.mu);
    for (;;) {
        s.cv.wait_for(lock, std::chrono::seconds(1), [&] { return s.stop || s.pending.size() >= kWriteChunk; });
        std::string chunk;
        chunk.swap(s.pending);
        const bool stop = s.stop;
        const std::function<void()> hook = s.hook;
        FILE* f = s.file;
        lock.unlock();
        if (!chunk.empty()) {
            if (hook) hook();
            std::fwrite(chunk.data(), 1, chunk.size(), f);
            std::fflush(f);  // a kill loses at most the last second
        }
        lock.lock();
        if (stop && s.pending.empty()) return;
    }
}

}  // namespace

bool open(const char* path) {
    close();
    FILE* f = std::fopen(path, "w");
    if (!f) {
        std::fprintf(stderr, "[INFRA] stats: cannot open %s\n", path);
        return false;
    }
    std::fputs("t_us,ev,kind,vip,src,dst,len,hash,extra_us\n", f);
    State& s = st();
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.file = f;
        s.pending.clear();
        s.stop = false;
        s.dropped = 0;
        s.writer = new std::thread(write_loop);
    }
    s.on = true;
    std::fprintf(stderr, "[INFRA] stats: writing %s\n", path);
    return true;
}

bool enabled() {
    read_env();
    return st().on.load(std::memory_order_relaxed);
}

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint32_t hash(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

void record(Ev ev, uint8_t kind, uint32_t vip, uint16_t src, uint16_t dst, const uint8_t* data, size_t len,
            int64_t extra_us) {
    if (!enabled()) return;
    char row[160];
    const int n = std::snprintf(row, sizeof(row), "%lld,%s,%u,%08x,%u,%u,%zu,%08x,%lld\n",
                                static_cast<long long>(now_us()), ev_name(ev), static_cast<unsigned>(kind), vip,
                                static_cast<unsigned>(src), static_cast<unsigned>(dst), len, hash(data, len),
                                static_cast<long long>(extra_us));
    if (n <= 0) return;
    State& s = st();
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (!s.file || s.stop) return;
        if (s.pending.size() + static_cast<size_t>(n) > kMaxPending) {
            s.dropped++;
            return;
        }
        s.pending.append(row, static_cast<size_t>(n));
        wake = s.pending.size() >= kWriteChunk;
    }
    if (wake) s.cv.notify_one();
}

void close() {
    State& s = st();
    std::thread* writer;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        s.on = false;
        writer = s.writer;
        if (!writer) return;
        s.stop = true;
    }
    s.cv.notify_one();
    writer->join();
    delete writer;
    std::lock_guard<std::mutex> lock(s.mu);
    std::fclose(s.file);
    s.file = nullptr;
    s.writer = nullptr;
    if (s.dropped)
        std::fprintf(stderr, "[INFRA] stats: %zu rows dropped (the disk did not keep up)\n", s.dropped);
}

void set_write_hook_for_test(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(st().mu);
    st().hook = std::move(hook);
}

}  // namespace infra::stats
