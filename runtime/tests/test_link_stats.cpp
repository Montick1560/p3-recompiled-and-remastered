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
    {   // the request body: exactly six integer keys, no address
        infra::LinkStats l;
        l.ping_rtt(40);
        l.rx_gap(120);
        const nlohmann::json j = infra::to_json(l.summary());
        CHECK(j.is_object() && j.size() == 6, "six keys");
        bool ints = true;
        for (const char* k : {"pings", "slow_pings", "rtt_p50", "rtt_p95", "rtt_max", "max_rx_gap_ms"})
            ints &= j.contains(k) && j[k].is_number_integer();
        CHECK(ints, "all six present and integers");
        CHECK(j["pings"] == 1 && j["rtt_p50"] == 40 && j["max_rx_gap_ms"] == 120, "values carried");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("link_stats: all tests passed\n");
    return 0;
}
