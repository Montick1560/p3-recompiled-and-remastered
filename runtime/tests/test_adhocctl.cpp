// adhocctl core against a fake PRO server: events, states, handler registry.
#include "net/adhocctl.h"
#include "fake_pro_server.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static const pro::Mac kMe = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0a};
static const pro::Mac kPeer = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0b};

struct Recorder {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<int, int>> events;  // (event, error)
    std::vector<uint32_t> args;

    adhocctl::Adhocctl::Notifier notifier() {
        return [this](const adhocctl::Handler& h, int event, int error) {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back({event, error});
            args.push_back(h.arg);
            cv.notify_all();
        };
    }
    bool wait_for(int event, int ms) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] {
            for (auto& e : events) if (e.first == event) return true;
            return false;
        });
    }
    size_t count(int event) {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (auto& e : events) n += e.first == event;
        return n;
    }
};

static AdhocConfig config_for(const FakeProServer& srv) {
    AdhocConfig c;
    c.server_port = srv.port;
    c.mac = kMe;
    c.nickname = "Hero";
    return c;
}

static void init_and_login(adhocctl::Adhocctl& a, FakeProServer& srv) {
    std::thread t([&srv] { srv.accept_one(); });
    a.init(config_for(srv), "UCES01421");
    t.join();
    srv.expect(pro::LOGIN);
}

static void connect_reaches_connected() {
    FakeProServer srv;
    adhocctl::Adhocctl a;
    Recorder rec;
    a.set_notifier(rec.notifier());
    init_and_login(a, srv);
    CHECK(a.add_handler(0x08800000, 0x1234, true) == 0, "first handler id 0");
    CHECK(a.connect("ROOM", adhocctl::Mode::Create) == 0, "create returns 0");
    std::vector<uint8_t> body;
    CHECK(srv.expect(pro::CONNECT, &body) && std::memcmp(body.data(), "ROOM", 4) == 0,
          "server receives CONNECT ROOM");
    srv.write(FakeProServer::s2c_bssid(kMe));
    CHECK(rec.wait_for(adhocctl::EVENT_CONNECT, 1000), "handler gets EVENT_CONNECT");
    CHECK(a.state() == adhocctl::STATE_CONNECTED, "state connected");
    CHECK(a.group() == "ROOM", "group kept");
    CHECK(rec.args.size() >= 1 && rec.args[0] == 0x1234, "handler arg passed");
    CHECK(a.connect("ROOM", adhocctl::Mode::Create) == 0, "create while connected returns 0");
    CHECK(rec.wait_for(adhocctl::EVENT_ERROR, 1000), "and fires ERROR (already connected)");
    a.term();
}

static void join_waits_for_a_peer() {
    FakeProServer srv;
    adhocctl::Adhocctl a;
    Recorder rec;
    a.set_notifier(rec.notifier());
    init_and_login(a, srv);
    a.add_handler(0x08800000, 0, true);
    a.connect("ROOM", adhocctl::Mode::Join);
    srv.expect(pro::CONNECT);
    srv.write(FakeProServer::s2c_bssid(kPeer));
    CHECK(!rec.wait_for(adhocctl::EVENT_CONNECT, 300), "join: no CONNECT before a peer is known");
    srv.write(FakeProServer::s2c_connect(kPeer, "Host", 2));
    CHECK(rec.wait_for(adhocctl::EVENT_CONNECT, 1000), "join: CONNECT once the host is known");
    CHECK(a.peers().size() == 1, "peer listed");
    a.term();
}

static void disconnect_event() {
    FakeProServer srv;
    adhocctl::Adhocctl a;
    Recorder rec;
    a.set_notifier(rec.notifier());
    init_and_login(a, srv);
    a.add_handler(0x08800000, 0, true);
    CHECK(a.disconnect() == 0, "disconnect while disconnected returns 0");
    CHECK(rec.wait_for(adhocctl::EVENT_DISCONNECT, 1000), "and still notifies DISCONNECT (PPSSPP)");
    a.connect("ROOM", adhocctl::Mode::Connect);
    srv.expect(pro::CONNECT);
    srv.write(FakeProServer::s2c_bssid(kMe));
    rec.wait_for(adhocctl::EVENT_CONNECT, 1000);
    a.disconnect();
    CHECK(srv.expect(pro::DISCONNECT), "server receives DISCONNECT only when in a group");
    CHECK(a.state() == adhocctl::STATE_DISCONNECTED && a.peers().empty(), "back to disconnected");
    a.term();
}

