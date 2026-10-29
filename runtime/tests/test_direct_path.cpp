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
};

struct Sent {
    int64_t at;
    Addr from, to;
    std::vector<uint8_t> bytes;
};

// Packets travel `delay` ms; `deliver(to)` false loses them. Every packet ever sent is kept in `log`.
struct Sim {
    int64_t now = 0;
    int delay = 20;
    std::function<bool(const Addr&)> deliver = [](const Addr&) { return true; };
    std::vector<Sent> flight, log;
    std::vector<Node*> nodes;

    void send(Node& n, std::vector<Out>& outs) {
        for (auto& o : outs) {
            log.push_back({now, n.from, o.to, o.bytes});
            if (deliver(o.to)) flight.push_back({now + delay, n.from, o.to, std::move(o.bytes)});
        }
        outs.clear();
    }
    void step(int ms) {
        now += ms;
        std::vector<Out> outs;
        for (Node* n : nodes) {
            n->path->tick(now, &outs);
            send(*n, outs);
        }
        std::vector<Sent> due;
        for (auto it = flight.begin(); it != flight.end();) {
            if (it->at <= now) {
                due.push_back(std::move(*it));
                it = flight.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& s : due) {
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
        sim.run_until(10300);
        const int ka = sim.count(b.keys, kB, Type::Keepalive, 300, 10300);
        CHECK(ka >= 4 && ka <= 6, "keepalive: about every 2 s while silent");
        CHECK(a.path->state() == PathState::Up, "keepalive: still Up");
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
