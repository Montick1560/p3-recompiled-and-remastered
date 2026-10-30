// net/direct_path.h: the per-peer direct path (probing, validation, keepalive, limits) on a simulated network
// with a fake clock.
#include "net/direct_path.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <vector>

using namespace direct;

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

namespace {

constexpr uint32_t kPair = 0x51E5A11D;
constexpr uint32_t kVipA = 0x0A500001, kVipB = 0x0A500002;
const Addr kA{0x01010101, 1000}, kB{0x02020202, 2000};

Keys keys_for(uint32_t me, uint32_t peer, uint8_t seed = 9) {
    uint8_t pk[32];
    for (int i = 0; i < 32; i++) pk[i] = static_cast<uint8_t>(seed + i);
    return derive_keys(pk, me, peer);
}

Rng seeded(uint64_t seed) {
    auto g = std::make_shared<std::mt19937_64>(seed);
    return [g] { return (*g)(); };
}

struct Node {
    std::unique_ptr<Path> path;
    Keys keys;
    Addr from;                    // the source address its packets carry
    std::vector<Addr> receives;   // addresses delivered to it
    std::vector<Opened> got;
    int64_t paused_until = 0;     // its process stalls: no tick, its packets wait for it
};

struct Sent {
    int64_t at;
    Addr from, to;
    std::vector<uint8_t> bytes;
};

// Packets travel `delay` ms; `deliver(to)` false loses them. Every packet ever sent is kept in `log`.
// The relay leg (kind 4) is off with relay_delay < 0; it carries a node's relay output to the other node.
struct Sim {
    int64_t now = 0;
    int delay = 20;
    int relay_delay = -1;
    bool relay_on = true;  // false: a u7 peer, which drops kind 4
    bool rx_first = false; // deliver before the ticks, as direct::Link::run does
    std::function<bool(const Addr&)> deliver = [](const Addr&) { return true; };
    std::vector<Sent> flight, log, relay_flight, relay_log;
    std::vector<Node*> nodes;
    std::vector<std::vector<uint8_t>> rel;

    std::vector<std::vector<uint8_t>>* relay_out() { return relay_delay >= 0 ? &rel : nullptr; }
    void relay(Node& n) {
        Node* to = nodes[0] == &n ? nodes[1] : nodes[0];
        for (auto& b : rel) {
            relay_log.push_back({now, n.from, to->from, b});
            if (relay_on) relay_flight.push_back({now + relay_delay, n.from, to->from, std::move(b)});
        }
        rel.clear();
    }
    bool paused(const Addr& to) const {
        for (Node* n : nodes)
            if (now < n->paused_until && std::find(n->receives.begin(), n->receives.end(), to) != n->receives.end())
                return true;
        return false;
    }
    std::vector<Sent> take_due(std::vector<Sent>& q) {
        std::vector<Sent> due;
        for (auto it = q.begin(); it != q.end();) {
            if (it->at <= now && !paused(it->to)) {
                due.push_back(std::move(*it));
                it = q.erase(it);
            } else {
                ++it;
            }
        }
        return due;
    }

