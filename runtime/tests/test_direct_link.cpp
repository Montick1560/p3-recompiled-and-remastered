// net/direct_link.h: the direct path's I/O shell on real loopback UDP sockets: two Links pair and exchange game
// datagrams; wrong keys never go Up; drop and stop; lab impairment; STUN answers only from the server we asked.
#include "net/direct_link.h"

#include "psp_socket_compat.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "net/stun.h"

using namespace direct;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

namespace {

constexpr uint32_t kVipA = 0x0A500001, kVipB = 0x0A500002, kLoopback = 0x7F000001;

struct Got {
    std::mutex mu;
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<uint32_t> vips;
    std::vector<std::pair<uint16_t, uint16_t>> ports;
    size_t size() {
        std::lock_guard<std::mutex> l(mu);
        return payloads.size();
    }
};

DeliverFn into(Got& g) {
    return [&g](uint32_t vip, Type type, uint16_t dst, uint16_t src, std::vector<uint8_t> payload) {
        if (type != Type::Dgram) return;
        std::lock_guard<std::mutex> l(g.mu);
        g.vips.push_back(vip);
        g.ports.push_back({dst, src});
        g.payloads.push_back(std::move(payload));
    };
}

bool wait_until(const std::function<bool()>& f, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

LinkOptions loopback_opts() {
    LinkOptions o;
    o.test_host_ips = {kLoopback};
    return o;
}

// Two started Links on loopback, paired with the given keys (same key: they can talk).
struct Pair {
    Link a, b;
    Got ga, gb;
    bool start(uint8_t key_a_seed = 1, uint8_t key_b_seed = 1) {
        if (!a.start(loopback_opts(), into(ga), nullptr) || !b.start(loopback_opts(), into(gb), nullptr))
            return false;
        uint8_t ka[32], kb[32];
        for (int i = 0; i < 32; i++) ka[i] = static_cast<uint8_t>(key_a_seed + i), kb[i] = static_cast<uint8_t>(key_b_seed + i);
        const auto ca = a.local_candidates(500), cb = b.local_candidates(500);
        if (ca.empty() || cb.empty()) return false;
        a.add_peer(kVipA, kVipB, 0xC0FFEE, ka, cb);
        b.add_peer(kVipB, kVipA, 0xC0FFEE, kb, ca);
        return true;
    }
};

int udp_socket(uint16_t* port) {
    psp_sock_startup();
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    *port = ntohs(a.sin_port);
    return fd;
}

std::vector<uint8_t> stun_answer(const uint8_t txid[12], uint32_t ip, uint16_t port) {
    std::vector<uint8_t> r = {0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42};
    r.insert(r.end(), txid, txid + 12);
    const uint16_t xp = port ^ 0x2112;
    const uint32_t xi = ip ^ stun::kMagicCookie;
    for (uint8_t v : {0x00, 0x20, 0x00, 0x08, 0x00, 0x01}) r.push_back(v);
    r.push_back(static_cast<uint8_t>(xp >> 8)), r.push_back(static_cast<uint8_t>(xp));
    for (int i = 3; i >= 0; i--) r.push_back(static_cast<uint8_t>(xi >> (8 * i)));
    return r;
}

void send_to(int fd, const std::vector<uint8_t>& b, const sockaddr_in& to) {
    ::sendto(fd, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0,
             reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

}  // namespace

int main() {
    {   // loopback_dgram_both_ways
        Pair p;
        CHECK(p.start(), "loopback: start + candidates");
        CHECK(wait_until([&] { return p.a.is_up(kVipB) && p.b.is_up(kVipA); }, 1000), "loopback: Up within 1 s");
        bool sent = true;
        for (int i = 0; i < 100; i++) {
            std::vector<uint8_t> d(1 + i, static_cast<uint8_t>(i));
            sent = sent && p.a.send_dgram(kVipB, 3658, 3659, d.data(), d.size());
            sent = sent && p.b.send_dgram(kVipA, 3659, 3658, d.data(), d.size());
        }
        CHECK(sent, "loopback: send_dgram true while Up");
        CHECK(wait_until([&] { return p.ga.size() == 100 && p.gb.size() == 100; }, 2000), "loopback: 100 each way");
        bool exact = true;
        {
            std::lock_guard<std::mutex> l(p.gb.mu);
            for (size_t i = 0; i < p.gb.payloads.size(); i++)
                exact = exact && p.gb.vips[i] == kVipA && p.gb.ports[i] == std::make_pair<uint16_t, uint16_t>(3658, 3659);
            std::vector<bool> seen(100);
            for (auto& d : p.gb.payloads)
                if (!d.empty() && d.size() == static_cast<size_t>(d[0]) + 1) seen[d[0]] = true;
            for (bool s : seen) exact = exact && s;
        }
        CHECK(exact, "loopback: exact bytes, ports and vip");
        const auto stats = p.a.stats_json();
        CHECK(stats.is_array() && stats.size() == 1 && stats[0]["path"] == "direct", "loopback: stats say direct");
        CHECK(stats.dump().find("127.0.0.1") == std::string::npos, "loopback: no address in the stats");
        const auto t0 = Clock::now();
        p.a.stop();
        p.b.stop();
        CHECK(Clock::now() - t0 < std::chrono::seconds(1), "stop_is_quick: <= 1 s for both");
    }
    {   // wrong_key_never_up
        Pair p;
        CHECK(p.start(1, 2), "wrong key: start");
        std::this_thread::sleep_for(std::chrono::seconds(6));
        CHECK(!p.a.is_up(kVipB) && !p.b.is_up(kVipA), "wrong key: never Up in 6 s");
        const uint8_t x[3] = {1, 2, 3};
        CHECK(!p.a.send_dgram(kVipB, 1, 2, x, 3), "wrong key: send_dgram false");
        CHECK(p.ga.size() == 0 && p.gb.size() == 0, "wrong key: nothing delivered");
        p.a.stop();
        p.b.stop();
    }
    {   // drop_peer_stops
        Pair p;
        CHECK(p.start(), "drop: start");
        CHECK(wait_until([&] { return p.a.is_up(kVipB) && p.b.is_up(kVipA); }, 1000), "drop: Up");
        p.a.drop_peer(kVipB);
        const uint8_t x[3] = {1, 2, 3};
        CHECK(!p.a.has_peer(kVipB) && !p.a.send_dgram(kVipB, 1, 2, x, 3), "drop: no peer, send false");
        p.b.send_dgram(kVipA, 1, 2, x, 3);  // B may not know yet (its Close may be in flight)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(p.ga.size() == 0, "drop: a packet from the old peer is not delivered");
        p.a.stop();
        p.b.stop();
    }
    {   // impair_loss_100_falls_back
        Pair p;
        CHECK(p.start(), "impair: start");
        CHECK(wait_until([&] { return p.a.is_up(kVipB); }, 1000), "impair: Up");
        p.a.set_impair("loss=100");
        CHECK(wait_until([&] { return !p.a.is_up(kVipB); }, 5000), "impair: loss 100 -> not Up within 5 s");
        p.a.stop();
        p.b.stop();
    }
    {   // stun_only_from_asked_server
        uint16_t sport = 0, oport = 0;
        const int server = udp_socket(&sport), other = udp_socket(&oport);
        Link l;
        Got g;
        LinkOptions o;
        o.stun = {"127.0.0.1:" + std::to_string(sport)};
        CHECK(l.start(o, into(g), nullptr), "stun: start");
        std::thread fake([&] {
            uint8_t buf[512];
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            const int n = ::recvfrom(server, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                     reinterpret_cast<sockaddr*>(&from), &fl);
            if (n != 20) return;
            const uint8_t* txid = buf + 8;
            uint8_t wrong[12];
            for (int i = 0; i < 12; i++) wrong[i] = static_cast<uint8_t>(txid[i] ^ 0x5A);
            send_to(other, stun_answer(txid, 0xCB00714D, 1111), from);   // right txid, wrong source
            send_to(server, stun_answer(wrong, 0xCB007158, 2222), from); // right source, wrong txid
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            send_to(server, stun_answer(txid, 0xC6336401, 4444), from);  // the real answer
        });
        const auto cands = l.local_candidates(1500);
        fake.join();
        int srflx = 0;
        bool right = false;
        for (const Cand& c : cands) {
            if (c.type != CandType::Srflx) continue;
            srflx++;
            right = c.addr.ip == 0xC6336401 && c.addr.port == 4444;
        }
        CHECK(srflx == 1 && right, "stun: only the asked server's answer with our txid sets the srflx candidate");
        l.stop();
        psp_sock_close(server);
        psp_sock_close(other);
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("direct link: all passed\n");
    return 0;
}
