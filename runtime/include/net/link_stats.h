#pragma once
// Relay-link telemetry of one infrastructure session: ping RTTs and the longest
// gap between server frames while a ping was outstanding (a TCP stall shows as
// one). infra::Client feeds it and sends the summary as a link_stats request.
// Presented frames tell a slow PC from a slow network. Numbers only: no address
// of any kind. Thread-safe.
#include <cstdint>
#include <deque>
#include <mutex>

#include "json.hpp"

namespace infra {

constexpr int kSlowPingMs = 250;
constexpr size_t kLinkRttWindow = 4096;  // newest RTTs kept for the percentiles
constexpr int kSlowFrameMs = 100;
constexpr int kLinkNumberMax = 600000;   // tools/infra_server.py LINK_MAX: a bigger number is a refused report

struct LinkSummary {
    int pings = 0, slow_pings = 0;                 // answered pings; those with RTT >= kSlowPingMs
    int rtt_p50 = -1, rtt_p95 = -1, rtt_max = -1;  // over the window; -1 without pings
    int max_rx_gap_ms = 0;                         // longest gap between server frames while a ping was out
    int frames = 0, slow_frames = 0;               // presented frames; those taking >= kSlowFrameMs
    int frame_max_ms = 0;
};

// Once per presented frame (lock-free: the render path never waits for it). One process, one set of counts; a
// session's LinkStats::reset starts them over.
void frame_presented(int64_t now_us);

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

// {"pings","slow_pings","rtt_p50","rtt_p95","rtt_max","max_rx_gap_ms","frames","slow_frames","frame_max_ms"},
// all integers.
nlohmann::json to_json(const LinkSummary& s);

}  // namespace infra
