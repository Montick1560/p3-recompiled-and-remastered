// p2p_stream: the game's TCP P2P sockets as ordered segments relayed by the
// infrastructure server. Two Streams tables are wired back to back here.
#include "net/p2p_stream.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "net/inet_errno.h"

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

using namespace inet_errno;

struct Wire {
    struct Item {
        int to;  // 0 = A, 1 = B
        p2p::Segment seg;
    };
    std::deque<Item> q;
};

static constexpr uint32_t kVipA = 0x0A500001, kVipB = 0x0A500002;

struct Pair {
    Wire w;
    p2p::Streams a{[this](uint32_t vip, uint16_t dst, uint16_t src, const std::vector<uint8_t>& b) {
        w.q.push_back({vip == kVipB ? 1 : -1, p2p::Segment{kVipA, dst, src, b}});
    }};
    p2p::Streams b{[this](uint32_t vip, uint16_t dst, uint16_t src, const std::vector<uint8_t>& bytes) {
        w.q.push_back({vip == kVipA ? 0 : -1, p2p::Segment{kVipB, dst, src, bytes}});
    }};
    int deliver() {
        int n = 0;
        while (!w.q.empty()) {
            Wire::Item it = w.q.front();
            w.q.pop_front();
            if (it.to == 0) a.on_segment(it.seg);
            if (it.to == 1) b.on_segment(it.seg);
            n++;
        }
        return n;
    }
};

