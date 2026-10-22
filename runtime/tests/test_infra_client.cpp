// infra_client: address forms, login, request/answer matching, timeouts,
// server loss, datagram queues and ping RTT, against an in-process fake of
// tools/infra_server.py speaking the net/infra_protocol frames over TCP.
#include "net/infra_client.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "net/infra_protocol.h"
#include "psp_socket_compat.h"

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

using Clock = std::chrono::steady_clock;
static long ms_since(Clock::time_point t) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// One-connection fake server. Answers login and ping by itself; every other
// frame goes to on_frame (on the server thread).
struct FakeInfraServer {
    int listen_fd = -1, client = -1;
    uint16_t port = 0;
    std::thread th;
    std::atomic<bool> stop{false};
    std::function<void(FakeInfraServer&, const infra::Frame&)> on_frame;
    int ping_delay_ms = 0;
    bool answer_pings = true;
    uint32_t vip = 0x0A500003;
    std::mutex mu;
    std::vector<infra::Frame> got;

    FakeInfraServer() {
        psp_sock_startup();
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
    ~FakeInfraServer() {
        stop = true;
        close_client();
        if (listen_fd >= 0) psp_sock_close(listen_fd);
        if (th.joinable()) th.join();
    }
    infra::Address address() const {
        infra::Address a;
        a.kind = infra::Address::Tcp;
        a.host = "127.0.0.1";
        a.port = port;
        return a;
    }
    void start() {
        th = std::thread([this] {
            client = static_cast<int>(::accept(listen_fd, nullptr, nullptr));
            infra::FrameReader r;
            uint8_t buf[8192];
            while (!stop && client >= 0) {
                const int n = ::recv(client, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n <= 0) break;
                r.feed(buf, static_cast<size_t>(n));
                infra::Frame f;
                while (r.next(&f)) {
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        got.push_back(f);
                    }
                    const std::string op = f.kind == infra::kKindJson ? f.json.value("op", "") : "";
                    if (op == "login") {
                        send_json({{"id", f.json["id"]}, {"ok", true}, {"session", "s1"}, {"vip", vip}});
                    } else if (op == "ping") {
                        if (!answer_pings) continue;
                        if (ping_delay_ms) sleep_ms(ping_delay_ms);
                        send_json({{"id", f.json["id"]}, {"ok", true}, {"t", f.json["t"]}});
                    } else if (on_frame) {
                        on_frame(*this, f);
                    }
                }
            }
        });
    }
    void send_raw(const std::vector<uint8_t>& v) {
        if (client >= 0) ::send(client, reinterpret_cast<const char*>(v.data()), static_cast<int>(v.size()), 0);
    }
    void send_json(const nlohmann::json& j) { send_raw(infra::encode_json(j)); }
    void close_client() {
        if (client >= 0) {
            ::shutdown(client, SHUT_RDWR);
            psp_sock_close(client);
            client = -1;
        }
    }
};

static void parse_address_forms() {
    using A = infra::Address;
    A a = infra::parse_address(nullptr);
    CHECK(a.kind == A::Wss && a.host == "online.montick.app" && a.port == 443 && a.path == "/infra",
          "unset -> the public server");
    CHECK(infra::parse_address("").kind == A::Wss, "empty -> the public server");
    CHECK(infra::parse_address("off").kind == A::Off, "off");
    a = infra::parse_address("1.2.3.4");
    CHECK(a.kind == A::Tcp && a.host == "1.2.3.4" && a.port == 27320, "bare host -> tcp 27320");
    a = infra::parse_address("host:9");
    CHECK(a.kind == A::Tcp && a.host == "host" && a.port == 9, "host:port");
    a = infra::parse_address("tcp://h");
    CHECK(a.kind == A::Tcp && a.host == "h" && a.port == 27320, "tcp://host");
    a = infra::parse_address("ws://127.0.0.1:27321/infra");
    CHECK(a.kind == A::Ws && a.host == "127.0.0.1" && a.port == 27321 && a.path == "/infra", "ws://");
    a = infra::parse_address("ws://h");
    CHECK(a.kind == A::Ws && a.port == 80 && a.path == "/infra", "ws default port and path");
    a = infra::parse_address("wss://online.montick.app/infra");
    CHECK(a.kind == A::Wss && a.host == "online.montick.app" && a.port == 443 && a.path == "/infra", "wss://");
    CHECK(infra::parse_address("host:99999").kind == A::Off, "bad port -> off");
    CHECK(infra::parse_address("ftp://h").kind == A::Off, "unknown scheme -> off");
}

static infra::Client::Options fast() {
    infra::Client::Options o;
    o.ping_interval_ms = 100;
    o.dead_after_ms = 3000;
    return o;
}

