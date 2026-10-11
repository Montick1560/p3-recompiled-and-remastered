// ptp: ad hoc stream sockets between two "instances" in one process bound to
// 127.0.0.1 and 127.0.0.2.
#include "net/ptp.h"
#include "psp_socket_compat.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static const pro::Mac kA = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0a};
static const pro::Mac kB = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0b};
static constexpr uint32_t ip_of(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return a | b << 8 | c << 16 | d << 24;
}
static constexpr uint16_t kOffset = 22000;

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

static void connect_accept_exchange() {
    ptp::Sockets a, b;
    a.set_resolver(resolver("127.0.0.1", kB, ip_of(127, 0, 0, 2)));
    b.set_resolver(resolver("127.0.0.2", kA, ip_of(127, 0, 0, 1)));
    const int lb = b.listen(400, 8192, 4);
    CHECK(lb >= 1 && b.state(lb) == ptp::LISTEN, "listen");
    const int ca = a.open(401, kB, 400, 8192);
    CHECK(ca >= 1 && ca != lb, "open (ids shared with PDP/listen sockets)");
    CHECK(a.connect(ca, 2000000, false) == 0 && a.state(ca) == ptp::ESTABLISHED, "blocking connect");
    pro::Mac peer{};
    uint16_t port = 0;
    const int nb = b.accept(lb, &peer, &port, 2000000, false);
    CHECK(nb >= 1 && peer == kA && port == 401, "accept gives the peer MAC and PSP port");
    const uint8_t msg[3] = {'a', 'b', 'c'};
    int len = 3;
    CHECK(a.send(ca, msg, &len, 0, false) == 0 && len == 3, "send");
    uint8_t buf[16];
    len = sizeof(buf);
    CHECK(b.recv(nb, buf, &len, 1000000, false) == 0 && len == 3 && std::memcmp(buf, msg, 3) == 0, "recv");
    len = sizeof(buf);
    CHECK(b.recv(nb, buf, &len, 0, true) == ptp::kErrWouldBlock, "nothing more: WOULD_BLOCK");
    const auto t0 = std::chrono::steady_clock::now();
    len = sizeof(buf);
    CHECK(b.recv(nb, buf, &len, 200000, false) == ptp::kErrTimeout && ms_since(t0) >= 150, "recv timeout");
    CHECK(a.flush(ca) == 0, "flush");
    CHECK(b.close(nb) == 0, "close accepted socket");
    len = sizeof(buf);
    CHECK(a.recv(ca, buf, &len, 1000000, false) == ptp::kErrDisconnected, "peer closed: DISCONNECTED");
    CHECK(a.close(ca) == 0 && b.close(lb) == 0, "close the rest");
    CHECK(a.close(ca) == ptp::kErrInvalidSocketId, "close twice");
}

static void connect_waits_for_listener() {
    ptp::Sockets a, b;
    a.set_resolver(resolver("127.0.0.1", kB, ip_of(127, 0, 0, 2)));
    b.set_resolver(resolver("127.0.0.2", kA, ip_of(127, 0, 0, 1)));
    const int ca = a.open(0, kB, 410, 8192);
    CHECK(a.connect(ca, 0, true) == ptp::kErrWouldBlock || a.state(ca) == ptp::SYN_SENT ||
              a.state(ca) == ptp::CLOSED, "nonblocking connect with no listener does not succeed");
    auto t0 = std::chrono::steady_clock::now();
    CHECK(a.connect(ca, 300000, false) == ptp::kErrTimeout, "blocking connect times out (refused is retried)");
    CHECK(ms_since(t0) >= 250, "after the timeout");
    std::thread later([&b] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        b.listen(410, 8192, 4);
    });
    t0 = std::chrono::steady_clock::now();
    CHECK(a.connect(ca, 3000000, false) == 0, "connect succeeds once the peer listens");
    later.join();
    CHECK(ms_since(t0) < 2500, "soon after the listener appears");
    a.close_all();
    b.close_all();
}

static void errors() {
    ptp::Sockets a;
    a.set_resolver(resolver("127.0.0.1", kB, ip_of(127, 0, 0, 2)));
    const pro::Mac stranger = {0x02, 9, 9, 9, 9, 9};
    const int s = a.open(420, stranger, 400, 8192);
    CHECK(a.connect(s, 100000, false) == ptp::kErrInvalidAddr, "unknown peer MAC");
    uint8_t buf[4] = {};
    int len = 4;
    CHECK(a.send(s, buf, &len, 0, true) == ptp::kErrNotConnected, "send before connect");
    const int l = a.listen(421, 8192, 1);
    pro::Mac peer{};
    uint16_t port = 0;
    CHECK(a.accept(l, &peer, &port, 0, true) == ptp::kErrWouldBlock, "accept nonblocking, nobody");
    CHECK(a.accept(s, &peer, &port, 0, true) == ptp::kErrNotListened, "accept on a connect socket");
    CHECK(a.connect(l, 0, true) == ptp::kErrNotOpened, "connect on a listen socket");
    CHECK(a.listen(421, 8192, 1) == ptp::kErrPortInUse, "listen twice on a port");
    a.close_all();
}

int main() {
    psp_sock_startup();
    connect_accept_exchange();
    connect_waits_for_listener();
    errors();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ptp: all passed\n");
    return 0;
}