    void send(Node& n, std::vector<Out>& outs) {
        for (auto& o : outs) {
            log.push_back({now, n.from, o.to, o.bytes});
            if (deliver(o.to)) flight.push_back({now + delay, n.from, o.to, std::move(o.bytes)});
        }
        outs.clear();
    }
    void step(int ms) {
        now += ms;
        if (!rx_first) ticks();
        rx();
        if (rx_first) ticks();
    }
    void ticks() {
        std::vector<Out> outs;
        for (Node* n : nodes) {
            if (now < n->paused_until) continue;
            n->path->tick(now, &outs, relay_out());
            send(*n, outs);
            relay(*n);
        }
    }
    void rx() {
        std::vector<Out> outs;
        for (auto& s : take_due(relay_flight)) {
            for (Node* n : nodes) {
                if (!(n->from == s.to)) continue;
                Opened app;
                if (n->path->on_relayed(s.bytes.data(), s.bytes.size(), now, &app, relay_out())) n->got.push_back(app);
                relay(*n);
            }
        }
        for (auto& s : take_due(flight)) {
            for (Node* n : nodes) {
                if (std::find(n->receives.begin(), n->receives.end(), s.to) == n->receives.end()) continue;
                Opened app;
                if (n->path->on_packet(s.from, s.bytes.data(), s.bytes.size(), now, &outs, &app))
                    n->got.push_back(app);
                send(*n, outs);
            }
        }
    }
    void run_until(int64_t t, int ms = 10) {
        while (now < t) step(ms);
    }
    // Packets of one type sent to `to` (opened with the receiver's keys), optionally in a time span.
    int count(const Keys& rx_keys, const Addr& to, Type type, int64_t t0 = 0, int64_t t1 = INT64_MAX) const {
        int n = 0;
        for (auto& s : log) {
            Opened o;
            if (s.to == to && s.at >= t0 && s.at < t1 && open(rx_keys.rx, s.bytes.data(), s.bytes.size(), &o) &&
                o.type == type)
                n++;
        }
        return n;
    }
};

void make_pair(Node& a, Node& b, std::vector<Cand> a_cands, std::vector<Cand> b_cands, PathConfig cfg = {}) {
    a.keys = keys_for(kVipA, kVipB);
    b.keys = keys_for(kVipB, kVipA);
    a.from = kA, b.from = kB;
    a.receives = {kA}, b.receives = {kB};
    a.path = std::make_unique<Path>(kPair, a.keys, std::move(a_cands), 0, seeded(1), cfg);
    b.path = std::make_unique<Path>(kPair, b.keys, std::move(b_cands), 0, seeded(2), cfg);
}

bool send_dgram(Sim& sim, Node& n, uint8_t tag) {
    std::vector<Out> outs;
    const uint8_t body[4] = {tag, 1, 2, 3};
    const bool ok = n.path->send(Type::Dgram, body, sizeof body, sim.now, &outs);
    sim.send(n, outs);
    return ok;
}

}  // namespace

