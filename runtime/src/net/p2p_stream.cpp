// The game's TCP P2P sockets over the infrastructure server (see net/p2p_stream.h).
#include "net/p2p_stream.h"

#include "net/inet_errno.h"

namespace p2p {

using namespace inet_errno;

void Streams::emit(uint32_t vip, uint16_t dst, uint16_t src, uint8_t type, const uint8_t* data, size_t len) {
    std::vector<uint8_t> b;
    b.reserve(1 + len);
    b.push_back(type);
    if (len) b.insert(b.end(), data, data + len);
    send_(vip, dst, src, b);
}

Streams::Conn* Streams::find(uint16_t local, uint32_t vip, uint16_t port) {
    for (auto& [id, c] : conns_)
        if (c.local == local && c.peer_vip == vip && c.peer_port == port) return &c;
    auto it = backlog_.find(local);
    if (it != backlog_.end())
        for (Conn& c : it->second)
            if (c.peer_vip == vip && c.peer_port == port) return &c;
    return nullptr;
}

bool Streams::port_used(uint16_t port) const {
    for (const auto& [id, p] : listeners_)
        if (p == port) return true;
    for (const auto& [id, c] : conns_)
        if (c.local == port) return true;
    return false;
}

uint16_t Streams::alloc_port() {
    for (;;) {
        const uint16_t p = next_port_;
        next_port_ = next_port_ == 0xFFFF ? kFirstLocalPort : static_cast<uint16_t>(next_port_ + 1);
        if (!port_used(p)) return p;
    }
}

int Streams::listen(int id, uint16_t port) {
    for (const auto& [lid, p] : listeners_)
        if (p == port && lid != id) return -kEADDRINUSE;
    listeners_[id] = port;
    return 0;
}

int Streams::connect(int id, uint32_t vip, uint16_t port) {
    auto it = conns_.find(id);
    if (it == conns_.end()) {
        Conn c;
        c.local = alloc_port();
        c.peer_vip = vip;
        c.peer_port = port;
        conns_[id] = c;
        emit(vip, port, c.local, kSyn);
        return -kEINPROGRESS;
    }
    const Conn& c = it->second;
    if (c.refused) return -kECONNREFUSED;
    return c.connected ? 0 : -kEALREADY;
}

int Streams::accept(int listener, int new_id, uint32_t* vip, uint16_t* port) {
    auto l = listeners_.find(listener);
    if (l == listeners_.end()) return -kEINVAL;
    auto b = backlog_.find(l->second);
    if (b == backlog_.end() || b->second.empty()) return -kEAGAIN;
    Conn c = std::move(b->second.front());
    b->second.pop_front();
    *vip = c.peer_vip;
    *port = c.peer_port;
    conns_[new_id] = std::move(c);
    return 0;
}

int Streams::send(int id, const uint8_t* data, size_t len) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return -kENOTCONN;
    const Conn& c = it->second;
    if (c.reset) return -kECONNRESET;
    if (!c.connected) return -kENOTCONN;
    for (size_t o = 0; o < len; o += kMaxSegmentData) {
        const size_t n = len - o < kMaxSegmentData ? len - o : kMaxSegmentData;
        emit(c.peer_vip, c.peer_port, c.local, kData, data + o, n);
    }
    return static_cast<int>(len);
}

int Streams::recv(int id, uint8_t* out, size_t cap) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return -kENOTCONN;
    Conn& c = it->second;
    if (!c.rx.empty()) {
        size_t n = 0;
        while (n < cap && !c.rx.empty()) {
            out[n++] = c.rx.front();
            c.rx.pop_front();
        }
        return static_cast<int>(n);
    }
    if (c.eof) return 0;
    if (c.reset) return -kECONNRESET;
    return -kEAGAIN;
}

void Streams::close(int id) {
    auto l = listeners_.find(id);
    if (l != listeners_.end()) {
        auto b = backlog_.find(l->second);
        if (b != backlog_.end()) {
            for (const Conn& c : b->second) emit(c.peer_vip, c.peer_port, c.local, kRst);
            backlog_.erase(b);
        }
        listeners_.erase(l);
    }
    auto it = conns_.find(id);
    if (it != conns_.end()) {
        const Conn& c = it->second;
        if (c.connected && !c.reset) emit(c.peer_vip, c.peer_port, c.local, kFin);
        conns_.erase(it);
    }
}

uint16_t Streams::local_port(int id) const {
    auto it = conns_.find(id);
    return it == conns_.end() ? 0 : it->second.local;
}

bool Streams::idle() const {
    for (const auto& [id, c] : conns_)
        if (!c.rx.empty()) return false;
    for (const auto& [port, q] : backlog_)
        for (const Conn& c : q)
            if (!c.rx.empty()) return false;
    return true;
}

void Streams::on_segment(const Segment& s) {
    if (s.bytes.empty()) return;
    const uint8_t type = s.bytes[0];
    Conn* c = find(s.dst_port, s.vip, s.src_port);
    switch (type) {
    case kSyn: {
        bool listening = false;
        for (const auto& [id, p] : listeners_) listening |= p == s.dst_port;
        if (!listening) {
            emit(s.vip, s.src_port, s.dst_port, kRst);
            return;
        }
        if (!c) {
            Conn n;
            n.local = s.dst_port;
            n.peer_vip = s.vip;
            n.peer_port = s.src_port;
            n.connected = true;
            backlog_[s.dst_port].push_back(std::move(n));
        }
        emit(s.vip, s.src_port, s.dst_port, kAccept);
        return;
    }
    case kAccept:
        if (c) c->connected = true;
        return;
    case kData:
        if (!c) {
            emit(s.vip, s.src_port, s.dst_port, kRst);
            return;
        }
        c->rx.insert(c->rx.end(), s.bytes.begin() + 1, s.bytes.end());
        return;
    case kFin:
        if (c) c->eof = true;
        return;
    case kRst:
        if (!c) return;
        if (c->connected)
            c->reset = true;
        else
            c->refused = true;
        return;
    default:
        return;
    }
}

}  // namespace p2p
