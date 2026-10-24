// Relay-link telemetry of one infrastructure session (see net/link_stats.h).
#include "net/link_stats.h"

#include <algorithm>
#include <vector>

namespace infra {

namespace {

// Nearest rank: the element at index ceil(p * n) - 1 of the sorted values (p in percent).
int nearest_rank(const std::vector<int>& sorted, int p) {
    const size_t n = sorted.size();
    const size_t rank = (static_cast<size_t>(p) * n + 99) / 100;
    return sorted[rank > 0 ? rank - 1 : 0];
}

}  // namespace

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
}

nlohmann::json to_json(const LinkSummary& s) {
    return {{"pings", s.pings},     {"slow_pings", s.slow_pings}, {"rtt_p50", s.rtt_p50},
            {"rtt_p95", s.rtt_p95}, {"rtt_max", s.rtt_max},       {"max_rx_gap_ms", s.max_rx_gap_ms}};
}

}  // namespace infra
