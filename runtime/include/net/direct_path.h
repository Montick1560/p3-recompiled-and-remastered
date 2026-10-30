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
    // Route choice: both routes get a challenge every keepalive_ms while Up; score = p50 + p95 of the last
    // route_window samples, an unanswered challenge counts lost_ms. The relay wins by route_margin_ms or more.
    int route_window = 20, route_min_samples = 5, route_margin_ms = 40, route_hold_ms = 10000, lost_ms = 1000;
    // Back to direct only when it is not slower (a band of route_margin_ms). A return to direct undone within
    // route_flap_ms doubles the next hold on the relay, up to route_max_hold_ms (direct slow only under our load).
    int route_flap_ms = 60000, route_max_hold_ms = 300000;
    int relay_silent_probes = 5;  // a u7 peer never answers kind 4: stop relay challenges after these many
    int max_relay_answers_per_s = 10;
};
enum class PathState { Probing, Up, Down, Closed };
struct PathStats {
    int rtt_p50 = -1, rtt_p95 = -1;
    uint64_t sent = 0, received = 0, dropped_auth = 0, dropped_replay = 0, dropped_rate = 0;
    uint64_t keepalives = 0, keepalive_acks = 0;  // the direct path's loss estimate
    int64_t up_ms = 0;
    int fallbacks = 0;
    int relay_p50 = -1, relay_p95 = -1;
    int64_t relay_ms = 0;  // Up, but the relay was faster and carried the datagrams
};

class Path {
public:
    Path(uint32_t pair_id, const Keys& keys, std::vector<Cand> cands, int64_t now_ms, Rng rng, PathConfig cfg = {});
    ~Path();  // wipes keys
    Path(const Path&) = delete;
    Path& operator=(const Path&) = delete;

    // *relay receives sealed relay challenges to send as kind 4; null: none (the relay route is not measured).
    void tick(int64_t now_ms, std::vector<Out>* out, std::vector<std::vector<uint8_t>>* relay = nullptr);
    // A packet whose header carries this pair id. true when *app holds an accepted Dgram/Seg/Ack.
    bool on_packet(const Addr& from, const uint8_t* pkt, size_t len, int64_t now_ms, std::vector<Out>* out,
                   Opened* app);
    // false: not use_direct() / too big
    bool send(Type type, const uint8_t* body, size_t len, int64_t now_ms, std::vector<Out>* out);
    // Seals for the relay (kind 4) with the same counter space; empty when the counter is exhausted or Closed.
    std::vector<uint8_t> seal_for_relay(Type type, const uint8_t* body, size_t len);
    // A packet that came through the relay (kind 4): same checks, no address learning. Relay challenges are
    // answered into *relay; their answers are relay samples. Neither changes the path or the state.
    bool on_relayed(const uint8_t* pkt, size_t len, int64_t now_ms, Opened* app,
                    std::vector<std::vector<uint8_t>>* relay = nullptr);
    void close(int64_t now_ms, std::vector<Out>* out);  // CLOSE to the path if Up, wipe, Closed
    PathState state() const { return state_; }
    uint32_t pair_id() const { return pair_id_; }
    CandType path_type() const { return path_type_; }
    PathStats stats() const;
    bool use_direct() const { return state_ == PathState::Up && !relay_preferred_; }
    bool relay_preferred() const { return relay_preferred_; }
    int direct_score() const { return score(direct_win_); }  // -1 below route_min_samples
    int relay_score() const { return score(relay_win_); }

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
        bool relay;  // sent as kind 4, to = Addr{}
    };

    std::vector<uint8_t> seal_next(Type type, const uint8_t* body, size_t len);
    bool accept(const uint8_t* pkt, size_t len, Opened* o);  // header, replay window, AEAD
    void emit(const Addr& to, Type type, const uint8_t* body, size_t len, int64_t now_ms, std::vector<Out>* out);
    void send_challenge(const Addr& to, Type type, int64_t now_ms, std::vector<Out>* out,
                        std::vector<std::vector<uint8_t>>* relay = nullptr);
    void answer(const Addr& from, const Opened& o, int64_t now_ms, std::vector<Out>* out);
    void on_ack(const Addr& from, const Opened& o, int64_t now_ms);
    void learn(const Addr& from);
    void start_attempt(int64_t now_ms);
    void go_down(int64_t now_ms);
    void catch_up(int64_t now_ms);  // a gap over kStallMs since the last call was our own stall: shift the timers
    void leave_up(int64_t now_ms);  // the Up time and relay time end; the relay is no longer preferred
    void sample(std::deque<int>* win, int ms);
    int score(const std::deque<int>& win) const;
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
    std::deque<int> direct_win_, relay_win_;  // route samples of this Up span
    bool relay_preferred_ = false, relay_answered_ = false;
    int relay_sent_ = 0;
    int64_t last_ka_ = 0, last_switch_ = INT64_MIN / 2, last_return_ = INT64_MIN / 2, relay_ack_window_ = INT64_MIN / 2;
    int flaps_ = 0;  // returns to direct undone within route_flap_ms, in a row
    int relay_acks_ = 0;
    PathStats stats_;
};

}  // namespace direct
