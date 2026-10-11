// matching: PSP ad hoc lobby protocol between contexts wired in memory.
#include "net/matching.h"

#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

using matching::Context;

static pro::Mac mac_of(uint8_t n) { return pro::Mac{0, 0, 0, 0, 0, n}; }

struct Ev {
    int event;
    pro::Mac peer;
    std::vector<uint8_t> opt;
};

// A tiny network: every context sees all the others as group peers.
struct Net {
    struct Node {
        std::unique_ptr<Context> ctx;
        std::vector<Ev> events;
    };
    std::map<pro::Mac, Node> nodes;
    struct Datagram {
        pro::Mac from, to;
        std::vector<uint8_t> bytes;
    };
    std::deque<Datagram> wire;
    uint64_t now = 1000000;
    bool drop = false;

    Context& add(uint8_t n, matching::Mode mode, int maxpeers, uint32_t hello_us = 500000,
                 int32_t keepalive_us = 300000, int init_count = 3, uint32_t rexmt_us = 200000) {
        const pro::Mac me = mac_of(n);
        matching::Io io;
        io.send = [this, me](const pro::Mac& to, uint16_t, const std::vector<uint8_t>& b) {
            if (!drop) wire.push_back({me, to, b});
        };
        io.group_peers = [this, me] {
            std::vector<pro::Mac> out;
            for (auto& kv : nodes) if (kv.first != me) out.push_back(kv.first);
            return out;
        };
        io.notify = [this, me](int e, const pro::Mac& p, const std::vector<uint8_t>& opt) {
            nodes[me].events.push_back({e, p, opt});
        };
        nodes[me].ctx = std::make_unique<Context>(mode, maxpeers, 300, hello_us, keepalive_us, init_count,
                                                  rexmt_us, me, io);
        return *nodes[me].ctx;
    }
    Context& ctx(uint8_t n) { return *nodes[mac_of(n)].ctx; }
    std::vector<Ev>& events(uint8_t n) { return nodes[mac_of(n)].events; }
    void deliver() {
        while (!wire.empty()) {
            Datagram d = wire.front();
            wire.pop_front();
            auto it = nodes.find(d.to);
            if (it != nodes.end()) it->second.ctx->on_packet(d.from, 300, d.bytes.data(), d.bytes.size(), now);
        }
    }
    void tick_all(uint64_t step_us) {
        now += step_us;
        for (auto& kv : nodes) kv.second.ctx->tick(now);
        deliver();
    }
    bool has(uint8_t n, int event, uint8_t peer) {
        for (const auto& e : events(n)) if (e.event == event && e.peer == mac_of(peer)) return true;
        return false;
    }
};

static void hello_offer() {
    Net net;
    net.add(1, matching::PARENT, 4).start({0xAB, 0xCD});
    net.add(2, matching::CHILD, 4).start({});
    net.tick_all(1);
    CHECK(net.has(2, matching::EV_HELLO, 1), "child hears the parent's hello");
    CHECK(!net.events(2).empty() && net.events(2)[0].opt == (std::vector<uint8_t>{0xAB, 0xCD}),
          "hello carries the hello opt");
    CHECK(net.ctx(2).peer_state(mac_of(1)) == matching::PEER_OFFER, "parent is an offer");
    CHECK(net.events(1).empty(), "a parent ignores hellos");
}

static void join_accept_and_siblings() {
    Net net;
    net.add(1, matching::PARENT, 4).start({1});
    net.add(2, matching::CHILD, 4).start({});
    net.add(3, matching::CHILD, 4).start({});
    net.tick_all(1);
    CHECK(net.ctx(2).select_target(mac_of(1), {9}, net.now) == 0, "child 2 asks to join");
    net.deliver();
    CHECK(net.has(1, matching::EV_REQUEST, 2), "parent gets the request");
    CHECK(net.ctx(1).peer_state(mac_of(2)) == matching::PEER_INCOMING_REQUEST, "incoming request");
    CHECK(net.ctx(1).select_target(mac_of(2), {7}, net.now) == 0, "parent accepts");
    CHECK(net.has(1, matching::EV_ESTABLISHED, 2), "parent: established");
    net.deliver();
    CHECK(net.has(2, matching::EV_ESTABLISHED, 1) && net.has(2, matching::EV_ACCEPT, 1), "child: established + accept");
    CHECK(net.ctx(2).members() == (std::vector<pro::Mac>{mac_of(1), mac_of(2)}), "child members [parent, self]");
    CHECK(net.ctx(1).members() == (std::vector<pro::Mac>{mac_of(1), mac_of(2)}), "parent members [self, child]");
    CHECK(net.ctx(2).select_target(mac_of(1), {}, net.now) == matching::kErrAlreadyEstablished, "already established");

    net.ctx(3).select_target(mac_of(1), {}, net.now);
    net.deliver();
    net.ctx(1).select_target(mac_of(3), {}, net.now);
    net.deliver();
    const std::vector<pro::Mac> all = {mac_of(1), mac_of(2), mac_of(3)};
    CHECK(net.ctx(1).members() == all, "parent lists both children oldest first");
    CHECK(net.ctx(3).members() == all, "new child learns its sibling from ACCEPT");
    CHECK(net.ctx(2).members() == all, "old child learns the new one from BIRTH");
    CHECK(net.ctx(1).connected_count() == 3 && net.ctx(2).connected_count() == 3, "connected counts");
}

