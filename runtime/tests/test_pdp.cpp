// pdp: ad hoc datagram sockets between two "instances" in one process, bound
// to 127.0.0.1 and 127.0.0.2 like two copies of the port on one PC.
#include "net/pdp.h"
#include "psp_socket_compat.h"

#include <chrono>
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static const pro::Mac kA = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0a};
static const pro::Mac kB = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0b};
static const pro::Mac kBroadcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static constexpr uint32_t ip_of(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return a | b << 8 | c << 16 | d << 24;
}
static const uint32_t kIpA = ip_of(127, 0, 0, 1);
static const uint32_t kIpB = ip_of(127, 0, 0, 2);
static constexpr uint16_t kOffset = 21000;

// Instance A sees B as its peer and vice versa.
static pdp::Resolver resolver(const char* bind, const pro::Mac& peer, uint32_t peer_ip) {
    pdp::Resolver r;
    r.peers = [peer, peer_ip] { return std::vector<pdp::Peer>{{peer, peer_ip}}; };
    r.bind_ip = bind;
    r.port_offset = kOffset;
    return r;
}

static int64_t ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static void send_and_receive() {
    pdp::Sockets a, b;
    a.set_resolver(resolver("127.0.0.1", kB, kIpB));
    b.set_resolver(resolver("127.0.0.2", kA, kIpA));
    const int sa = a.create(300, 8192), sb = b.create(300, 8192);
    CHECK(sa >= 1 && sb >= 1, "create on the same PSP port at two addresses");
    const uint8_t msg[5] = {'h', 'e', 'l', 'l', 'o'};
    CHECK(a.send(sa, kB, 300, msg, 5) == 0, "send");
    pro::Mac from{};
    uint16_t port = 0;
    uint8_t buf[64];
    int len = sizeof(buf);
    CHECK(b.recv(sb, &from, &port, buf, &len, 500000, false) == 0, "recv");
    CHECK(len == 5 && std::memcmp(buf, msg, 5) == 0, "payload");
    CHECK(from == kA && port == 300, "sender MAC and PSP port");
    CHECK(a.send(sa, kBroadcast, 300, msg, 3) == 0, "broadcast");
    len = sizeof(buf);
    CHECK(b.recv(sb, &from, &port, buf, &len, 500000, false) == 0 && len == 3, "broadcast reaches peers");
    const pro::Mac stranger = {0x02, 1, 2, 3, 4, 5};
    CHECK(a.send(sa, stranger, 300, msg, 5) == 0, "unknown MAC: faked success");
}

static void two_bind_addresses() {
    pdp::Sockets a, b;
    a.set_resolver(resolver("127.0.0.1", kB, kIpB));
    b.set_resolver(resolver("127.0.0.2", kA, kIpA));
    const int sa = a.create(301, 8192), sb = b.create(301, 8192);
    const uint8_t x = 0x42;
    b.send(sb, kA, 301, &x, 1);
    pro::Mac from{};
    uint16_t port = 0;
    uint8_t buf[8];
    int len = sizeof(buf);
    CHECK(a.recv(sa, &from, &port, buf, &len, 500000, false) == 0 && from == kB && buf[0] == 0x42,
          "B -> A arrives at A's address");
    len = sizeof(buf);
    CHECK(b.recv(sb, &from, &port, buf, &len, 0, true) == pdp::kErrWouldBlock, "nothing echoed to B");
}

static void unknown_source_dropped() {
    pdp::Sockets b;
    b.set_resolver(resolver("127.0.0.2", kA, kIpA));
    const int sb = b.create(302, 8192);
    psp_sock_startup();
    const int raw = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    sockaddr_in src{};
    src.sin_family = AF_INET;
    ::inet_pton(AF_INET, "127.0.0.3", &src.sin_addr);
    ::bind(raw, reinterpret_cast<sockaddr*>(&src), sizeof(src));
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    ::inet_pton(AF_INET, "127.0.0.2", &dst.sin_addr);
    dst.sin_port = htons(302 + kOffset);
    const char junk[4] = {1, 2, 3, 4};
    ::sendto(raw, junk, 4, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    psp_sock_close(raw);
    pro::Mac from{};
    uint16_t port = 0;
    uint8_t buf[8];
    int len = sizeof(buf);
    const int rc = b.recv(sb, &from, &port, buf, &len, 200000, false);
    CHECK(rc == pdp::kErrTimeout, "datagram from a non-peer is dropped");
}

static void recv_timeouts() {
    pdp::Sockets a;
    a.set_resolver(resolver("127.0.0.1", kB, kIpB));
    const int s = a.create(303, 8192);
    pro::Mac from{};
    uint16_t port = 0;
    uint8_t buf[8];
    int len = sizeof(buf);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(a.recv(s, &from, &port, buf, &len, 0, true) == pdp::kErrWouldBlock, "nonblock: WOULD_BLOCK");
    CHECK(ms_since(t0) < 50, "at once");
    t0 = std::chrono::steady_clock::now();
    CHECK(a.recv(s, &from, &port, buf, &len, 200000, false) == pdp::kErrTimeout, "blocking: TIMEOUT");
    const auto ms = ms_since(t0);
    CHECK(ms >= 150 && ms <= 600, "after about the timeout");
    CHECK(a.recv(77, &from, &port, buf, &len, 0, true) == pdp::kErrInvalidSocketId, "bad id");
}

static void port_in_use_and_buffers() {
    pdp::Sockets a, b;
    a.set_resolver(resolver("127.0.0.1", kB, kIpB));
    b.set_resolver(resolver("127.0.0.2", kA, kIpA));
    const int s1 = a.create(304, 8192);
    CHECK(a.create(304, 8192) == pdp::kErrPortInUse, "same PSP port twice");
    CHECK(a.create(305, 0) == pdp::kErrInvalidArg, "zero buffer");
    const int s0 = a.create(0, 8192);
    CHECK(s0 >= 1 && a.local_port(s0) != 0, "port 0 gets a free port");
    const int sb = b.create(304, 8192);
    uint8_t big[100];
    std::memset(big, 7, sizeof(big));
    b.send(sb, kA, 304, big, 100);
    pro::Mac from{};
    uint16_t port = 0;
    uint8_t small[10];
    int len = sizeof(small);
    CHECK(a.recv(s1, &from, &port, small, &len, 500000, false) == pdp::kErrNotEnoughSpace && len == 100,
          "too small: NOT_ENOUGH_SPACE with the needed size");
    uint8_t buf[128];
    len = sizeof(buf);
    CHECK(a.recv(s1, &from, &port, buf, &len, 0, true) == 0 && len == 100, "datagram kept for a bigger read");
    CHECK(a.send(s1, kB, 0, buf, 1) == pdp::kErrInvalidPort, "port 0 send");
    CHECK(a.send(s1, kB, 304, buf, -1) == pdp::kErrInvalidDatalen, "negative length");
    CHECK(a.remove(s1) == 0 && a.remove(s1) == pdp::kErrInvalidSocketId, "remove once");
    CHECK(a.create(304, 8192) >= 1, "port free again after remove");
}

int main() {
    psp_sock_startup();
    send_and_receive();
    two_bind_addresses();
    unknown_source_dropped();
    recv_timeouts();
    port_in_use_and_buffers();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("pdp: all passed\n");
    return 0;
}