static void login_sets_vip() {
    FakeInfraServer s;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.configured(), "configured");
    CHECK(c.connect("P3A", 2000), "connect + login");
    CHECK(c.vip() == 0x0A500003 && c.session() == "s1", "vip and session from the login answer");
    CHECK(c.connected(), "connected");
    std::lock_guard<std::mutex> lock(s.mu);
    CHECK(!s.got.empty() && s.got[0].json["op"] == "login" && s.got[0].json["online_id"] == "P3A",
          "first frame is the login");

    infra::Client off(infra::parse_address("off"), fast());
    CHECK(!off.configured() && !off.connect("P3A", 100), "off: never connects");
}

static void request_answer_matched_by_id() {
    FakeInfraServer s;
    nlohmann::json first;
    s.on_frame = [&](FakeInfraServer& srv, const infra::Frame& f) {
        if (f.json["op"] == "a") {
            first = f.json;
            return;
        }
        // answer b, then the held a
        srv.send_json({{"id", f.json["id"]}, {"ok", true}, {"x", "b"}});
        srv.send_json({{"id", first["id"]}, {"ok", true}, {"x", "a"}});
    };
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000), "connect");
    std::mutex mu;
    std::string ra, rb;
    std::atomic<int> done{0};
    c.request({{"op", "a"}}, [&](const nlohmann::json& j) { std::lock_guard<std::mutex> l(mu); ra = j.value("x", "?"); done++; }, 2000);
    sleep_ms(50);
    c.request({{"op", "b"}}, [&](const nlohmann::json& j) { std::lock_guard<std::mutex> l(mu); rb = j.value("x", "?"); done++; }, 2000);
    const auto t = Clock::now();
    while (done < 2 && ms_since(t) < 2000) sleep_ms(10);
    std::lock_guard<std::mutex> l(mu);
    CHECK(done == 2 && ra == "a" && rb == "b", "answers matched by id when they come out of order");
}

static void request_times_out() {
    FakeInfraServer s;
    s.on_frame = [](FakeInfraServer&, const infra::Frame&) {};  // never answers
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000), "connect");
    std::atomic<int> calls{0};
    std::atomic<long> took{0};
    nlohmann::json ans;
    const auto t = Clock::now();
    c.request({{"op", "slow"}}, [&](const nlohmann::json& j) { ans = j; took = ms_since(t); calls++; }, 300);
    while (calls == 0 && ms_since(t) < 2000) sleep_ms(10);
    sleep_ms(200);
    CHECK(calls == 1, "done runs exactly once");
    CHECK(ans.value("ok", true) == false && ans.value("error", 0u) == infra::kErrTimeout, "timeout error");
    CHECK(took >= 300 && took <= 500, "within timeout + 200 ms");

    std::atomic<int> late{0};
    infra::Client never(s.address(), fast());  // not connected
    never.request({{"op", "x"}}, [&](const nlohmann::json& j) {
        if (j.value("error", 0u) == infra::kErrTimeout) late++;
    }, 300);
    CHECK(late == 1, "not connected: fails at once");
}

static void disconnect_fails_pending() {
    FakeInfraServer s;
    s.on_frame = [](FakeInfraServer& srv, const infra::Frame& f) {
        if (f.json["op"] == "join") {
            srv.send_json({{"id", f.json["id"]}, {"ok", true}, {"room", {{"id", 5}, {"me", 0x11}}}});
        } else if (f.json["op"] == "hang") {
            srv.close_client();
        }
    };
    s.start();
    infra::Client c(s.address(), fast());
    std::mutex mu;
    std::vector<nlohmann::json> events;
    c.on_event([&](const nlohmann::json& e) { std::lock_guard<std::mutex> l(mu); events.push_back(e); });
    CHECK(c.connect("P3A", 2000), "connect");
    std::atomic<int> joined{0};
    c.request({{"op", "join"}, {"room_id", 5}}, [&](const nlohmann::json& j) { if (j.value("ok", false)) joined++; }, 2000);
    const auto t = Clock::now();
    while (!joined && ms_since(t) < 2000) sleep_ms(10);
    CHECK(joined == 1, "joined");
    std::atomic<int> failed{0};
    c.request({{"op", "hang"}}, [&](const nlohmann::json& j) {
        if (!j.value("ok", true) && j.value("error", 0u) == infra::kErrTimeout) failed++;
    }, 5000);
    const auto t2 = Clock::now();
    while (!failed && ms_since(t2) < 2000) sleep_ms(10);
    CHECK(failed == 1 && ms_since(t2) < 1500, "pending request fails when the server closes");
    sleep_ms(100);
    CHECK(!c.connected(), "not connected after the loss");
    std::lock_guard<std::mutex> l(mu);
    bool destroyed = false;
    for (const auto& e : events) destroyed |= e.value("ev", "") == "room_destroyed" && e.value("room_id", 0) == 5;
    CHECK(destroyed, "room_destroyed synthesised for the joined room");
}

