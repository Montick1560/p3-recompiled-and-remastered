#pragma once
// Relay-link telemetry of one infrastructure session: ping RTTs and the longest
// gap between server frames while a ping was outstanding (a TCP stall shows as
// one). infra::Client feeds it and sends the summary as a link_stats request.
// Numbers only: no address of any kind. Thread-safe.
#include <deque>
#include <mutex>

#include "json.hpp"

namespace infra {

constexpr int kSlowPingMs = 250;
constexpr size_t kLinkRttWindow = 4096;  // newest RTTs kept for the percentiles

struct LinkSummary {
    int pings = 0, slow_pings = 0;                 // answered pings; those with RTT >= kSlowPingMs
    int rtt_p50 = -1, rtt_p95 = -1, rtt_max = -1;  // over the window; -1 without pings
    int max_rx_gap_ms = 0;                         // longest gap between server frames while a ping was out
};

class LinkStats {
public:
    void ping_rtt(int ms);
    void rx_gap(int ms);
    LinkSummary summary() const;
    void reset();

private:
    mutable std::mutex mu_;
    std::deque<int> rtts_;
    int pings_ = 0, slow_ = 0, max_gap_ = 0;
};

// {"pings","slow_pings","rtt_p50","rtt_p95","rtt_max","max_rx_gap_ms"}, all integers.
nlohmann::json to_json(const LinkSummary& s);

}  // namespace infra
