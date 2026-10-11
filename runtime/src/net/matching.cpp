// PSP ad hoc matching state machine, ported from PPSSPP
// Core/HLE/sceNetAdhocMatching.cpp and proAdhoc.cpp (GPL-2.0-or-later).
// Function comments name the PPSSPP original.
#include "net/matching.h"

#include <algorithm>

namespace matching {

Context::Context(Mode mode, int maxpeers, uint16_t port, uint32_t hello_us, int32_t keepalive_us,
                 int init_count, uint32_t rexmt_us, const pro::Mac& self, Io io)
    : mode_(mode),
      maxpeers_(maxpeers),
      port_(port),
      hello_us_(hello_us),
      keepalive_us_(keepalive_us < 1 ? kDefaultKeepaliveUs : static_cast<uint32_t>(keepalive_us)),
      self_(self),
      io_(std::move(io)) {
    // sceNetAdhocMatchingCreate: the game's timeout plus 0.5 s for Internet play.
    timeout_us_ = (static_cast<uint64_t>(static_cast<uint32_t>(keepalive_us)) + rexmt_us) *
                      static_cast<uint64_t>(init_count) + 500000;
}

void Context::start(std::vector<uint8_t> hello_opt) {
    if (!hello_opt.empty()) hello_ = std::move(hello_opt);
    running_ = true;
    first_tick_ = true;
}

// matchingInputThread exit path: sendByePacket + clearPeerList.
void Context::stop() {
    if (!running_) return;
    for (const auto& m : peers_) {
        if (m.state == PEER_PARENT || m.state == PEER_CHILD || m.state == PEER_P2P ||
            m.state == PEER_CANCEL_IN_PROGRESS) {
            Packet bye;
            bye.type = BYE;
            send_to(m.mac, bye);
        }
    }
    peers_.clear();
    running_ = false;
}

int Context::set_hello_opt(std::vector<uint8_t> opt) {
    if (mode_ == CHILD) return kErrInvalidMode;
    if (!running_) return kErrNotRunning;
    hello_ = std::move(opt);
    return 0;
}

// ---- peer list helpers (proAdhoc.cpp) ----

Context::Member* Context::find(const pro::Mac& mac) {
    for (auto& m : peers_) {
        if (m.mac == mac) return &m;
    }
    return nullptr;
}

const Context::Member* Context::find(const pro::Mac& mac) const {
    for (const auto& m : peers_) {
        if (m.mac == mac) return &m;
    }
    return nullptr;
}

Context::Member* Context::find_state(int state) {
    for (auto& m : peers_) {
        if (m.state == state) return &m;
    }
    return nullptr;
}

const Context::Member* Context::find_state(int state) const {
    for (const auto& m : peers_) {
        if (m.state == state) return &m;
    }
    return nullptr;
}

int Context::count_children() const {
    return static_cast<int>(std::count_if(peers_.begin(), peers_.end(),
                                          [](const Member& m) { return m.state == PEER_CHILD; }));
}

void Context::erase(const pro::Mac& mac) {
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const Member& m) { return m.mac == mac; }),
                 peers_.end());
}

void Context::add_front(const pro::Mac& mac, int state, uint64_t now) {
    peers_.insert(peers_.begin(), Member{mac, state, now});
}

int Context::connected_count() const {  // countConnectedPeers
    if (mode_ == PARENT) return count_children() + 1;
    if (mode_ == CHILD) return find_state(PEER_PARENT) ? count_children() + 2 : 1;
    return find_state(PEER_P2P) ? 2 : 1;
}

int Context::peer_state(const pro::Mac& mac) const {
    const Member* m = find(mac);
    return m ? m->state : -1;
}

uint16_t Context::port_of(const pro::Mac& mac) const {
    auto it = peer_port_.find(mac);
    return it != peer_port_.end() ? it->second : port_;
}

void Context::send_to(const pro::Mac& mac, const Packet& p) {
    if (io_.send) io_.send(mac, port_of(mac), encode(p));
}

void Context::notify(int event, const pro::Mac& mac, const std::vector<uint8_t>& opt) {
    if (io_.notify) io_.notify(event, mac, opt);
}

// ---- senders (sceNetAdhocMatching.cpp send*Packet) ----

void Context::send_accept(const pro::Mac& mac, const std::vector<uint8_t>& opt) {
    const Member* peer = find(mac);
    if (!peer || (peer->state != PEER_CHILD && peer->state != PEER_P2P)) return;
    Packet p;
    p.type = ACCEPT;
    p.opt = opt;
    if (mode_ == PARENT && connected_count() - 2 > 0) {
        for (const auto& m : peers_) {
            if (m.mac != mac && m.state == PEER_CHILD) p.siblings.push_back(m.mac);
        }
    }
    send_to(mac, p);
    notify(EV_ESTABLISHED, mac);
}

