// net/link_stats.h: relay-link numbers of one session (ping RTT percentiles, slow pings, the longest
// receive gap while a ping was outstanding), sent to the server as link_stats.
#include "net/link_stats.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    {   // nothing measured
        infra::LinkStats l;
        const infra::LinkSummary s = l.summary();
        CHECK(s.pings == 0 && s.slow_pings == 0, "empty: no pings");
        CHECK(s.rtt_p50 == -1 && s.rtt_p95 == -1 && s.rtt_max == -1, "empty: -1 percentiles");
        CHECK(s.max_rx_gap_ms == 0, "empty: no gap");
    }
    {   // nearest rank: index ceil(p*n)-1 of the sorted RTTs
        infra::LinkStats l;
        for (int ms = 100; ms >= 1; ms--) l.ping_rtt(ms);
        const infra::LinkSummary s = l.summary();
        CHECK(s.rtt_p50 == 50 && s.rtt_p95 == 95 && s.rtt_max == 100, "1..100 -> p50 50, p95 95, max 100");
        CHECK(s.pings == 100, "100 pings");
    }
    {   // slow means >= 250 ms
        infra::LinkStats l;
        for (int ms : {100, 249, 250, 900}) l.ping_rtt(ms);
        CHECK(l.summary().slow_pings == 2, "250 and 900 are slow");
    }
    {   // the gap is the longest one
        infra::LinkStats l;
        l.rx_gap(30);
        l.rx_gap(700);
        l.rx_gap(10);
        CHECK(l.summary().max_rx_gap_ms == 700, "max gap 700");
    }
    {   // the percentile window keeps the newest 4096 RTTs; the counters cover every ping
        infra::LinkStats l;
        for (int i = 0; i < 5000; i++) l.ping_rtt(i < 904 ? 10000 : 50);
        const infra::LinkSummary s = l.summary();
        CHECK(s.rtt_max == 50 && s.rtt_p95 == 50, "the 904 oldest RTTs left the window");
        CHECK(s.pings == 5000 && s.slow_pings == 904, "counts are of all answered pings");
    }
    {   // reset starts a new session
        infra::LinkStats l;
        l.ping_rtt(300);
        l.rx_gap(500);
        l.reset();
        const infra::LinkSummary s = l.summary();
        CHECK(s.pings == 0 && s.slow_pings == 0 && s.rtt_max == -1 && s.max_rx_gap_ms == 0, "reset clears all");
    }
    {   // presented frames: slow means 100 ms or more; the gap before the first frame of a session is not one
        infra::LinkStats l;
        l.reset();
        int64_t t = 5000000;
        infra::frame_presented(t);
        for (int i = 0; i < 10; i++) infra::frame_presented(t += 16000);
        CHECK(l.summary().frames == 10 && l.summary().slow_frames == 0, "10 frames, none slow");
        infra::frame_presented(t += 99999);
        CHECK(l.summary().slow_frames == 0, "99.999 ms is not slow");
        infra::frame_presented(t += 150000);
        const infra::LinkSummary s = l.summary();
        CHECK(s.frames == 12 && s.slow_frames == 1 && s.frame_max_ms == 150, "one slow frame of 150 ms");
        l.reset();
        CHECK(l.summary().frames == 0 && l.summary().slow_frames == 0 && l.summary().frame_max_ms == 0, "reset: frames");
        infra::frame_presented(t += 9000000);
        CHECK(l.summary().frames == 0 && l.summary().frame_max_ms == 0, "the time before the session does not count");
        infra::frame_presented(t += 2000000000000);
        CHECK(l.summary().frame_max_ms == infra::kLinkNumberMax, "numbers stay in the server's range");
        l.reset();
    }
    {   // the request body: exactly nine integer keys, no address
        infra::LinkStats l;
        l.ping_rtt(40);
        l.rx_gap(120);
        const nlohmann::json j = infra::to_json(l.summary());
        CHECK(j.is_object() && j.size() == 9, "nine keys");
        bool ints = true;
        for (const char* k : {"pings", "slow_pings", "rtt_p50", "rtt_p95", "rtt_max", "max_rx_gap_ms", "frames",
                              "slow_frames", "frame_max_ms"})
            ints &= j.contains(k) && j[k].is_number_integer();
        CHECK(ints, "all nine present and integers");
        CHECK(j["pings"] == 1 && j["rtt_p50"] == 40 && j["max_rx_gap_ms"] == 120, "values carried");
    }
    {   // frame pace: frames of 20 ms or more missed 60 fps, of 40 ms or more missed 30 fps; reset clears them
        infra::LinkStats l;
        l.reset();
        int64_t t = 7000000;
        infra::frame_presented(t);
        for (int ms : {16, 19, 20, 39, 40, 100}) infra::frame_presented(t += ms * 1000);
        infra::FramePace p = infra::frame_pace();
        CHECK(p.frames_over_20 == 4 && p.frames_over_40 == 2, "20/39/40/100 miss 60 fps; 40/100 miss 30 fps");
        CHECK(l.summary().slow_frames == 1, "only 100 ms is slow");
        infra::set_pc_profile(2, infra::kGpuIntegrated);
        infra::set_fps_target(60);
        p = infra::frame_pace();
        CHECK(p.fps_target == 60 && p.render_scale == 2 && p.gpu_class == infra::kGpuIntegrated, "profile carried");
        const nlohmann::json j = infra::to_json(p);
        bool ints = j.is_object() && j.size() == 5;
        for (const char* k : {"frames_over_20", "frames_over_40", "fps_target", "render_scale", "gpu_class"})
            ints &= j.contains(k) && j[k].is_number_integer();
        CHECK(ints, "frame_pace body: five integer keys");
        l.reset();
        p = infra::frame_pace();
        CHECK(p.frames_over_20 == 0 && p.frames_over_40 == 0, "reset clears the pace counts");
        CHECK(p.fps_target == 60 && p.gpu_class == infra::kGpuIntegrated, "reset keeps the PC profile");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("link_stats: all tests passed\n");
    return 0;
}