static void deny_and_maxpeers() {
    Net net;
    net.add(1, matching::PARENT, 2).start({});  // room for one child
    net.add(2, matching::CHILD, 2).start({});
    net.add(3, matching::CHILD, 2).start({});
    net.add(4, matching::CHILD, 2).start({});
    net.tick_all(1);
    net.ctx(2).select_target(mac_of(1), {}, net.now);
    net.deliver();
    CHECK(net.ctx(1).cancel_target(mac_of(2), {5}, net.now) == 0, "parent refuses child 2");
    net.deliver();
    CHECK(net.has(2, matching::EV_DENY, 1), "child 2 gets DENY");
    // PPSSPP keeps a denied request pending until the game cancels it.
    CHECK(net.ctx(2).select_target(mac_of(1), {}, net.now) == matching::kErrRequestInProgress,
          "denied request still pending");
    net.ctx(3).select_target(mac_of(1), {}, net.now);
    net.deliver();
    CHECK(net.ctx(1).select_target(mac_of(3), {}, net.now) == 0, "child 3 accepted");
    net.deliver();
    CHECK(net.ctx(1).select_target(mac_of(2), {}, net.now) == matching::kErrExceedMaxnum,
          "room full: EXCEED_MAXNUM is checked first (PPSSPP order)");
    net.ctx(4).select_target(mac_of(1), {}, net.now);
    net.deliver();
    CHECK(net.has(4, matching::EV_DENY, 1), "full parent answers JOIN with CANCEL -> DENY");
}

static void leave_and_bye() {
    Net net;
    net.add(1, matching::PARENT, 4).start({});
    net.add(2, matching::CHILD, 4).start({});
    net.tick_all(1);
    net.ctx(2).select_target(mac_of(1), {}, net.now);
    net.deliver();
    net.ctx(1).select_target(mac_of(2), {}, net.now);
    net.deliver();
    net.ctx(2).stop();
    net.deliver();
    CHECK(net.has(1, matching::EV_BYE, 2), "parent gets BYE when the child stops");
    CHECK(net.ctx(1).members() == std::vector<pro::Mac>{mac_of(1)}, "child removed");
}

static void timeout() {
    Net net;
    net.add(1, matching::PARENT, 4).start({});
    net.add(2, matching::CHILD, 4).start({});
    net.tick_all(1);
    net.ctx(2).select_target(mac_of(1), {}, net.now);
    net.deliver();
    net.ctx(1).select_target(mac_of(2), {}, net.now);
    net.deliver();
    net.tick_all(300000);
    CHECK(!net.has(1, matching::EV_TIMEOUT, 2), "pings keep the child alive");
    net.drop = true;  // the child goes silent
    for (int i = 0; i < 12; i++) {
        net.now += 300000;
        net.ctx(1).tick(net.now);
    }
    CHECK(net.has(1, matching::EV_TIMEOUT, 2), "silent child times out");
    CHECK(net.ctx(1).members() == std::vector<pro::Mac>{mac_of(1)}, "and is dropped");
}

static void p2p_pair() {
    Net net;
    net.add(1, matching::P2P, 2).start({1});
    net.add(2, matching::P2P, 2).start({2});
    net.tick_all(1);
    CHECK(net.has(1, matching::EV_HELLO, 2) && net.has(2, matching::EV_HELLO, 1), "both hear hellos");
    CHECK(net.ctx(1).select_target(mac_of(2), {}, net.now) == 0, "A joins B");
    net.deliver();
    CHECK(net.has(2, matching::EV_REQUEST, 1), "B gets the request");
    CHECK(net.ctx(2).select_target(mac_of(1), {}, net.now) == 0, "B accepts");
    net.deliver();
    CHECK(net.has(1, matching::EV_ESTABLISHED, 2) && net.has(1, matching::EV_ACCEPT, 2), "A established");
    CHECK(net.ctx(1).members() == (std::vector<pro::Mac>{mac_of(1), mac_of(2)}), "A members");
    CHECK(net.ctx(2).members() == (std::vector<pro::Mac>{mac_of(2), mac_of(1)}), "B members");
}

static void malformed_and_unknown() {
    Net net;
    net.add(1, matching::PARENT, 4).start({});
    Context& p = net.ctx(1);
    const uint8_t bad_join[3] = {2, 1, 0};
    p.on_packet(mac_of(9), 300, bad_join, sizeof(bad_join), net.now);
    CHECK(net.wire.size() == 1 && net.wire.front().bytes[0] == matching::CANCEL,
          "bad JOIN answered with CANCEL like PPSSPP");
    net.wire.clear();
    const uint8_t junk[2] = {0x42, 0};
    p.on_packet(mac_of(9), 300, junk, sizeof(junk), net.now);
    CHECK(net.wire.empty() && net.events(1).empty(), "unknown packet ignored");
    CHECK(p.select_target(mac_of(9), {}, net.now) == matching::kErrUnknownTarget, "unknown target");
    CHECK(p.set_hello_opt({1}) == 0, "parent sets hello opt");
    Context& c = net.add(2, matching::CHILD, 4);
    CHECK(c.set_hello_opt({1}) == matching::kErrInvalidMode, "child cannot");
}

int main() {
    hello_offer();
    join_accept_and_siblings();
    deny_and_maxpeers();
    leave_and_bye();
    timeout();
    p2p_pair();
    malformed_and_unknown();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("matching: all passed\n");
    return 0;
}
