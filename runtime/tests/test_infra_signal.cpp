// infra_signal: sceNetApctlGetInfo(STRENGTH) from the infrastructure server RTT.
#include "net/infra_signal.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

int main() {
    CHECK(infra::strength_from_rtt(-1) == 99, "no measurement yet: full");
    CHECK(infra::strength_from_rtt(0) == 99 && infra::strength_from_rtt(80) == 99, "<= 80 ms: 99");
    CHECK(infra::strength_from_rtt(81) == 75 && infra::strength_from_rtt(150) == 75, "<= 150 ms: 75");
    CHECK(infra::strength_from_rtt(151) == 50 && infra::strength_from_rtt(250) == 50, "<= 250 ms: 50");
    CHECK(infra::strength_from_rtt(251) == 25 && infra::strength_from_rtt(400) == 25, "<= 400 ms: 25");
    CHECK(infra::strength_from_rtt(401) == 5 && infra::strength_from_rtt(5000) == 5, "worse: 5");
    // Ping under each hero: the direct route when measured, else both server RTTs.
    CHECK(infra::peer_ping_ms(35, 60, 90) == 35, "direct route wins");
    CHECK(infra::peer_ping_ms(-1, 60, 90) == 150, "relay: ours + theirs");
    CHECK(infra::peer_ping_ms(-1, -1, 90) == -1 && infra::peer_ping_ms(-1, 60, -1) == -1, "a half unknown: unknown");
    using Q = infra::PingQuality;
    CHECK(infra::ping_quality(-1) == Q::Unknown, "unknown");
    CHECK(infra::ping_quality(0) == Q::Good && infra::ping_quality(79) == Q::Good, "< 80 good");
    CHECK(infra::ping_quality(80) == Q::Fair && infra::ping_quality(149) == Q::Fair, "< 150 fair");
    CHECK(infra::ping_quality(150) == Q::Poor && infra::ping_quality(5000) == Q::Poor, ">= 150 poor");
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("infra_signal: all tests passed\n");
    return 0;
}