void Context::send_join(const pro::Mac& mac, const std::vector<uint8_t>& opt) {
    const Member* peer = find(mac);
    if (!peer || peer->state != PEER_OUTGOING_REQUEST) return;
    Packet p;
    p.type = JOIN;
    p.opt = opt;
    send_to(mac, p);
}

void Context::send_cancel(const pro::Mac& mac, const std::vector<uint8_t>& opt, uint64_t now) {
    Packet p;
    p.type = CANCEL;
    p.opt = opt;
    send_to(mac, p);
    Member* peer = find(mac);
    if (!peer) return;
    if (mode_ == CHILD) {
        peers_.clear();
    } else {
        peer->lastping = now;
    }
}

void Context::send_birth(const pro::Mac& mac) {
    if (!find(mac)) return;
    Packet p;
    p.type = BIRTH;
    p.mac = mac;
    for (const auto& m : peers_) {
        if (m.mac != mac && m.state == PEER_CHILD) send_to(m.mac, p);
    }
}

void Context::send_death(const pro::Mac& mac) {
    if (!find(mac)) return;
    for (const auto& m : peers_) {
        Packet p;
        if (m.mac == mac) {
            p.type = BYE;
        } else if (m.state == PEER_CHILD) {
            p.type = DEATH;
            p.mac = mac;
        } else {
            continue;
        }
        send_to(m.mac, p);
    }
    erase(mac);
}

// proAdhoc.cpp handleTimeout
void Context::handle_timeout(uint64_t now) {
    const std::vector<Member> snapshot = peers_;
    for (const auto& m : snapshot) {
        if (m.state == PEER_SELF || now - m.lastping <= timeout_us_) continue;
        const bool watched =
            (mode_ == CHILD && m.state == PEER_PARENT) || (mode_ == PARENT && m.state == PEER_CHILD) ||
            (mode_ == P2P && (m.state == PEER_P2P || m.state == PEER_OFFER || m.state == PEER_INCOMING_REQUEST ||
                              m.state == PEER_OUTGOING_REQUEST || m.state == PEER_CANCEL_IN_PROGRESS));
        if (!watched) continue;
        notify(EV_TIMEOUT, m.mac);
        if (mode_ == PARENT) {
            send_death(m.mac);
        } else {
            send_cancel(m.mac, {}, now);
        }
    }
}

// matchingInputThread loop body (timers)
void Context::tick(uint64_t now) {
    if (!running_) return;
    if (first_tick_) {
        first_tick_ = false;
        last_hello_ = now - hello_us_;
        last_ping_ = now - keepalive_us_;
    }
    const bool offering = (mode_ == PARENT && count_children() < maxpeers_ - 1) ||
                          (mode_ == P2P && find_state(PEER_P2P) == nullptr);
    if (offering && hello_us_ > 0 && now - last_hello_ >= hello_us_) {
        Packet hello;
        hello.type = HELLO;
        hello.opt = hello_;
        if (io_.group_peers) {
            for (const auto& mac : io_.group_peers()) send_to(mac, hello);
        }
        last_hello_ = now;
    }
    if (now - last_ping_ >= keepalive_us_) {
        handle_timeout(now);
        Packet ping;
        if (io_.group_peers) {
            for (const auto& mac : io_.group_peers()) send_to(mac, ping);
        }
        last_ping_ = now;
    }
}

// ---- public calls (sceNetAdhocMatchingSelectTarget / CancelTargetWithOpt) ----

int Context::select_target(const pro::Mac& mac, const std::vector<uint8_t>& opt, uint64_t now) {
    (void)now;
    if (!running_) return kErrNotRunning;
    Member* peer = find(mac);
    if (!peer) return kErrUnknownTarget;
    if (mode_ == PARENT) {
        if (peer->state == PEER_CHILD) return kErrAlreadyEstablished;
        if (count_children() == maxpeers_ - 1) return kErrExceedMaxnum;
        if (peer->state == PEER_INCOMING_REQUEST) {
            peer->state = PEER_CHILD;
            send_birth(mac);
            send_accept(mac, opt);
            return 0;
        }
    } else if (mode_ == CHILD) {
        if (find_state(PEER_PARENT)) return kErrAlreadyEstablished;
        if (find_state(PEER_OUTGOING_REQUEST)) return kErrRequestInProgress;
        if (peer->state == PEER_OFFER) {
            peer->state = PEER_OUTGOING_REQUEST;
            send_join(mac, opt);
            return 0;
        }
    } else {
        if (find_state(PEER_P2P)) return kErrAlreadyEstablished;
        if (find_state(PEER_OUTGOING_REQUEST)) return kErrRequestInProgress;
        if (peer->state == PEER_OFFER) {
            peer->state = PEER_OUTGOING_REQUEST;
            send_join(mac, opt);
            return 0;
        }
        if (peer->state == PEER_INCOMING_REQUEST) {
            peer->state = PEER_P2P;
            send_accept(mac, opt);
            return 0;
        }
    }
    return kErrTargetNotReady;
}

