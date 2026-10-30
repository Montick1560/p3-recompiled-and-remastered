// net/direct_link.h: the direct P2P path's I/O shell: one UDP socket and one thread for every peer of this
// client. STUN gathering (answers only from the servers asked, with our transaction id), one direct::Path per
// paired peer, and game datagrams in and out. Never logs an address: only the candidate type, the vip and the RTT.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "net/direct_path.h"

namespace direct {

struct LinkOptions {
    std::vector<std::string> stun;        // "host:port" from the login answer
    std::vector<uint32_t> test_host_ips;  // tests only: host candidates to add (e.g. 127.0.0.1)
    std::string impair;                   // "delay=MS,jitter=MS,loss=PCT,reorder=PCT"; the caller drops it in beta builds
};

// A game datagram (any order) or, from the stream channel on, a stream segment already in order.
using DeliverFn =
    std::function<void(uint32_t peer_vip, Type type, uint16_t dst, uint16_t src, std::vector<uint8_t> payload)>;
using RelayFn = std::function<void(uint32_t peer_vip, std::vector<uint8_t> sealed)>;  // to send as kind 4

// Lab link impairment of one side: loss drops packets both ways; delay, jitter and reorder hold outgoing ones.
struct Impair {
    int delay = 0, jitter = 0, loss = 0, reorder = 0;
};
bool parse_impair(const std::string& text, Impair* out);  // false: malformed (nothing changes)

class Link {
public:
    Link() = default;
    ~Link() { stop(); }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    bool start(const LinkOptions& opt, DeliverFn deliver, RelayFn relay);  // false: no socket or no randomness
    void stop();  // Close to every peer, wipe, join; <= 1 s
    bool running() const { return running_; }
    // srflx from STUN (+ test_host_ips, + this PC's LAN addresses as host candidates), at most 4, after gathering
    // for up to wait_ms.
    std::vector<Cand> local_candidates(int wait_ms);
    void add_peer(uint32_t my_vip, uint32_t peer_vip, uint32_t pair_id, const uint8_t key[32], std::vector<Cand> cands);
    void drop_peer(uint32_t peer_vip);
    bool has_peer(uint32_t peer_vip) const;
    bool is_up(uint32_t peer_vip) const;
    bool send_dgram(uint32_t peer_vip, uint16_t dst, uint16_t src, const uint8_t* data, size_t len);  // false -> relay
    void on_relayed(uint32_t peer_vip, const uint8_t* bytes, size_t len);  // a kind 4 payload
    void set_impair(const std::string& text);  // lab: changes the impairment of a running link
    nlohmann::json stats_json() const;  // direct_stats "pairs" array, no addresses

private:
    struct Peer {
        std::unique_ptr<Path> path;
        PathState logged = PathState::Probing;
        bool logged_relay = false;  // the route choice last logged
    };
    struct Delivery {
        uint32_t vip;
        Opened o;
    };
    struct Held {  // an outgoing packet the lab impairment delays
        int64_t due;
        Out out;
    };
    struct StunAsk {
        Addr server;
        uint8_t txid[12];
        bool answered = false;
    };
    void run();
    void transmit(std::vector<Out>& outs, int64_t now);  // under mu_
    void sendto_now(const Out& o);
    void receive(int64_t now, std::vector<Delivery>* got);  // under mu_
    void log_state(uint32_t vip, Peer& p);
    void deliver_all(std::vector<Delivery>& got);
    using Relayed = std::vector<std::pair<uint32_t, std::vector<uint8_t>>>;  // (peer vip, sealed) for relay_
    void relay_all(Relayed& rel);
    nlohmann::json entry(const PathStats& s) const;

    int fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<std::thread::id> thread_id_{};  // the running thread's id, cleared once joined
    std::mutex join_mu_;         // two stop() calls never join at once
    std::atomic<bool> running_{false};
    LinkOptions opt_;
    DeliverFn deliver_;
    RelayFn relay_;
    Rng rng_;
    mutable std::mutex mu_;
    std::condition_variable stun_cv_;
    std::map<uint32_t, Peer> peers_;  // by peer vip
    std::vector<StunAsk> stun_asks_;
    std::vector<Addr> srflx_;
    Impair impair_;
    std::deque<Held> held_;
    std::vector<nlohmann::json> finished_;  // stats of peers dropped during this connection
    uint64_t dropped_unknown_ = 0;
};

}  // namespace direct
