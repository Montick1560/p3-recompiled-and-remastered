// pdp::Sockets / ptp::Sockets in relay mode against a small in-process
// aemu_postoffice relay (PDP forwarding, PTP listen/connect/accept pairing).
#include "net/pdp.h"
#include "net/ptp.h"
#include "net/relay_protocol.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <tuple>
#include <vector>

#include "psp_socket_compat.h"
#ifndef _WIN32
#include <sys/select.h>
#endif

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static const pro::Mac kA = {0x02, 0, 0, 0, 0, 0x0a};
static const pro::Mac kB = {0x02, 0, 0, 0, 0, 0x0b};
static const pro::Mac kBroadcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// ---- fake relay ----
class FakeRelay {
public:
    FakeRelay() {
        psp_sock_startup();
        listener_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(listener_, 16);
        socklen_t len = sizeof(a);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { run(); });
    }
    ~FakeRelay() {
        stop_ = true;
        thread_.join();
        for (auto& kv : sessions_) psp_sock_close(kv.first);
        psp_sock_close(listener_);
    }
    uint16_t port() const { return port_; }

private:
    struct S {
        std::vector<uint8_t> rx;
        int type = -1;
        pro::Mac src{}, dst{};
        uint16_t sport = 0, dport = 0;
        int peer = -1;
        bool waiting = false;
    };
    using Key = std::tuple<int, pro::Mac, uint16_t>;  // (type, mac, port)

    void send(int fd, const std::vector<uint8_t>& b) { ::send(fd, reinterpret_cast<const char*>(b.data()), (int)b.size(), 0); }
    static std::vector<uint8_t> addr(const pro::Mac& m, uint16_t p) {
        std::vector<uint8_t> b(m.begin(), m.end());
        b.push_back(0);
        b.push_back(0);
        b.push_back(static_cast<uint8_t>(p));
        b.push_back(static_cast<uint8_t>(p >> 8));
        return b;
    }
    void drop(int fd) {
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;
        const int peer = it->second.peer;
        psp_sock_close(fd);
        sessions_.erase(it);
        for (auto r = reg_.begin(); r != reg_.end();) r = r->second == fd ? reg_.erase(r) : std::next(r);
        if (peer >= 0) drop(peer);
    }
    int find(int type, const pro::Mac& m, uint16_t p) {
        auto it = reg_.find(Key{type, m, p});
        return it == reg_.end() ? -1 : it->second;
    }
    void on_bytes(int fd) {
        S& s = sessions_[fd];
        for (;;) {
            if (s.type < 0) {
                if (s.rx.size() < 24) return;
                std::memcpy(&s.type, s.rx.data(), 4);
                std::memcpy(s.src.data(), s.rx.data() + 4, 6);
                s.sport = static_cast<uint16_t>(s.rx[12] | (s.rx[13] << 8));
                std::memcpy(s.dst.data(), s.rx.data() + 14, 6);
                s.dport = static_cast<uint16_t>(s.rx[22] | (s.rx[23] << 8));
                s.rx.erase(s.rx.begin(), s.rx.begin() + 24);
                if (s.type == relay::INIT_PDP || s.type == relay::INIT_PTP_LISTEN) {
                    reg_[Key{s.type, s.src, s.sport}] = fd;
                } else if (s.type == relay::INIT_PTP_CONNECT) {
                    const int l = find(relay::INIT_PTP_LISTEN, s.dst, s.dport);
                    if (l < 0) return drop(fd);
                    s.waiting = true;
                    send(l, addr(s.src, s.sport));
                } else {  // ACCEPT: find the waiting connect from (dst, dport) to (src, sport)
                    int c = -1;
                    for (auto& kv : sessions_) {
                        const S& o = kv.second;
                        if (o.waiting && o.type == relay::INIT_PTP_CONNECT && o.src == s.dst &&
                            o.sport == s.dport && o.dst == s.src && o.dport == s.sport) c = kv.first;
                    }
                    if (c < 0) return drop(fd);
                    S& o = sessions_[c];
                    o.waiting = false;
                    o.peer = fd;
                    s.peer = c;
                    send(c, addr(s.src, s.sport));
                    send(fd, addr(s.dst, s.dport));
                }
                continue;
            }
            if (s.type == relay::INIT_PDP) {
                if (s.rx.size() < 14) return;
                uint32_t n;
                std::memcpy(&n, s.rx.data() + 10, 4);
                if (s.rx.size() < 14 + n) return;
                pro::Mac to;
                std::memcpy(to.data(), s.rx.data(), 6);
                const uint16_t tp = static_cast<uint16_t>(s.rx[8] | (s.rx[9] << 8));
                const int t = find(relay::INIT_PDP, to, tp);
                if (t >= 0) {
                    const auto h = relay::encode_pdp_header(s.src, s.sport, n);
                    std::vector<uint8_t> out(h.begin(), h.end());
                    out.insert(out.end(), s.rx.begin() + 14, s.rx.begin() + 14 + n);
                    send(t, out);
                }
                s.rx.erase(s.rx.begin(), s.rx.begin() + 14 + n);
                continue;
            }
            if (s.peer >= 0 && !s.rx.empty()) {  // PTP data: pass the frames through
                send(s.peer, s.rx);
                s.rx.clear();
            }
            return;
        }
    }
    void run() {
        while (!stop_) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(static_cast<unsigned>(listener_), &set);
            int maxfd = listener_;
            for (auto& kv : sessions_) {
                FD_SET(static_cast<unsigned>(kv.first), &set);
                if (kv.first > maxfd) maxfd = kv.first;
            }
            timeval tv{0, 20000};
            if (::select(maxfd + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
            if (FD_ISSET(static_cast<unsigned>(listener_), &set)) {
                const int fd = static_cast<int>(::accept(listener_, nullptr, nullptr));
                if (fd >= 0) sessions_[fd];
            }
            std::vector<int> ready;
            for (auto& kv : sessions_) {
                if (FD_ISSET(static_cast<unsigned>(kv.first), &set)) ready.push_back(kv.first);
            }
            for (int fd : ready) {
                if (!sessions_.count(fd)) continue;
                uint8_t buf[4096];
                const int n = ::recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n <= 0) {
                    drop(fd);
                    continue;
                }
                sessions_[fd].rx.insert(sessions_[fd].rx.end(), buf, buf + n);
                on_bytes(fd);
            }
        }
    }

    int listener_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::map<int, S> sessions_;
    std::map<Key, int> reg_;
};