static void datagram_queue_per_port() {
    FakeInfraServer s;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000), "connect");
    const uint8_t d1[2] = {1, 2}, d2[1] = {9};
    s.send_raw(infra::encode_datagram(0x0A500002, 3658, 4000, d1, 2));
    s.send_raw(infra::encode_datagram(0x0A500004, 3659, 4001, d2, 1));
    for (int i = 0; i < 300; i++) {
        const uint8_t b[2] = {static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)};
        s.send_raw(infra::encode_datagram(0x0A500002, 5000, 1, b, 2));
    }
    sleep_ms(300);
    infra::Client::Datagram g;
    CHECK(c.pop_datagram(3659, &g) && g.vip == 0x0A500004 && g.src_port == 4001 && g.payload.size() == 1 &&
              g.payload[0] == 9, "port 3659 gets its datagram");
    CHECK(c.pop_datagram(3658, &g) && g.vip == 0x0A500002 && g.payload == std::vector<uint8_t>({1, 2}),
          "port 3658 gets its datagram");
    CHECK(!c.pop_datagram(3658, &g), "then empty");
    int n = 0, first = -1;
    while (c.pop_datagram(5000, &g)) {
        if (first < 0) first = g.payload[0] | (g.payload[1] << 8);
        n++;
    }
    CHECK(n == 256 && first == 44, "queue capped at 256, oldest dropped");

    const uint8_t out[3] = {7, 8, 9};
    c.send_datagram(0x0A500002, 3658, 3659, out, 3);
    sleep_ms(200);
    std::lock_guard<std::mutex> lock(s.mu);
    bool seen = false;
    for (const auto& f : s.got)
        seen |= f.kind == infra::kKindDatagram && f.vip == 0x0A500002 && f.dst_port == 3658 && f.src_port == 3659 &&
                f.payload == std::vector<uint8_t>({7, 8, 9});
    CHECK(seen, "send_datagram reaches the server");
}

static void rtt_from_ping() {
    FakeInfraServer s;
    s.ping_delay_ms = 40;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.rtt_ms() == -1, "no RTT before connecting");
    CHECK(c.connect("P3A", 2000), "connect");
    const auto t = Clock::now();
    while (c.rtt_ms() < 0 && ms_since(t) < 2000) sleep_ms(10);
    sleep_ms(600);
    const int rtt = c.rtt_ms();
    CHECK(rtt >= 35 && rtt <= 200, "RTT from the ping answers");
}

static void silent_server_is_dead() {
    FakeInfraServer s;
    s.answer_pings = false;
    s.on_frame = [](FakeInfraServer&, const infra::Frame&) {};
    s.start();
    infra::Client::Options o = fast();
    o.dead_after_ms = 500;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    std::atomic<int> failed{0};
    c.request({{"op", "x"}}, [&](const nlohmann::json& j) { if (!j.value("ok", true)) failed++; }, 30000);
    const auto t = Clock::now();
    while (c.connected() && ms_since(t) < 3000) sleep_ms(20);
    CHECK(!c.connected() && ms_since(t) < 1500, "a server silent past dead_after_ms is dropped");
    while (failed == 0 && ms_since(t) < 3000) sleep_ms(10);  // lost() runs the callbacks after the flag
    CHECK(failed == 1, "its pending request fails");
}

// Optional: INFRA_WS_URL=ws://127.0.0.1:27321/infra with tools/infra_server.py
// running exercises the WebSocket transport against the real server.
static void ws_against_real_server() {
    const char* url = std::getenv("INFRA_WS_URL");
    if (!url) return;
    infra::Client c(infra::parse_address(url), fast());
    CHECK(c.connect("WSTEST", 3000), "ws connect + login");
    CHECK(c.vip() != 0, "ws vip");
    std::atomic<int> ok{0};
    c.request({{"op", "search"}, {"ints", nlohmann::json::array()}},
              [&](const nlohmann::json& j) { if (j.value("ok", false) && j.contains("total")) ok++; }, 3000);
    const auto t = Clock::now();
    while (!ok && ms_since(t) < 3000) sleep_ms(10);
    CHECK(ok == 1, "ws search answered");
    while (c.rtt_ms() < 0 && ms_since(t) < 3000) sleep_ms(10);
    CHECK(c.rtt_ms() >= 0, "ws ping RTT");
    std::printf("ws: vip %08X rtt %d ms\n", c.vip(), c.rtt_ms());
}

int main() {
    parse_address_forms();
    login_sets_vip();
    request_answer_matched_by_id();
    request_times_out();
    disconnect_fails_pending();
    datagram_queue_per_port();
    rtt_from_ping();
    silent_server_is_dead();
    ws_against_real_server();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("infra_client: all tests passed\n");
    return 0;
}