int Context::cancel_target(const pro::Mac& mac, const std::vector<uint8_t>& opt, uint64_t now) {
    if (!running_) return kErrNotRunning;
    Member* peer = find(mac);
    if (!peer) return 0;
    const bool cancellable =
        (mode_ == CHILD && (peer->state == PEER_PARENT || peer->state == PEER_OUTGOING_REQUEST)) ||
        (mode_ == PARENT && (peer->state == PEER_CHILD || peer->state == PEER_INCOMING_REQUEST)) ||
        (mode_ == P2P && (peer->state == PEER_P2P || peer->state == PEER_INCOMING_REQUEST));
    if (!cancellable) return 0;
    if (mode_ == PARENT && peer->state == PEER_CHILD && connected_count() > 1) {
        send_death(mac);  // removes the peer
        Packet cancel;
        cancel.type = CANCEL;
        cancel.opt = opt;
        send_to(mac, cancel);
        return 0;
    }
    peer->state = PEER_CANCEL_IN_PROGRESS;
    send_cancel(mac, opt, now);
    if (Member* still = find(mac)) still->lastping = 0;
    return 0;
}

// ---- receive (sceNetAdhocMatching.cpp actOn*Packet) ----

void Context::on_packet(const pro::Mac& from, uint16_t from_port, const uint8_t* data, size_t len,
                        uint64_t now) {
    if (!running_ || data == nullptr || len == 0) return;
    peer_port_[from] = from_port;
    Packet p;
    const bool ok = decode(data, len, &p);
    if (!ok) {
        if (data[0] == JOIN) on_join(from, nullptr, now);  // PPSSPP answers a bad JOIN with CANCEL
        return;
    }
    switch (p.type) {
    case PING:
        if (Member* m = find(from)) m->lastping = now;
        break;
    case HELLO: on_hello(from, p, now); break;
    case JOIN: on_join(from, &p, now); break;
    case ACCEPT: on_accept(from, p, now); break;
    case CANCEL: on_cancel(from, p); break;
    case BULK: on_bulk(from, p); break;
    case BIRTH: on_birth(from, p, now); break;
    case DEATH: on_death(from, p); break;
    case BYE: on_bye(from); break;
    default: break;
    }
}

void Context::on_hello(const pro::Mac& from, const Packet& p, uint64_t now) {
    if (!((mode_ == CHILD && !find_state(PEER_PARENT)) || (mode_ == P2P && !find_state(PEER_P2P)))) return;
    Member* peer = find(from);
    if (!peer) {
        add_front(from, PEER_OFFER, now);
        peer = find(from);
    }
    if (peer->state != PEER_OUTGOING_REQUEST && peer->state != PEER_INCOMING_REQUEST) {
        notify(EV_HELLO, from, p.opt);
    }
}

void Context::on_join(const pro::Mac& from, const Packet* p, uint64_t now) {
    if (mode_ == CHILD) return;
    const bool room = (mode_ == PARENT && count_children() < maxpeers_ - 1) ||
                      (mode_ == P2P && !find_state(PEER_P2P));
    if (room && p != nullptr) {
        Member* peer = find(from);
        // A parent learns of a child only through a join: a known one is a repeat.
        if (peer && peer->lastping != 0 && mode_ == PARENT) return;
        if (!peer) {
            add_front(from, PEER_INCOMING_REQUEST, now);
        } else {
            peer->state = PEER_INCOMING_REQUEST;
            peer->lastping = now;
        }
        notify(EV_REQUEST, from, p->opt);
        return;
    }
    send_cancel(from, {}, now);
}

void Context::on_accept(const pro::Mac& from, const Packet& p, uint64_t now) {
    if (mode_ == PARENT) return;
    if (!((mode_ == CHILD && !find_state(PEER_PARENT)) || (mode_ == P2P && !find_state(PEER_P2P)))) return;
    Member* request = find_state(PEER_OUTGOING_REQUEST);
    if (!request || request->mac != from) return;
    request->state = mode_ == CHILD ? PEER_PARENT : PEER_P2P;
    // postAcceptCleanPeerList
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [](const Member& m) {
                                    return m.state != PEER_CHILD && m.state != PEER_P2P && m.state != PEER_PARENT &&
                                           m.state != PEER_SELF;
                                }),
                 peers_.end());
    if (mode_ == CHILD) {
        // postAcceptAddSiblings (last sibling first) then addMember(self)
        for (auto it = p.siblings.rbegin(); it != p.siblings.rend(); ++it) {
            if (Member* m = find(*it)) {
                m->state = PEER_CHILD;
                m->lastping = now;
            } else {
                add_front(*it, PEER_CHILD, now);
            }
        }
        if (Member* me = find(self_)) {
            me->state = PEER_SELF;
            me->lastping = now;
        } else {
            add_front(self_, PEER_SELF, now);
        }
    }
    notify(EV_ESTABLISHED, from);
    notify(EV_ACCEPT, from, p.opt);
}