static std::string recv_all(p2p::Streams& s, int id) {
    std::string out;
    char buf[700];
    for (;;) {
        const int n = s.recv(id, reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

static void connect_accept_data() {
    Pair p;
    CHECK(p.b.listen(1, 3700) == 0, "listen");
    uint32_t vip = 0;
    uint16_t port = 0;
    CHECK(p.b.accept(1, 9, &vip, &port) == -kEAGAIN, "nothing to accept yet");
    CHECK(p.a.connect(0, kVipB, 3700) == -kEINPROGRESS, "first connect: in progress");
    CHECK(p.a.connect(0, kVipB, 3700) == -kEALREADY, "again before the answer: already");
    CHECK(p.a.send(0, reinterpret_cast<const uint8_t*>("x"), 1) == -kENOTCONN, "no send before connected");
    p.deliver();
    CHECK(p.a.connect(0, kVipB, 3700) == 0, "connected once the listener answered");
    CHECK(p.b.accept(1, 9, &vip, &port) == 0 && vip == kVipA && port == p.a.local_port(0) && port != 0,
          "accept gives the connector's vip and port");
    CHECK(p.b.accept(1, 10, &vip, &port) == -kEAGAIN, "one connection, one accept");
    CHECK(p.a.send(0, reinterpret_cast<const uint8_t*>("hello"), 5) == 5, "send");
    p.deliver();
    uint8_t buf[16];
    CHECK(p.b.recv(9, buf, sizeof(buf)) == 5 && std::memcmp(buf, "hello", 5) == 0, "recv on the accepted socket");
    CHECK(p.b.recv(9, buf, sizeof(buf)) == -kEAGAIN, "then nothing");
    CHECK(p.b.send(9, reinterpret_cast<const uint8_t*>("world"), 5) == 5, "send back");
    p.deliver();
    CHECK(p.a.recv(0, buf, 3) == 3 && std::memcmp(buf, "wor", 3) == 0, "partial recv");
    CHECK(p.a.recv(0, buf, 16) == 2 && std::memcmp(buf, "ld", 2) == 0, "rest");
}

static void refused_without_listener() {
    Pair p;
    CHECK(p.a.connect(0, kVipB, 4000) == -kEINPROGRESS, "connect");
    p.deliver();
    CHECK(p.a.connect(0, kVipB, 4000) == -kECONNREFUSED, "no listener: refused");
}

static void close_gives_eof_and_reset() {
    Pair p;
    p.b.listen(1, 3700);
    p.a.connect(0, kVipB, 3700);
    p.deliver();
    uint32_t vip;
    uint16_t port;
    CHECK(p.b.accept(1, 9, &vip, &port) == 0, "accepted");
    p.a.send(0, reinterpret_cast<const uint8_t*>("bye"), 3);
    p.a.close(0);
    p.deliver();
    uint8_t buf[8];
    CHECK(p.b.recv(9, buf, 8) == 3, "data sent before the close arrives first");
    CHECK(p.b.recv(9, buf, 8) == 0, "then end of stream");
    CHECK(p.b.send(9, buf, 1) == 1, "the other side may still send");
    p.deliver();  // A has no such connection any more: RST back
    p.deliver();
    CHECK(p.b.recv(9, buf, 8) == -kECONNRESET || p.b.recv(9, buf, 8) == 0, "after the RST: reset or eof");
    CHECK(p.b.send(9, buf, 1) == -kECONNRESET, "send after reset");
}

static void big_payload_in_order() {
    Pair p;
    p.b.listen(1, 3700);
    p.a.connect(0, kVipB, 3700);
    p.deliver();
    uint32_t vip;
    uint16_t port;
    p.b.accept(1, 9, &vip, &port);
    std::string big;
    for (int i = 0; i < 5000; i++) big.push_back(static_cast<char>('a' + i % 26));
    CHECK(p.a.send(0, reinterpret_cast<const uint8_t*>(big.data()), big.size()) == 5000, "big send accepted");
    CHECK(p.deliver() >= 5, "split into segments");
    CHECK(recv_all(p.b, 9) == big, "same bytes, same order");
}

static void two_connections_one_listener() {
    Pair p;
    p.b.listen(1, 3700);
    p.a.connect(0, kVipB, 3700);
    p.a.connect(2, kVipB, 3700);
    p.deliver();
    CHECK(p.a.connect(0, kVipB, 3700) == 0 && p.a.connect(2, kVipB, 3700) == 0, "both connected");
    CHECK(p.a.local_port(0) != p.a.local_port(2), "distinct local ports");
    uint32_t vip;
    uint16_t pa, pb;
    CHECK(p.b.accept(1, 7, &vip, &pa) == 0 && p.b.accept(1, 8, &vip, &pb) == 0 && pa != pb, "two accepts");
    p.a.send(2, reinterpret_cast<const uint8_t*>("two"), 3);
    p.a.send(0, reinterpret_cast<const uint8_t*>("one"), 3);
    p.deliver();
    const int one = pa == p.a.local_port(0) ? 7 : 8, two = one == 7 ? 8 : 7;
    CHECK(recv_all(p.b, one) == "one" && recv_all(p.b, two) == "two", "data kept apart per connection");
    p.b.close(1);  // closing the listener leaves accepted connections alone
    CHECK(p.b.send(7, reinterpret_cast<const uint8_t*>("k"), 1) == 1, "accepted socket still usable");
}

static void idle_until_read() {
    Pair p;
    CHECK(p.b.idle(), "nothing buffered");
    p.b.listen(1, 3700);
    p.a.connect(0, kVipB, 3700);
    p.deliver();
    uint32_t vip;
    uint16_t port;
    p.b.accept(1, 9, &vip, &port);
    p.a.send(0, reinterpret_cast<const uint8_t*>("bye"), 3);
    p.deliver();
    CHECK(!p.b.idle(), "unread bytes: not idle");
    uint8_t buf[8];
    p.b.recv(9, buf, 8);
    CHECK(p.b.idle(), "read: idle again");
    p.a.connect(2, kVipB, 3700);
    p.deliver();
    CHECK(p.b.idle(), "an empty connection in the backlog is idle");
    p.a.send(2, reinterpret_cast<const uint8_t*>("x"), 1);
    p.deliver();
    CHECK(!p.b.idle(), "data for a connection not accepted yet is unread data");
}

static void rx_cap_resets_connection() {
    Pair p;
    p.b.listen(1, 3700);
    p.a.connect(0, kVipB, 3700);
    p.deliver();
    uint32_t vip;
    uint16_t port;
    p.b.accept(1, 9, &vip, &port);
    std::vector<uint8_t> chunk(64 * 1024, 7);
    for (size_t sent = 0; sent < p2p::kMaxUnreadBytes + 2 * chunk.size(); sent += chunk.size()) {
        p.a.send(0, chunk.data(), chunk.size());
        p.deliver();
    }
    uint8_t b[4];
    CHECK(p.b.idle(), "unread bytes over the cap are dropped, not kept");
    CHECK(p.b.recv(9, b, 4) == -kECONNRESET, "the flooded connection is reset");
    p.deliver();
    CHECK(p.a.send(0, b, 1) == -kECONNRESET, "and the sender learns it");
}

static void backlog_cap() {
    Pair p;
    p.b.listen(1, 3700);
    int refused = 0, ok = 0;
    for (int i = 0; i < static_cast<int>(p2p::kMaxBacklog) + 4; i++) p.a.connect(10 + i, kVipB, 3700);
    p.deliver();
    for (int i = 0; i < static_cast<int>(p2p::kMaxBacklog) + 4; i++) {
        const int r = p.a.connect(10 + i, kVipB, 3700);
        ok += r == 0;
        refused += r == -kECONNREFUSED;
    }
    CHECK(ok == static_cast<int>(p2p::kMaxBacklog) && refused == 4, "connections past the backlog are refused");
}

static void unknown_socket() {
    Pair p;
    uint8_t b[1];
    CHECK(p.a.recv(5, b, 1) == -kENOTCONN && p.a.send(5, b, 1) == -kENOTCONN, "not a stream socket");
    CHECK(p.a.listen(1, 3700) == 0 && p.a.listen(2, 3700) == -kEADDRINUSE, "one listener per port");
}

int main() {
    connect_accept_data();
    refused_without_listener();
    close_gives_eof_and_reset();
    big_payload_in_order();
    two_connections_one_listener();
    idle_until_read();
    rx_cap_resets_connection();
    backlog_cap();
    unknown_socket();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("p2p_stream: all tests passed\n");
    return 0;
}