static void scan_lists_groups() {
    FakeProServer srv;
    adhocctl::Adhocctl a;
    Recorder rec;
    a.set_notifier(rec.notifier());
    init_and_login(a, srv);
    a.add_handler(0x08800000, 0, true);
    CHECK(a.scan() == 0 && a.state() == adhocctl::STATE_SCANNING, "scan starts");
    srv.expect(pro::SCAN);
    std::vector<uint8_t> scan(15, 0);
    scan[0] = pro::SCAN;
    std::memcpy(&scan[1], "ROOM", 4);
    std::memcpy(&scan[9], kPeer.data(), 6);
    srv.write(scan);
    srv.write({pro::SCAN_COMPLETE});
    CHECK(rec.wait_for(adhocctl::EVENT_SCAN, 1000), "EVENT_SCAN on completion");
    auto r = a.scan_results();
    CHECK(r.size() == 1 && r[0].group == "ROOM" && r[0].host == kPeer, "scan result kept");
    CHECK(a.state() == adhocctl::STATE_DISCONNECTED, "scan ends disconnected");
    a.term();
}

static void init_twice_and_handlers() {
    FakeProServer srv;
    adhocctl::Adhocctl a;
    init_and_login(a, srv);
    CHECK(a.init(config_for(srv), "UCES01421") == adhocctl::kErrAlreadyInitialized, "init twice");
    CHECK(a.add_handler(0x08800000, 1, true) == 0, "id 0");
    CHECK(a.add_handler(0x08800000, 2, true) == 0, "same entry: 0, not added");
    CHECK(a.add_handler(0x08800100, 3, true) == 1, "id 1");
    CHECK(a.add_handler(0, 0, false) == adhocctl::kErrInvalidArg, "invalid entry");
    CHECK(a.del_handler(0) == 0 && a.add_handler(0x08800200, 4, true) == 0, "freed id reused");
    a.term();
    CHECK(!a.inited() && a.del_handler(0) == adhocctl::kErrNotInitialized, "term clears");
    CHECK(a.connect("ROOM", adhocctl::Mode::Create) == adhocctl::kErrNotInitialized, "connect needs init");
}

static void server_unreachable_still_inits() {
    FakeProServer srv;
    const uint16_t port = srv.port;
    psp_sock_close(srv.listen_fd);
    srv.listen_fd = -1;
    adhocctl::Adhocctl a;
    AdhocConfig c;
    c.server_port = port;
    c.mac = kMe;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(a.init(c, "UCES01421") == 0 && a.inited(), "init succeeds without a server");
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4), "and returns promptly");
    CHECK(!a.server_connected(), "not connected");
    CHECK(a.connect("ROOM", adhocctl::Mode::Create) == 0, "connect accepted (never completes)");
    CHECK(a.group() == "ROOM", "group recorded");
    CHECK(a.connect("bad name!", adhocctl::Mode::Create) == adhocctl::kAdhocErrInvalidArg ||
              a.state() != adhocctl::STATE_CONNECTED,
          "invalid group rejected");
    a.term();
}


static void resolve_peers() {
    FakeProServer srv;
    adhocctl::Adhocctl a;
    Recorder rec;
    a.set_notifier(rec.notifier());
    init_and_login(a, srv);
    a.connect("ROOM", adhocctl::Mode::Connect);
    srv.expect(pro::CONNECT);
    srv.write(FakeProServer::s2c_connect(kPeer, "Peer", 2));  // 10.0.0.2
    srv.write(FakeProServer::s2c_bssid(kPeer));
    rec.wait_for(adhocctl::EVENT_CONNECT, 1000);
    uint32_t ip = 0;
    pro::Mac mac{};
    CHECK(a.resolve_mac(kPeer, &ip) && ip == (10u | 2u << 24), "peer MAC -> IP");
    CHECK(a.resolve_ip(10u | 2u << 24, &mac) && mac == kPeer, "peer IP -> MAC");
    CHECK(!a.resolve_mac(kMe, &ip) && !a.resolve_ip(10u | 9u << 24, &mac), "unknown fail");
    CHECK(a.local_mac() == kMe && a.port_offset() == 10000, "local identity");
    a.term();
}


static void reconnects_on_connect() {
    // PPSSPP sets isAdhocctlNeedLogin on create/connect: a dropped server
    // connection is logged in again before the group request.
    FakeProServer srv;
    adhocctl::Adhocctl a;
    Recorder rec;
    a.set_notifier(rec.notifier());
    init_and_login(a, srv);
    a.add_handler(0x08800000, 0, true);
    srv.close_client();
    for (int i = 0; i < 50 && a.server_connected(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!a.server_connected(), "server connection lost");
    std::thread t([&srv] { srv.accept_one(); });
    CHECK(a.connect("ROOM", adhocctl::Mode::Connect) == 0, "connect after the loss");
    t.join();
    CHECK(srv.expect(pro::LOGIN) && srv.expect(pro::CONNECT), "logged in again, then CONNECT");
    srv.write(FakeProServer::s2c_bssid(kMe));
    CHECK(rec.wait_for(adhocctl::EVENT_CONNECT, 1000), "and the group completes");
    a.term();
}

int main() {
    connect_reaches_connected();
    join_waits_for_a_peer();
    disconnect_event();
    scan_lists_groups();
    init_twice_and_handlers();
    server_unreachable_still_inits();
    resolve_peers();
    reconnects_on_connect();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("adhocctl: all passed\n");
    return 0;
}
