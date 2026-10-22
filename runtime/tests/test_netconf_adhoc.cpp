// netconf_adhoc: what the sceUtilityNetconf dialog does on each status poll.
#include "net/netconf_adhoc.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

using netconf::Command;

static void connects() {
    netconf::Input in;
    in.action = netconf::CREATE_ADHOC;
    auto s = netconf::step(in);
    CHECK(!s.finished && s.command == Command::Create, "create requested first");
    in.requested = true;
    in.elapsed_ms = 300;
    s = netconf::step(in);
    CHECK(!s.finished && s.command == Command::None, "then waits");
    in.adhocctl_state = 1;  // CONNECTED
    s = netconf::step(in);
    CHECK(s.finished && s.result == netconf::kResultSuccess, "finishes when connected");

    netconf::Input c;
    c.action = netconf::CONNECT_ADHOC;
    CHECK(netconf::step(c).command == Command::Connect, "connect action uses Connect");
}

static void netconf_times_out() {
    netconf::Input in;
    in.action = netconf::CONNECT_ADHOC;
    in.requested = true;
    in.elapsed_ms = netconf::kConnectTimeoutMs - 1;
    CHECK(!netconf::step(in).finished, "still waiting before the timeout");
    in.elapsed_ms = netconf::kConnectTimeoutMs + 1;
    auto s = netconf::step(in);
    CHECK(s.finished && s.result == netconf::kResultAbort, "aborts after 15 s without a group");
    in.have_group = false;
    in.requested = false;
    in.elapsed_ms = 0;
    CHECK(netconf::step(in).command == Command::None, "no group data: nothing to request");
}

static void infra_action_aborts_without_infra() {
    for (int a : {int(netconf::CONNECT_APNET), int(netconf::STATUS_APNET), int(netconf::CONNECT_APNET_LAST), 9}) {
        netconf::Input in;
        in.action = a;
        auto s = netconf::step(in);
        CHECK(s.finished && s.result == netconf::kResultAbort, "infrastructure off/unknown actions abort");
    }
    netconf::Input in;
    in.action = 9;
    in.infra_ok = true;
    CHECK(netconf::step(in).finished && netconf::step(in).result == netconf::kResultAbort, "unknown action aborts");
}

static void infra_action_ok_when_infra_ok() {
    netconf::Input in;
    in.action = netconf::CONNECT_APNET;
    in.infra_ok = true;
    auto s = netconf::step(in);
    CHECK(!s.finished && s.command == Command::InfraConnect, "infrastructure: connect to the server first");
    in.requested = true;
    in.infra_pending = true;
    s = netconf::step(in);
    CHECK(!s.finished && s.command == Command::None, "waits while connecting");
    in.infra_pending = false;
    s = netconf::step(in);
    CHECK(s.finished && s.result == netconf::kResultSuccess, "then finishes with success");
    in.action = netconf::CONNECT_APNET_LAST;
    CHECK(netconf::step(in).finished && netconf::step(in).result == netconf::kResultSuccess, "CONNECT_APNET_LAST too");
}

static void join_scans_until_group_seen() {
    netconf::Input in;
    in.action = netconf::JOIN_ADHOC;
    CHECK(netconf::step(in).command == Command::Scan, "join scans first");
    in.adhocctl_state = 2;  // SCANNING
    CHECK(netconf::step(in).command == Command::None, "nothing while scanning");
    in.adhocctl_state = 0;
    in.since_scan_ms = 100;
    CHECK(netconf::step(in).command == Command::None, "rescans are rate limited");
    in.since_scan_ms = netconf::kRescanMs;
    CHECK(netconf::step(in).command == Command::Scan, "group not listed: scan again");
    in.group_seen = true;
    CHECK(netconf::step(in).command == Command::Join, "group listed: join");
}

int main() {
    connects();
    netconf_times_out();
    infra_action_aborts_without_infra();
    infra_action_ok_when_infra_ok();
    join_scans_until_group_seen();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("netconf_adhoc: all passed\n");
    return 0;
}
