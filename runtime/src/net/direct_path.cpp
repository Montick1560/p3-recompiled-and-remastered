#include "net/direct_path.h"

#include <algorithm>

namespace direct {

namespace {

constexpr int64_t kStallMs = 1000;    // a tick this late means our own process stalled, not the peer
constexpr size_t kMaxChallenges = 64;
constexpr int kMaxLearned = 8;        // addresses learned from the peer's packets, per pairing
constexpr size_t kRttSamples = 256;

uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

}  // namespace

Path::Path(uint32_t pair_id, const Keys& keys, std::vector<Cand> cands, int64_t now_ms, Rng rng, PathConfig cfg)
    : pair_id_(pair_id), keys_(keys), rng_(std::move(rng)), cfg_(cfg), last_tick_(now_ms) {
    for (const Cand& c : cands) targets_.push_back({c.addr, c.type});
    start_attempt(now_ms);
}

Path::~Path() { wipe(); }

void Path::wipe() {
    keys_.wipe();
    challenges_.clear();
}

void Path::start_attempt(int64_t now_ms) {
    attempts_++;
    state_ = PathState::Probing;
    attempt_start_ = next_probe_ = now_ms;
    retry_at_ = -1;
    for (Target& t : targets_) t.probes = 0, t.pending = false;
}

void Path::go_down(int64_t now_ms) {
    if (state_ == PathState::Up) stats_.up_ms += now_ms - up_since_;
    state_ = PathState::Down;
    retry_at_ = attempts_ < cfg_.max_attempts ? now_ms + cfg_.retry_ms : -1;
}

std::vector<uint8_t> Path::seal_next(Type type, const uint8_t* body, size_t len) {
    if (tx_counter_ + 1 >= kCounterLimit) return {};
    return seal(keys_.tx, pair_id_, ++tx_counter_, type, body, len);
}

void Path::emit(const Addr& to, Type type, const uint8_t* body, size_t len, int64_t now_ms, std::vector<Out>* out) {
    std::vector<uint8_t> p = seal_next(type, body, len);
    if (p.empty()) return;
    out->push_back({to, std::move(p)});
    last_tx_ = now_ms;
    stats_.sent++;
}

void Path::send_challenge(const Addr& to, Type type, int64_t now_ms, std::vector<Out>* out) {
    const uint64_t v = rng_();
    uint8_t body[8];
    for (int i = 0; i < 8; i++) body[i] = static_cast<uint8_t>(v >> (8 * i));
    emit(to, type, body, sizeof body, now_ms, out);
    challenges_.push_back({v, to, now_ms, type});
    if (type == Type::Keepalive) stats_.keepalives++;
    if (challenges_.size() > kMaxChallenges) challenges_.erase(challenges_.begin());
}

void Path::tick(int64_t now_ms, std::vector<Out>* out) {
    if (state_ == PathState::Closed) return;
    const int64_t gap = now_ms - last_tick_;
    if (gap > kStallMs) {  // resume as if no time passed: no burst, and the peer is not declared dead for our stall
        for (int64_t* t : {&attempt_start_, &next_probe_, &last_rx_, &last_tx_, &up_since_}) *t += gap;
        if (retry_at_ >= 0) retry_at_ += gap;
        for (Challenge& c : challenges_) c.sent_ms += gap;
    }
    last_tick_ = now_ms;
    if (state_ == PathState::Down && retry_at_ >= 0 && now_ms >= retry_at_) start_attempt(now_ms);
    if (state_ == PathState::Probing && now_ms - attempt_start_ >= cfg_.attempt_ms) go_down(now_ms);
    if (state_ == PathState::Up && now_ms - last_rx_ >= cfg_.dead_ms) {
        stats_.fallbacks++;
        go_down(now_ms);
    }
    if (state_ == PathState::Up && now_ms - last_tx_ >= cfg_.keepalive_ms)
        send_challenge(path_, Type::Keepalive, now_ms, out);
    if ((state_ == PathState::Probing || state_ == PathState::Up) && now_ms >= next_probe_) {
        // Probing: every target. Up: only addresses the peer's packets came from, until validated.
        for (Target& t : targets_) {
            if (state_ == PathState::Up && !t.pending) continue;
            if (t.probes >= cfg_.probes_per_cand) {
                t.pending = false;
                continue;
            }
            t.probes++;
            send_challenge(t.addr, Type::Probe, now_ms, out);
        }
        next_probe_ = now_ms + cfg_.probe_interval_ms;
    }
}

bool Path::rate_ok(size_t len, int64_t now_ms) {
    while (!recent_.empty() && now_ms - recent_.front().first >= 1000) {
        recent_bytes_ -= static_cast<int64_t>(recent_.front().second);
        recent_.pop_front();
    }
    if (static_cast<int>(recent_.size()) < cfg_.max_pkts_per_s &&
        recent_bytes_ + static_cast<int64_t>(len) <= cfg_.max_bytes_per_s) {
        recent_.push_back({now_ms, len});
        recent_bytes_ += static_cast<int64_t>(len);
        return true;
    }
    if (over_since_ < 0 || now_ms - last_over_ > 1000) over_since_ = now_ms;  // a quiet second resets the count
    last_over_ = now_ms;
    if (now_ms - over_since_ >= int64_t{cfg_.flood_s} * 1000) {  // flooded for flood_s: back to the relay for good
        if (state_ == PathState::Up) stats_.up_ms += now_ms - up_since_;
        wipe();
        state_ = PathState::Closed;
    }
    return false;
}

bool Path::accept(const uint8_t* pkt, size_t len, Opened* o) {
    uint32_t pid;
    uint64_t counter;
    if (!read_header(pkt, len, &pid, &counter) || pid != pair_id_) {
        stats_.dropped_auth++;
        return false;
    }
    if (!window_.check(counter)) {
        stats_.dropped_replay++;
        return false;
    }
    if (!open(keys_.rx, pkt, len, o)) {
        stats_.dropped_auth++;
        return false;
    }
    window_.accept(counter);
    stats_.received++;
    return true;
}

void Path::learn(const Addr& from) {
    if (state_ == PathState::Up && from == path_) return;
    auto it = std::find_if(targets_.begin(), targets_.end(), [&](const Target& t) { return t.addr == from; });
    const bool known = it != targets_.end();
    if (known && (state_ != PathState::Up || it->pending)) return;
    if (learned_ >= kMaxLearned) return;
    learned_++;
    if (!known) {
        targets_.push_back({from, CandType::Srflx});
        it = targets_.end() - 1;
    }
    if (state_ == PathState::Up) it->pending = true, it->probes = 0;
}

void Path::answer(const Addr& from, const Opened& o, int64_t now_ms, std::vector<Out>* out) {
    if (o.body.size() != 8) return;
    if (!(state_ == PathState::Up && from == path_)) {
        if (now_ms - ack_window_ >= 1000) ack_window_ = now_ms, acks_ = 0;
        if (acks_ >= cfg_.max_unvalidated_acks_per_s) return;
        acks_++;
    }
    emit(from, Type::ProbeAck, o.body.data(), 8, now_ms, out);
}

void Path::on_ack(const Addr& from, const Opened& o, int64_t now_ms) {
    if (o.body.size() != 8) return;
    const uint64_t v = le64(o.body.data());
    auto c = std::find_if(challenges_.begin(), challenges_.end(),
                          [&](const Challenge& ch) { return ch.value == v && ch.to == from; });
    if (c == challenges_.end()) return;
    rtts_.push_back(static_cast<int>(now_ms - c->sent_ms));
    if (c->type == Type::Keepalive) stats_.keepalive_acks++;
    if (rtts_.size() > kRttSamples) rtts_.erase(rtts_.begin());
    challenges_.erase(c);
    if (state_ == PathState::Up && from == path_) return;
    auto t = std::find_if(targets_.begin(), targets_.end(), [&](const Target& x) { return x.addr == from; });
    path_type_ = t != targets_.end() ? t->type : CandType::Srflx;
    path_ = from;  // validated: it holds the pair key and answered at this address
    if (state_ != PathState::Up) {
        state_ = PathState::Up;
        up_since_ = now_ms;
        last_rx_ = now_ms;
    }
    for (Target& x : targets_) x.pending = false;
}

bool Path::on_packet(const Addr& from, const uint8_t* pkt, size_t len, int64_t now_ms, std::vector<Out>* out,
                     Opened* app) {
    if (state_ == PathState::Closed) return false;
    if (!rate_ok(len, now_ms)) {
        stats_.dropped_rate++;
        return false;
    }
    Opened o;
    if (!accept(pkt, len, &o)) return false;
    last_rx_ = now_ms;
    learn(from);
    switch (o.type) {
    case Type::Probe:
    case Type::Keepalive:
        answer(from, o, now_ms, out);
        return false;
    case Type::ProbeAck:
        on_ack(from, o, now_ms);
        return false;
    case Type::Close:
        if (state_ == PathState::Up) stats_.up_ms += now_ms - up_since_;
        wipe();
        state_ = PathState::Closed;
        return false;
    default:
        *app = std::move(o);
        return true;
    }
}

bool Path::on_relayed(const uint8_t* pkt, size_t len, int64_t now_ms, Opened* app) {
    if (state_ == PathState::Closed) return false;
    Opened o;
    if (!accept(pkt, len, &o)) return false;
    if (o.type == Type::Close) {
        if (state_ == PathState::Up) stats_.up_ms += now_ms - up_since_;
        wipe();
        state_ = PathState::Closed;
        return false;
    }
    if (o.type != Type::Dgram && o.type != Type::Seg && o.type != Type::Ack) return false;
    *app = std::move(o);
    return true;
}

bool Path::send(Type type, const uint8_t* body, size_t len, int64_t now_ms, std::vector<Out>* out) {
    if (state_ != PathState::Up) return false;
    std::vector<uint8_t> p = seal_next(type, body, len);
    if (p.empty()) return false;
    out->push_back({path_, std::move(p)});
    last_tx_ = now_ms;
    stats_.sent++;
    return true;
}

std::vector<uint8_t> Path::seal_for_relay(Type type, const uint8_t* body, size_t len) {
    if (state_ == PathState::Closed) return {};
    return seal_next(type, body, len);
}

void Path::close(int64_t now_ms, std::vector<Out>* out) {
    if (state_ == PathState::Closed) return;
    if (state_ == PathState::Up) {
        emit(path_, Type::Close, nullptr, 0, now_ms, out);
        stats_.up_ms += now_ms - up_since_;
    }
    wipe();
    state_ = PathState::Closed;
}

PathStats Path::stats() const {
    PathStats s = stats_;
    if (state_ == PathState::Up) s.up_ms += last_tick_ - up_since_;
    if (!rtts_.empty()) {
        std::vector<int> v = rtts_;
        std::sort(v.begin(), v.end());
        auto rank = [&](int pct) { return v[(v.size() * pct + 99) / 100 - 1]; };  // nearest rank
        s.rtt_p50 = rank(50);
        s.rtt_p95 = rank(95);
    }
    return s;
}

}  // namespace direct
