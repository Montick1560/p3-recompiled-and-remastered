// ProClient against an in-process fake PRO server on 127.0.0.1.
#include "net/pro_client.h"
#include "psp_socket_compat.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static const pro::Mac kMac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0a};
static const pro::Mac kPeerMac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0b};

// One-connection fake server: listen() on an ephemeral port, accept() later.
struct FakeServer {
    int listen_fd = -1;
    int client = -1;
    uint16_t port = 0;

    FakeServer() {
        listen_fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(listen_fd, 1);
        socklen_t len = sizeof(a);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
    }
    ~FakeServer() {
        if (client >= 0) psp_sock_close(client);
        psp_sock_close(listen_fd);
    }
    void accept_one() { client = static_cast<int>(::accept(listen_fd, nullptr, nullptr)); }
    bool read_exact(uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            int r = ::recv(client, reinterpret_cast<char*>(buf + got), static_cast<int>(n - got), 0);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }
    void write(const std::vector<uint8_t>& p) {
        ::send(client, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0);
    }
    void close_client() {
        psp_sock_close(client);
        client = -1;
    }
};

static std::vector<uint8_t> s2c_connect(const pro::Mac& mac, const char* nick, uint8_t last_ip) {
    std::vector<uint8_t> p(139, 0);
    p[0] = pro::CONNECT;
    std::memcpy(&p[1], nick, std::strlen(nick));
    std::memcpy(&p[129], mac.data(), 6);
    p[135] = 10;
    p[138] = last_ip;
    return p;
}

static void login_sent() {
    FakeServer srv;
    ProClient c;
    std::thread t([&srv] { srv.accept_one(); });
    const bool ok = c.start("127.0.0.1", srv.port, kMac, "Hero", "UCES01421");
    t.join();
    CHECK(ok && c.connected(), "start connects");
    uint8_t login[144];
    CHECK(srv.read_exact(login, sizeof(login)), "server reads 144 bytes");
    CHECK(login[0] == pro::LOGIN && std::memcmp(&login[1], kMac.data(), 6) == 0, "login carries our MAC");
    CHECK(std::memcmp(&login[135], "UCES01421", 9) == 0, "login carries the product code");
    c.send_connect("ROOM");
    uint8_t conn[9];
    CHECK(srv.read_exact(conn, 9) && conn[0] == pro::CONNECT && std::memcmp(&conn[1], "ROOM", 4) == 0,
          "connect packet");
    c.stop();
}

static void peer_joined_and_left() {
    FakeServer srv;
    ProClient c;
    std::thread t([&srv] { srv.accept_one(); });
    c.start("127.0.0.1", srv.port, kMac, "Hero", "UCES01421");
    t.join();
    uint8_t login[144];
    srv.read_exact(login, sizeof(login));
    srv.write(s2c_connect(kPeerMac, "Peer", 7));
    pro::Event e;
    CHECK(c.wait_event(&e, 1000) && e.op == pro::CONNECT && e.nick == "Peer", "peer joined event");
    auto peers = c.peers();
    CHECK(peers.size() == 1 && peers[0].mac == kPeerMac && peers[0].ip == (10u | 7u << 24),
          "peer table has the peer");
    srv.write(s2c_connect(kPeerMac, "Peer", 7));  // duplicate notice
    c.wait_event(&e, 1000);
    CHECK(c.peers().size() == 1, "duplicate MAC replaces, not appends");
    srv.write({pro::DISCONNECT, 10, 0, 0, 7});
    CHECK(c.wait_event(&e, 1000) && e.op == pro::DISCONNECT && e.ip == (10u | 7u << 24), "peer left event");
    CHECK(c.peers().empty(), "peer removed");
    c.stop();
}

static void server_gone() {
    FakeServer srv;
    ProClient c;
    std::thread t([&srv] { srv.accept_one(); });
    c.start("127.0.0.1", srv.port, kMac, "Hero", "UCES01421");
    t.join();
    srv.close_client();
    pro::Event e;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = c.wait_event(&e, 2000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    CHECK(got && e.op == pro::DISCONNECT && e.ip == 0, "server loss reported as DISCONNECT ip 0");
    CHECK(ms < 1000, "reported within 1 s");
    CHECK(!c.connected(), "not connected after loss");
    CHECK(!c.send_scan(), "sends fail after loss");
    c.stop();
    c.stop();  // idempotent
}

static void unreachable_fails_fast() {
    FakeServer srv;  // listening socket we close: connection refused
    const uint16_t port = srv.port;
    psp_sock_close(srv.listen_fd);
    srv.listen_fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    ProClient c;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = c.start("127.0.0.1", port, kMac, "Hero", "UCES01421", "", 1500);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    CHECK(!ok && !c.connected(), "refused connection fails");
    CHECK(ms <= 2500, "within the connect timeout");
    CHECK(!c.start("no.such.host.invalid", port, kMac, "Hero", "UCES01421", "", 500),
          "unresolvable host fails");
}

int main() {
    psp_sock_startup();
    login_sent();
    peer_joined_and_left();
    server_gone();
    unreachable_fails_fast();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("pro_client: all passed\n");
    return 0;
}
