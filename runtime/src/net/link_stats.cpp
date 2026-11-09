// Relay-link telemetry of one infrastructure session (see net/link_stats.h).
#include "net/link_stats.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace infra {

namespace {

std::atomic<int64_t> g_last_present_us{0};  // 0: no frame yet in this session
std::atomic<int> g_frames{0}, g_slow_frames{0}, g_frame_max_ms{0};

// Nearest rank: the element at index ceil(p * n) - 1 of the sorted values (p in percent).
int nearest_rank(const std::vector<int>& sorted, int p) {
    const size_t n = sorted.size();
    const size_t rank = (static_cast<size_t>(p) * n + 99) / 100;
    return sorted[rank > 0 ? rank - 1 : 0];
}

}  // namespace

void frame_presented(int64_t now_us) {
    const int64_t prev = g_last_present_us.exchange(now_us, std::memory_order_relaxed);
    if (prev == 0) return;
    const int ms = static_cast<int>(std::min<int64_t>((now_us - prev) / 1000, kLinkNumberMax));
    if (g_frames.load(std::memory_order_relaxed) < kLinkNumberMax) g_frames.fetch_add(1, std::memory_order_relaxed);
    if (ms >= kSlowFrameMs && g_slow_frames.load(std::memory_order_relaxed) < kLinkNumberMax)
        g_slow_frames.fetch_add(1, std::memory_order_relaxed);
    if (ms > g_frame_max_ms.load(std::memory_order_relaxed)) g_frame_max_ms.store(ms, std::memory_order_relaxed);
}

void LinkStats::ping_rtt(int ms) {
    std::lock_guard<std::mutex> lock(mu_);
    pings_++;
    if (ms >= kSlowPingMs) slow_++;
    rtts_.push_back(ms);
    if (rtts_.size() > kLinkRttWindow) rtts_.pop_front();
}

void LinkStats::rx_gap(int ms) {
    std::lock_guard<std::mutex> lock(mu_);
    max_gap_ = std::max(max_gap_, ms);
}

LinkSummary LinkStats::summary() const {
    LinkSummary s;
    s.frames = g_frames.load(std::memory_order_relaxed);
    s.slow_frames = g_slow_frames.load(std::memory_order_relaxed);
    s.frame_max_ms = g_frame_max_ms.load(std::memory_order_relaxed);
    std::vector<int> v;
    {
        std::lock_guard<std::mutex> lock(mu_);
        s.pings = pings_;
        s.slow_pings = slow_;
        s.max_rx_gap_ms = max_gap_;
        v.assign(rtts_.begin(), rtts_.end());
    }
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.rtt_p50 = nearest_rank(v, 50);
    s.rtt_p95 = nearest_rank(v, 95);
    s.rtt_max = v.back();
    return s;
}

void LinkStats::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    rtts_.clear();
    pings_ = slow_ = max_gap_ = 0;
    g_last_present_us = 0;
    g_frames = g_slow_frames = g_frame_max_ms = 0;
}

nlohmann::json to_json(const LinkSummary& s) {
    return {{"pings", s.pings},     {"slow_pings", s.slow_pings},   {"rtt_p50", s.rtt_p50},
            {"rtt_p95", s.rtt_p95}, {"rtt_max", s.rtt_max},         {"max_rx_gap_ms", s.max_rx_gap_ms},
            {"frames", s.frames},   {"slow_frames", s.slow_frames}, {"frame_max_ms", s.frame_max_ms}};
}

}  // namespace infra