void Context::on_cancel(const pro::Mac& from, const Packet& p) {
    Member* peer = find(from);
    if (!peer) return;
    const Member* request = find_state(PEER_OUTGOING_REQUEST);
    if (mode_ == CHILD) {
        const Member* parent = find_state(PEER_PARENT);
        if (request == peer) {
            notify(EV_DENY, from, p.opt);
            peer->lastping = 0;
        } else if (parent == peer) {
            const std::vector<Member> snapshot = peers_;
            for (const auto& m : snapshot) {
                if (m.state == PEER_CHILD || m.state == PEER_PARENT) notify(EV_LEAVE, m.mac, p.opt);
            }
            peers_.clear();
        }
    } else if (mode_ == PARENT) {
        if (peer->state == PEER_INCOMING_REQUEST) {
            notify(EV_CANCEL, from, p.opt);
            peer->lastping = 0;
        } else if (peer->state == PEER_CHILD) {
            notify(EV_LEAVE, from, p.opt);
            peer->lastping = 0;
        }
    } else {
        const Member* p2p = find_state(PEER_P2P);
        if (request == peer) {
            notify(EV_DENY, from, p.opt);
            peer->lastping = 0;
        } else if (p2p == peer) {
            notify(EV_LEAVE, from, p.opt);
            peer->lastping = 0;
        } else if (peer->state == PEER_INCOMING_REQUEST) {
            notify(EV_CANCEL, from, p.opt);
            peer->lastping = 0;
        }
    }
}

void Context::on_bulk(const pro::Mac& from, const Packet& p) {
    const Member* peer = find(from);
    if (!peer || p.opt.empty()) return;
    const bool member = (mode_ == PARENT && peer->state == PEER_CHILD) ||
                        (mode_ == CHILD && (peer->state == PEER_CHILD || peer->state == PEER_PARENT)) ||
                        (mode_ == P2P && peer->state == PEER_P2P);
    if (member) notify(EV_DATA, from, p.opt);
}

void Context::on_birth(const pro::Mac& from, const Packet& p, uint64_t now) {
    const Member* peer = find(from);
    if (!peer || mode_ != CHILD || peer != find_state(PEER_PARENT)) return;
    add_front(p.mac, PEER_CHILD, now);
}

void Context::on_death(const pro::Mac& from, const Packet& p) {
    const Member* peer = find(from);
    if (!peer || mode_ != CHILD || peer != find_state(PEER_PARENT)) return;
    const Member* dead = find(p.mac);
    if (!dead || dead->state != PEER_CHILD) return;
    notify(EV_LEAVE, p.mac);
    erase(p.mac);
}

void Context::on_bye(const pro::Mac& from) {
    const Member* peer = find(from);
    if (!peer) return;
    const int s = peer->state;
    if ((mode_ == PARENT && s == PEER_CHILD) || (mode_ == CHILD && s == PEER_CHILD) ||
        (mode_ == P2P && (s == PEER_P2P || s == PEER_OFFER || s == PEER_INCOMING_REQUEST ||
                          s == PEER_OUTGOING_REQUEST || s == PEER_CANCEL_IN_PROGRESS))) {
        if (mode_ != CHILD) notify(EV_BYE, from);
        erase(from);
    } else if (mode_ == CHILD && s == PEER_PARENT) {
        notify(EV_BYE, from);
        peers_.clear();
    }
}

// sceNetAdhocMatchingGetMembers
std::vector<pro::Mac> Context::members() const {
    std::vector<pro::Mac> out;
    const int count = connected_count();
    if (count == 1 || mode_ != CHILD) out.push_back(self_);
    if (mode_ == P2P) {
        if (const Member* p2p = find_state(PEER_P2P)) out.push_back(p2p->mac);
        return out;
    }
    if (const Member* parent = find_state(PEER_PARENT)) out.push_back(parent->mac);
    // Oldest first (PPSSPP pushes the newest-first list to the front of a deque).
    for (auto it = peers_.rbegin(); it != peers_.rend(); ++it) {
        if (it->state == PEER_CHILD || (mode_ == CHILD && it->state == PEER_SELF)) out.push_back(it->mac);
    }
    return out;
}

}  // namespace matching