int main() {
    {   // punch_both_sides
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        CHECK(a.path->state() == PathState::Up && b.path->state() == PathState::Up, "punch: both Up in 300 ms");
        CHECK(a.path->path_type() == CandType::Srflx, "punch: srflx path");
        CHECK(send_dgram(sim, a, 0xA1) && send_dgram(sim, b, 0xB2), "punch: send works when Up");
        sim.run_until(400);
        CHECK(b.got.size() == 1 && b.got[0].type == Type::Dgram && b.got[0].body[0] == 0xA1, "punch: A->B dgram");
        CHECK(a.got.size() == 1 && a.got[0].body[0] == 0xB2, "punch: B->A dgram");
        CHECK(a.path->stats().rtt_p50 >= 40 && a.path->stats().rtt_p50 <= 60, "punch: rtt ~ 2 x 20 ms");
    }
    {   // unreachable_goes_down
        Sim sim;
        sim.deliver = [](const Addr&) { return false; };
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a};
        sim.run_until(4990);
        CHECK(a.path->state() == PathState::Probing, "unreachable: still probing at 4.99 s");
        sim.run_until(5010);
        CHECK(a.path->state() == PathState::Down, "unreachable: Down at 5 s");
        CHECK(sim.count(b.keys, kB, Type::Probe, 0, 5010) <= 10, "unreachable: <= 10 probes in attempt 1");
        CHECK(sim.count(b.keys, kB, Type::Probe, 0, 5010) >= 5, "unreachable: it did probe");
        bool small = true;
        for (auto& s : sim.log) small = small && s.bytes.size() <= 96;
        CHECK(small, "unreachable: probes <= 96 B");
        sim.run_until(34900);
        CHECK(sim.count(b.keys, kB, Type::Probe, 5010, 34900) == 0, "unreachable: quiet until the retry");
        sim.run_until(35200);
        CHECK(sim.count(b.keys, kB, Type::Probe, 34900, 35200) > 0, "unreachable: retry at 35 s");
        sim.run_until(200000, 50);
        CHECK(sim.count(b.keys, kB, Type::Probe) <= 30, "unreachable: <= 3 attempts x 10 probes");
        const size_t sent = sim.log.size();
        sim.run_until(400000, 100);
        CHECK(sim.log.size() == sent, "unreachable: no probes ever after the third attempt");
        CHECK(a.path->state() == PathState::Down, "unreachable: stays Down");
        uint8_t x = 1;
        std::vector<Out> outs;
        CHECK(!a.path->send(Type::Dgram, &x, 1, sim.now, &outs) && outs.empty(), "unreachable: send false");
    }
    {   // lying_candidate_bounded: B also lists a third party that never answers
        Sim sim;
        const Addr third{0x03030303, 3000};
        sim.deliver = [&](const Addr& to) { return !(to == third); };
        Node a, b;
        make_pair(a, b, {{third, CandType::Srflx}, {kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(1000);
        for (int i = 0; i < 50; i++) send_dgram(sim, a, 1), sim.step(20);
        // The peer dies, so the path retries every 30 s up to the attempt limit.
        sim.deliver = [](const Addr&) { return false; };
        sim.run_until(300000, 50);
        int to_third = 0, dgram_to_third = 0;
        for (auto& s : sim.log) {
            if (!(s.to == third)) continue;
            to_third++;
            Opened o;
            if (open(b.keys.rx, s.bytes.data(), s.bytes.size(), &o) && o.type == Type::Dgram) dgram_to_third++;
        }
        CHECK(to_third <= 30, "lying: <= 30 packets to the third party over the pairing");
        CHECK(dgram_to_third == 0, "lying: never a Dgram to it");
    }
    {   // garbage_ignored
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        std::vector<Out> outs;
        Opened app;
        std::mt19937_64 rng(5);
        std::vector<uint8_t> junk(60);
        for (auto& x : junk) x = static_cast<uint8_t>(rng());
        CHECK(!a.path->on_packet(kB, junk.data(), junk.size(), sim.now, &outs, &app) && outs.empty(), "random bytes");
        Keys other = keys_for(kVipB, kVipA, 77);
        const uint8_t body[4] = {9, 9, 9, 9};
        auto forged = seal(other.tx, kPair, 1000, Type::Dgram, body, 4);
        CHECK(!a.path->on_packet(kB, forged.data(), forged.size(), sim.now, &outs, &app) && outs.empty(),
              "other key");
        auto forged_probe = seal(other.tx, kPair, 1001, Type::Probe, body, 4);
        CHECK(!a.path->on_packet({0x09090909, 9}, forged_probe.data(), forged_probe.size(), sim.now, &outs, &app) &&
                  outs.empty(),
              "a forged probe gets no answer");
        auto real = seal(b.keys.tx, kPair, 5000, Type::Dgram, body, 4);
        CHECK(a.path->on_packet(kB, real.data(), real.size(), sim.now, &outs, &app), "valid packet");
        CHECK(!a.path->on_packet(kB, real.data(), real.size(), sim.now, &outs, &app), "replay dropped");
        const PathStats st = a.path->stats();
        CHECK(st.dropped_auth >= 3 && st.dropped_replay == 1, "garbage counted");
    }
    {   // keepalive_and_dead
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        while (sim.now < 10300) send_dgram(sim, a, 1), sim.run_until(sim.now + 100);
        const int ka = sim.count(b.keys, kB, Type::Keepalive, 300, 10300);
        CHECK(ka >= 4 && ka <= 6, "keepalive: about every 2 s, also with traffic (it is the direct sample)");
        CHECK(a.path->state() == PathState::Up, "keepalive: still Up");
        const PathStats ks = a.path->stats();
        CHECK(ks.keepalives >= 4 && ks.keepalive_acks + 1 >= ks.keepalives, "keepalive: counted, and answered");
        sim.nodes = {&a};  // B goes silent
        sim.run_until(12200);
        CHECK(a.path->state() == PathState::Up, "dead: Up within 4 s of silence");
        sim.run_until(14500);
        CHECK(a.path->state() == PathState::Down, "dead: Down after 4 s of silence");
        CHECK(a.path->stats().fallbacks == 1, "dead: fallback counted");
        CHECK(a.path->stats().up_ms >= 12000, "dead: up time counted");
        sim.nodes = {&a, &b};
        sim.run_until(46000);
        CHECK(a.path->state() == PathState::Up, "dead: the retry 30 s later finds the path again");
    }
    {   // rebinding_revalidates
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        const Addr rebound{kB.ip, 2001};
        b.from = rebound;
        b.receives = {kB, rebound};
        int64_t t_ack = -1;
        Sent first_dgram_new{};
        for (int i = 0; i < 40; i++) {
            send_dgram(sim, a, 7);
            sim.step(100);  // B, silent, sends keepalives from its new port: A learns it
            for (auto& s : sim.log) {
                Opened o;
                if (t_ack < 0 && s.from == rebound && s.to == kA && open(a.keys.rx, s.bytes.data(), s.bytes.size(), &o) &&
                    o.type == Type::ProbeAck)
                    t_ack = s.at + sim.delay;  // when A has it
            }
        }
        CHECK(t_ack > 0, "rebinding: B answered a probe at the new address");
        bool early = false, late = false;
        for (auto& s : sim.log) {
            Opened o;
            if (!open(b.keys.rx, s.bytes.data(), s.bytes.size(), &o) || o.type != Type::Dgram || !(s.from == kA)) continue;
            if (s.to == rebound && s.at < t_ack) early = true;
            if (s.to == rebound && s.at >= t_ack) late = true;
        }
        CHECK(!early, "rebinding: no Dgram to the new address before its ProbeAck");
        CHECK(late, "rebinding: Dgrams go to the new address after it");
        CHECK(a.path->state() == PathState::Up, "rebinding: still Up");
    }
    {   // rate_limit_floods_to_closed
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        Sim sim;
        sim.nodes = {&a, &b};
        sim.run_until(300);
        std::vector<Out> outs;
        Opened app;
        uint64_t ctr = 1u << 20;
        int accepted_in_first_second = 0;
        const uint8_t body[8] = {};
        for (int ms = 0; ms < 4000 && a.path->state() != PathState::Closed; ms += 10) {
            sim.now += 10;
            a.path->tick(sim.now, &outs);
            for (int i = 0; i < 7; i++) {
                auto p = seal(b.keys.tx, kPair, ctr++, Type::Dgram, body, sizeof body);
                if (a.path->on_packet(kB, p.data(), p.size(), sim.now, &outs, &app) && ms < 1000)
                    accepted_in_first_second++;
            }
            outs.clear();
        }
        CHECK(accepted_in_first_second <= 600, "flood: at most 600 accepted per second");
        CHECK(a.path->stats().dropped_rate > 0, "flood: drops counted");
        CHECK(a.path->state() == PathState::Closed, "flood: Closed after 3 s over the limit");
    }
    {   // stall_then_resume
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        for (int i = 0; i < 20; i++) send_dgram(sim, a, 1);
        sim.run_until(1000);
        const size_t before = sim.log.size();
        sim.now += 30000;
        std::vector<Out> outs;
        a.path->tick(sim.now, &outs);
        int ka = 0, probes = 0;
        for (auto& o : outs) {
            Opened op;
            if (!open(b.keys.rx, o.bytes.data(), o.bytes.size(), &op)) continue;
            ka += op.type == Type::Keepalive;
            probes += op.type == Type::Probe;
        }
        CHECK(outs.size() <= 1 + 4, "stall: one tick sends little");
        CHECK(ka <= 1 && probes <= 4, "stall: at most one keepalive and one probe round");
        CHECK(a.path->state() == PathState::Up, "stall: our own stall is not the peer's death");
        (void)before;
    }
    {   // relay_shares_counters
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        std::vector<std::vector<uint8_t>> relayed;
        for (int i = 0; i < 10; i++) {
            send_dgram(sim, a, static_cast<uint8_t>(i));
            const uint8_t body[2] = {0xEE, static_cast<uint8_t>(i)};
            relayed.push_back(a.path->seal_for_relay(Type::Seg, body, 2));
        }
        sim.run_until(400);
        int via_relay = 0;
        Opened app;
        for (auto& r : relayed) via_relay += b.path->on_relayed(r.data(), r.size(), sim.now, &app) && app.type == Type::Seg;
        CHECK(b.got.size() == 10 && via_relay == 10, "relay: interleaved direct and relayed packets all open");
        CHECK(!b.path->on_relayed(relayed[3].data(), relayed[3].size(), sim.now, &app), "relay: replay dropped");
        std::vector<Out> outs;
        CHECK(!b.path->on_packet(kA, relayed[4].data(), relayed[4].size(), sim.now, &outs, &app),
              "relay: a relayed packet replayed on the direct path is dropped");
    }
    {   // faster_relay_wins
        Sim sim;
        sim.delay = 150, sim.relay_delay = 20;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(15000);
        CHECK(a.path->state() == PathState::Up, "faster relay: direct still Up");
        CHECK(a.path->relay_preferred() && !a.path->use_direct(), "faster relay: relay preferred in 15 s");
        CHECK(!send_dgram(sim, a, 1), "faster relay: send false, so the datagram goes by relay");
        CHECK(a.path->stats().relay_p50 >= 40 && a.path->stats().relay_p50 <= 60, "faster relay: relay rtt ~ 2 x 20");
        sim.delay = 10;
        while (sim.now < 55000 && !a.path->use_direct()) sim.step(10);
        CHECK(a.path->use_direct(), "faster relay: back to direct within 40 s once it is faster");
        CHECK(a.path->stats().relay_ms >= 10000, "faster relay: relay time counted");
    }
    {   // close_routes_stay_direct
        Sim sim;
        sim.delay = 30, sim.relay_delay = 40;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(500);
        bool always = true;
        while (sim.now < 60000) sim.step(10), always = always && a.path->use_direct();
        CHECK(always, "close routes: direct at every step");
        CHECK(a.path->relay_score() > 0 && a.path->direct_score() > 0, "close routes: both measured");
    }
    {   // hold_between_switches
        Sim sim;
        sim.relay_delay = 100;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        std::vector<int64_t> changes;
        bool pref = false;
        std::mt19937 rng(7);
        while (sim.now < 120000) {
            // Each 2 s challenge sees 20 or 300 ms at random: the direct p50 keeps crossing the relay's score.
            if (sim.now % 2000 == 1000) sim.delay = rng() % 2 ? 10 : 150;
            sim.step(10);
            if (a.path->relay_preferred() != pref) pref = !pref, changes.push_back(sim.now);
        }
        bool apart = true;
        for (size_t i = 1; i < changes.size(); i++) apart = apart && changes[i] - changes[i - 1] >= 10000;
        CHECK(apart, "hold: switches at least 10 s apart");
        CHECK(changes.size() >= 2, "hold: the scores did cross, so the hold was exercised");
    }
    {   // lossy_direct_goes_relay
        Sim sim;
        sim.delay = 10, sim.relay_delay = 60;
        std::mt19937 rng(3);
        sim.deliver = [&](const Addr& to) { return !(to == kB) || rng() % 10 >= 4; };
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(30000);
        CHECK(a.path->state() == PathState::Up, "lossy: direct Up");
        CHECK(a.path->relay_preferred(), "lossy: lost challenges count, the relay wins");
    }
    {   // old_peer_never_answers
        Sim sim;
        sim.relay_delay = 20, sim.relay_on = false;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(180000);
        CHECK(a.path->use_direct(), "old peer: stays direct");
        int from_a = 0;
        for (auto& s : sim.relay_log) from_a += s.from == kA;
        CHECK(from_a == 7, "old peer: 5 relay challenges, then one retry a minute (about 70 s and 130 s)");
        CHECK(a.path->stats().relay_p50 == -1, "old peer: no relay numbers");
    }
    {   // stall_is_not_loss
        Sim sim;
        sim.delay = 15, sim.relay_delay = 20;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(20000);
        const uint64_t k0 = a.path->stats().keepalives;
        while (a.path->stats().keepalives == k0) sim.step(10);  // both challenges of A are in flight now
        sim.step(3000);
        sim.run_until(sim.now + 5000);
        CHECK(!a.path->relay_preferred(), "stall: no switch");
        CHECK(a.path->stats().relay_p95 < 1000, "stall: no fake relay loss");
        CHECK(a.path->direct_score() < 1000 && a.path->relay_score() < 1000, "stall: no fake loss in the scores");
    }
    {   // stall_ack_before_tick: the answer to an in-flight challenge is read before the first tick after a stall
        Sim sim;
        sim.delay = 15, sim.relay_delay = 20, sim.rx_first = true;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(20000);
        const uint64_t k0 = a.path->stats().keepalives;
        while (a.path->stats().keepalives == k0) sim.step(10);
        a.paused_until = sim.now + 3000;  // only A stalls: B's answers wait in A's socket
        sim.run_until(sim.now + 8000);
        CHECK(a.path->stats().rtt_p95 < 1000, "stall, rx first: no 3 s direct sample");
        CHECK(a.path->stats().relay_p95 < 1000, "stall, rx first: no 3 s relay sample");
    }
    {   // one_lost_challenge_no_switch
        Sim sim;
        sim.delay = 15, sim.relay_delay = 40;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(30000);
        const uint64_t k0 = a.path->stats().keepalives;
        bool armed = true;
        int dropped = 0;
        sim.deliver = [&](const Addr& to) {  // the first A->B packet after A's next keepalive is that keepalive
            if (!armed || !(to == kB) || a.path->stats().keepalives == k0) return true;
            armed = false, dropped++;
            return false;
        };
        bool always = true;
        while (sim.now < 70000) sim.step(10), always = always && a.path->use_direct();
        CHECK(dropped == 1, "one lost: exactly one keepalive dropped");
        CHECK(always, "one lost: one lost challenge alone never switches");
    }
    {   // relay_answers_capped
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        std::vector<std::vector<uint8_t>> rel;
        Opened app;
        const uint8_t body[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        for (int i = 0; i < 50; i++) {
            auto p = seal(b.keys.tx, kPair, (1u << 20) + i, Type::Keepalive, body, sizeof body);
            CHECK(!a.path->on_relayed(p.data(), p.size(), sim.now + i * 20, &app, &rel), "capped: not app data");
        }
        int acks = 0;
        for (auto& r : rel) acks += open(b.keys.rx, r.data(), r.size(), &app) && app.type == Type::ProbeAck;
        CHECK(acks == 10 && rel.size() == 10, "capped: 10 relay answers per second");
    }
    {   // band_keeps_relay: direct a little slower than the relay does not take the datagrams back
        Sim sim;  // 5 ms steps: RTTs in 10 ms units, so a score can sit inside the 40 ms band
        sim.delay = 150, sim.relay_delay = 40;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        while (sim.now < 15000 && !a.path->relay_preferred()) sim.step(5);
        CHECK(a.path->relay_preferred(), "band: relay preferred within 15 s");
        sim.delay = 45;  // direct score 180, relay 160
        bool kept = true;
        for (const int64_t t = sim.now + 100000; sim.now < t;) sim.step(5), kept = kept && a.path->relay_preferred();
        CHECK(kept, "band: a direct path 20 ms slower never takes the datagrams back");
        sim.delay = 30;  // direct score 120
        for (const int64_t t = sim.now + 50000; sim.now < t && !a.path->use_direct();) sim.step(5);
        CHECK(a.path->use_direct(), "band: direct is back once it is not slower");
    }
    {   // load_flaps_back_off: direct is slow only while it carries A's datagrams (bufferbloat)
        auto to_relay = [](const PathConfig& cfg) {
            Sim sim;
            sim.relay_delay = 40;
            Node a, b;
            make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}}, cfg);
            sim.nodes = {&a, &b};
            std::vector<int64_t> at;
            int64_t last_direct = INT64_MIN / 2;
            bool pref = false;
            while (sim.now < 600000) {
                sim.delay = sim.now - last_direct < 2000 ? 150 : 10;
                if (sim.now % 50 == 0 && send_dgram(sim, a, 1)) last_direct = sim.now;
                sim.step(10);
                if (a.path->relay_preferred() != pref && (pref = !pref)) at.push_back(sim.now);
            }
            return at;
        };
        PathConfig no_backoff;
        no_backoff.route_flap_ms = 0;
        const std::vector<int64_t> n = to_relay(PathConfig{}), n0 = to_relay(no_backoff);
        CHECK(n.size() >= 2 && n.size() * 2 <= n0.size(), "flaps: repeated flips back off");
        bool apart = true;
        for (size_t i = 1; i < n.size(); i++) apart = apart && n[i] - n[i - 1] >= PathConfig{}.route_hold_ms;
        CHECK(apart, "flaps: switches to the relay at least route_hold_ms apart");
    }
    {   // relay_stats_match_score: the reported relay numbers are the ones the route choice used
        Sim sim;
        sim.delay = 15, sim.relay_delay = 20;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        auto challenge = [&](const Sent& s) {  // A's relay challenges, not its answers to B's
            Opened o;
            return s.from == kA && open(b.keys.rx, s.bytes.data(), s.bytes.size(), &o) && o.type == Type::Keepalive;
        };
        auto challenges = [&] { return std::count_if(sim.relay_log.begin(), sim.relay_log.end(), challenge); };
        while (challenges() < 3) sim.step(10);
        sim.relay_flight.erase(std::remove_if(sim.relay_flight.begin(), sim.relay_flight.end(), challenge),
                               sim.relay_flight.end());  // A's 3rd relay challenge is lost
        while (challenges() < 6) sim.step(10);
        sim.run_until(sim.now + 100);
        const PathStats st = a.path->stats();  // 6 samples: 5 at 40 ms, 1 lost
        CHECK(st.relay_p95 == 40, "relay stats: p95 rounded down like the score, not the lost 1000");
        CHECK(a.path->relay_score() == st.relay_p50 + st.relay_p95, "relay stats: score = reported p50 + p95");
    }
    {   // relay_probing_retries: a relay that was silent at first is measured once it answers
        Sim sim;
        sim.relay_delay = 20, sim.relay_on = false;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(15000);
        sim.relay_on = true;
        while (sim.now < 15000 + PathConfig{}.relay_retry_ms + 5000 && a.path->stats().relay_p50 < 0) sim.step(10);
        CHECK(a.path->stats().relay_p50 >= 0, "relay retry: measured again within relay_retry_ms");
        const size_t before = sim.relay_log.size();
        sim.run_until(sim.now + 10000);
        int later = 0;
        for (size_t i = before; i < sim.relay_log.size(); i++) later += sim.relay_log[i].from == kA;
        CHECK(later >= 4, "relay retry: then a relay challenge every 2 s again");
    }
    {   // relay_answers_capped_sliding: no 20 answers across a second boundary
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        std::vector<std::vector<uint8_t>> rel;
        Opened app;
        const uint8_t body[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint64_t ctr = 1u << 21;
        size_t late = 0;
        auto challenge = [&](int64_t at) {
            const size_t n = rel.size();
            auto p = seal(b.keys.tx, kPair, ctr++, Type::Keepalive, body, sizeof body);
            a.path->on_relayed(p.data(), p.size(), at, &app, &rel);
            if (at >= sim.now + 990) late += rel.size() - n;
        };
        challenge(sim.now);
        for (int i = 0; i < 9; i++) challenge(sim.now + 990 + i);
        for (int i = 0; i < 10; i++) challenge(sim.now + 1000 + i);
        CHECK(late <= 10 && late >= 9, "capped: at most 10 answers in any second");
    }
    {   // close_wipes
        Sim sim;
        Node a, b;
        make_pair(a, b, {{kB, CandType::Srflx}}, {{kA, CandType::Srflx}});
        sim.nodes = {&a, &b};
        sim.run_until(300);
        std::vector<Out> outs;
        a.path->close(sim.now, &outs);
        CHECK(outs.size() == 1, "close: one Close packet");
        sim.send(a, outs);
        sim.run_until(400);
        CHECK(a.path->state() == PathState::Closed && b.path->state() == PathState::Closed, "close: both Closed");
        CHECK(!send_dgram(sim, a, 1), "close: send false");
        auto p = seal(b.keys.tx, kPair, 1u << 30, Type::Dgram, nullptr, 0);
        Opened app;
        CHECK(!a.path->on_packet(kB, p.data(), p.size(), sim.now, &outs, &app), "close: valid packet refused");
        CHECK(a.path->seal_for_relay(Type::Seg, nullptr, 0).empty(), "close: no relay seals");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("direct path: all passed\n");
    return 0;
}
