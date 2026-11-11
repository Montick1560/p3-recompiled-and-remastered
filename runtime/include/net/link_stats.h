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

// How the frames of this session kept pace, and what the PC was asked to do: sent as frame_pace (an older
// server refuses the op and keeps link_stats). Counts cover the same frames as LinkSummary::frames.
constexpr int kGpuUnknown = 0, kGpuIntegrated = 1, kGpuDedicated = 2, kGpuSoftware = 3;
struct FramePace {
    int frames_over_20 = 0, frames_over_40 = 0;  // frames of >= 20 ms (missed 60 fps), >= 40 ms (missed 30 fps)
    int fps_target = 0, render_scale = 0, gpu_class = 0;  // 30 or 60; PSPRECOMP_RENDER_SCALE; kGpu*
};
// Once at start (runtime: scale and GPU class; game module: the frame rate it runs at); LinkStats::reset keeps them.
void set_pc_profile(int render_scale, int gpu_class);
void set_fps_target(int fps);
FramePace frame_pace();
// {"frames_over_20","frames_over_40","fps_target","render_scale","gpu_class"}, all integers.
nlohmann::json to_json(const FramePace& p);

// {"pings","slow_pings","rtt_p50","rtt_p95","rtt_max","max_rx_gap_ms","frames","slow_frames","frame_max_ms"},
// all integers.
nlohmann::json to_json(const LinkSummary& s);

}  // namespace infra
