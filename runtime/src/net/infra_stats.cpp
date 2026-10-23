// Network event log of infrastructure mode (see net/infra_stats.h).
#include "net/infra_stats.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace infra::stats {

namespace {

std::mutex g_mu;
FILE* g_file = nullptr;
std::atomic<bool> g_on{false};
int64_t g_last_flush_us = 0;
std::once_flag g_env_once;

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
    std::call_once(g_env_once, [] {
        const char* p = std::getenv("PSPRECOMP_INFRA_STATS");
        if (p && p[0] && !g_on) open(p);
    });
}

}  // namespace

bool open(const char* path) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) std::fclose(g_file);
    g_on = false;
    g_file = std::fopen(path, "w");
    if (!g_file) {
        std::fprintf(stderr, "[INFRA] stats: cannot open %s\n", path);
        return false;
    }
    std::setvbuf(g_file, nullptr, _IOFBF, 64 * 1024);
    std::fputs("t_us,ev,kind,vip,src,dst,len,hash,extra_us\n", g_file);
    g_last_flush_us = now_us();
    g_on = true;
    std::fprintf(stderr, "[INFRA] stats: writing %s\n", path);
    return true;
}

bool enabled() {
    read_env();
    return g_on.load(std::memory_order_relaxed);
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
    const uint32_t h = hash(data, len);
    const int64_t t = now_us();
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_file) return;
    std::fprintf(g_file, "%lld,%s,%u,%08x,%u,%u,%zu,%08x,%lld\n", static_cast<long long>(t), ev_name(ev),
                 static_cast<unsigned>(kind), vip, static_cast<unsigned>(src), static_cast<unsigned>(dst), len, h,
                 static_cast<long long>(extra_us));
    if (t - g_last_flush_us >= 1000000) {  // a kill loses at most the last second
        std::fflush(g_file);
        g_last_flush_us = t;
    }
}

void close() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_on = false;
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

}  // namespace infra::stats