static pdp::Resolver resolver(const FakeRelay& relay, const pro::Mac& self, const pro::Mac& other) {
    pdp::Resolver r;
    r.peers = [other] { return std::vector<pdp::Peer>{{other, 0}}; };
    r.port_offset = 10000;
    r.relay = true;
    r.relay_host = "127.0.0.1";
    r.relay_port = relay.port();
    r.self_mac = self;
    return r;
}

static void pdp_relay(const FakeRelay& relay) {
    pdp::Sockets a, b;
    a.set_resolver(resolver(relay, kA, kB));
    b.set_resolver(resolver(relay, kB, kA));
    const int ia = a.create(3000, 8192), ib = b.create(3000, 8192);
    CHECK(ia >= 1 && ib >= 1 && ia != ib, "PDP sockets created through the relay");
    CHECK(a.local_port(ia) == 3000, "local port is the PSP port");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint8_t abc[3] = {'a', 'b', 'c'};
    CHECK(b.send(ib, kA, 3000, abc, 3) == 0, "send to A");
    uint8_t buf[16];
    pro::Mac from{};
    uint16_t from_port = 0;
    int len = 2;
    CHECK(a.recv(ia, &from, &from_port, buf, &len, 1000000, false) == pdp::kErrNotEnoughSpace && len == 3,
          "small buffer: datagram size reported");
    len = sizeof(buf);
    CHECK(a.recv(ia, &from, &from_port, buf, &len, 1000000, false) == 0 && len == 3 && from == kB &&
              from_port == 3000 && std::memcmp(buf, abc, 3) == 0, "datagram from B through the relay");
    len = sizeof(buf);
    CHECK(a.recv(ia, &from, &from_port, buf, &len, 0, true) == pdp::kErrWouldBlock, "nothing else queued");
    CHECK(b.send(ib, kBroadcast, 3000, abc, 3) == 0, "broadcast");
    len = sizeof(buf);
    CHECK(a.recv(ia, &from, &from_port, buf, &len, 1000000, false) == 0 && len == 3, "broadcast reached the peer");
    CHECK(a.remove(ia) == 0 && b.remove(ib) == 0, "remove");
}

static void ptp_relay(const FakeRelay& relay) {
    ptp::Sockets a, b;
    a.set_resolver(resolver(relay, kA, kB));
    b.set_resolver(resolver(relay, kB, kA));
    const int l = a.listen(3100, 8192, 1);
    CHECK(l >= 1 && a.state(l) == ptp::LISTEN, "listen through the relay");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int c = b.open(3200, kA, 3100, 8192);
    CHECK(c >= 1, "open");
    CHECK(b.connect(c, 0, true) == ptp::kErrWouldBlock, "nonblocking connect: in progress");
    pro::Mac peer{};
    uint16_t peer_port = 0;
    const int s = a.accept(l, &peer, &peer_port, 2000000, false);
    CHECK(s >= 1 && peer == kB && peer_port == 3200, "accept: B's MAC and PSP port");
    CHECK(b.connect(c, 2000000, false) == 0 && b.state(c) == ptp::ESTABLISHED, "connect completes");
    const uint8_t ping[4] = {'p', 'i', 'n', 'g'};
    int len = 4;
    CHECK(b.send(c, ping, &len, 1000000, false) == 0 && len == 4, "send ping");
    uint8_t buf[16];
    len = 2;
    CHECK(a.recv(s, buf, &len, 1000000, false) == 0 && len == 2 && buf[0] == 'p', "first part of the frame");
    len = sizeof(buf);
    CHECK(a.recv(s, buf, &len, 1000000, false) == 0 && len == 2 && buf[1] == 'g', "rest of the frame");
    len = 4;
    CHECK(a.send(s, reinterpret_cast<const uint8_t*>("pong"), &len, 1000000, false) == 0, "send pong");
    len = sizeof(buf);
    CHECK(b.recv(c, buf, &len, 1000000, false) == 0 && len == 4 && std::memcmp(buf, "pong", 4) == 0, "pong back");
    len = sizeof(buf);
    CHECK(b.recv(c, buf, &len, 0, true) == ptp::kErrWouldBlock, "nothing pending");
    CHECK(a.close(s) == 0, "close accepted socket");
    len = sizeof(buf);
    CHECK(b.recv(c, buf, &len, 1000000, false) == ptp::kErrDisconnected, "peer closed: disconnected");
    a.close(l);
    b.close(c);
}

int main() {
    FakeRelay relay;
    pdp_relay(relay);
    ptp_relay(relay);
    if (failures == 0) std::printf("relay_sockets tests passed\n");
    return failures == 0 ? 0 : 1;
}
