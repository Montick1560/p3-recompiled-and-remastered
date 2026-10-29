// net/direct_path.h: one peer's direct P2P path (spec "Security model", "Connectivity"). Pure: the caller owns
// the socket and the clock. A candidate becomes the path only after an authenticated ProbeAck from that address
// echoes a challenge we sent to it; unauthenticated input changes nothing and gets no answer.
#pragma once

#include "net/direct_crypto.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace direct {

struct Addr {
    uint32_t ip = 0;
    uint16_t port = 0;
    bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
};
enum class CandType : uint8_t { Host, Srflx };
struct Cand {
    Addr addr;
    CandType type = CandType::Srflx;
};
struct Out {
    Addr to;
    std::vector<uint8_t> bytes;
};
using Rng = std::function<uint64_t()>;  // secure_random in production, seeded in tests

struct PathConfig {
    int probe_interval_ms = 100, probes_per_cand = 10, attempt_ms = 5000, max_attempts = 3, retry_ms = 30000;
    int keepalive_ms = 2000, dead_ms = 4000;
    int max_pkts_per_s = 600, max_bytes_per_s = 1 << 20, flood_s = 3;
    int max_unvalidated_acks_per_s = 10;
};
enum class PathState { Probing, Up, Down, Closed };
struct PathStats {
    int rtt_p50 = -1, rtt_p95 = -1;
    uint64_t sent = 0, received = 0, dropped_auth = 0, dropped_replay = 0, dropped_rate = 0;
    uint64_t keepalives = 0, keepalive_acks = 0;  // the direct path's loss estimate
    int64_t up_ms = 0;
    int fallbacks = 0;
};

class Path {
public:
    Path(uint32_t pair_id, const Keys& keys, std::vector<Cand> cands, int64_t now_ms, Rng rng, PathConfig cfg = {});
    ~Path();  // wipes keys
    Path(const Path&) = delete;
    Path& operator=(const Path&) = delete;

    void tick(int64_t now_ms, std::vector<Out>* out);
    // A packet whose header carries this pair id. true when *app holds an accepted Dgram/Seg/Ack.
    bool on_packet(const Addr& from, const uint8_t* pkt, size_t len, int64_t now_ms, std::vector<Out>* out,
                   Opened* app);
    bool send(Type type, const uint8_t* body, size_t len, int64_t now_ms, std::vector<Out>* out);  // false: not Up / too big
    // Seals for the relay (kind 4) with the same counter space; empty when the counter is exhausted or Closed.
    std::vector<uint8_t> seal_for_relay(Type type, const uint8_t* body, size_t len);
    // A packet that came through the relay (kind 4): same checks, no address learning.
    bool on_relayed(const uint8_t* pkt, size_t len, int64_t now_ms, Opened* app);
    void close(int64_t now_ms, std::vector<Out>* out);  // CLOSE to the path if Up, wipe, Closed
    PathState state() const { return state_; }
    uint32_t pair_id() const { return pair_id_; }
    CandType path_type() const { return path_type_; }
    PathStats stats() const;

private:
    struct Target {  // an address we probe: a candidate, or one the peer's authenticated packets came from
        Addr addr;
        CandType type;
        int probes = 0;        // this attempt, or since it was learned while Up
        bool pending = false;  // Up: learned from the peer, not validated yet
    };
    struct Challenge {
        uint64_t value;
        Addr to;
        int64_t sent_ms;
        Type type;
    };

    std::vector<uint8_t> seal_next(Type type, const uint8_t* body, size_t len);
    bool accept(const uint8_t* pkt, size_t len, Opened* o);  // header, replay window, AEAD
    void emit(const Addr& to, Type type, const uint8_t* body, size_t len, int64_t now_ms, std::vector<Out>* out);
    void send_challenge(const Addr& to, Type type, int64_t now_ms, std::vector<Out>* out);
    void answer(const Addr& from, const Opened& o, int64_t now_ms, std::vector<Out>* out);
    void on_ack(const Addr& from, const Opened& o, int64_t now_ms);
    void learn(const Addr& from);
    void start_attempt(int64_t now_ms);
    void go_down(int64_t now_ms);
    bool rate_ok(size_t len, int64_t now_ms);
    void wipe();

    uint32_t pair_id_;
    Keys keys_;
    std::vector<Target> targets_;
    std::vector<Challenge> challenges_;
    Rng rng_;
    PathConfig cfg_;
    PathState state_ = PathState::Probing;
    CandType path_type_ = CandType::Srflx;
    Addr path_{};
    uint64_t tx_counter_ = 0;
    ReplayWindow window_;
    int attempts_ = 0, learned_ = 0;
    int64_t attempt_start_ = 0, next_probe_ = 0, retry_at_ = -1, last_tick_ = 0;
    int64_t last_rx_ = 0, last_tx_ = 0, up_since_ = 0;
    std::deque<std::pair<int64_t, size_t>> recent_;  // packets accepted by the limiter in the last second
    int64_t recent_bytes_ = 0, over_since_ = -1, last_over_ = 0;
    int64_t ack_window_ = INT64_MIN / 2;
    int acks_ = 0;
    std::vector<int> rtts_;
    PathStats stats_;
};

}  // namespace direct
