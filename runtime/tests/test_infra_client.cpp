// infra_client: address forms, login, request/answer matching, timeouts,
// server loss, datagram queues and ping RTT, against an in-process fake of
// tools/infra_server.py speaking the net/infra_protocol frames over TCP.
#include "net/infra_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "fake_infra_server.h"
#include "net/direct_link.h"
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

static void login_carries_extra_fields() {
    FakeInfraServer s;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000, {{"console", "02000000000a"}}), "connect with extra login fields");
    std::lock_guard<std::mutex> lock(s.mu);
    CHECK(!s.got.empty() && s.got[0].json["op"] == "login" && s.got[0].json["online_id"] == "P3A" &&
              s.got[0].json.value("console", "") == "02000000000a",
          "login carries the console id");
}

static void login_fields_carry_proto_and_invite() {
    nlohmann::json f = infra::login_fields("02000000000a", "P3-K7QF-2MXD");
    CHECK(f.value("console", "") == "02000000000a" && f.value("proto", 0) == 3 && infra::kProto == 3,
          "console id and protocol 3");
    CHECK(f.value("invite", "") == "P3-K7QF-2MXD", "invite when given");
    CHECK(!infra::login_fields("02000000000a", "").contains("invite"), "no invite when empty");
    CHECK(!infra::login_fields("02000000000a", nullptr).contains("invite"), "no invite when unset");
    CHECK(infra::login_fields("02000000000a", nullptr).value("proto", 0) == 3, "proto always");
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

// The game is in its hideout room and a VS room: a lost connection destroys both, once each.
static void loss_destroys_every_room() {
    FakeInfraServer s;
    s.on_frame = [](FakeInfraServer& srv, const infra::Frame& f) {
        const std::string op = f.json.value("op", "");
        if (op == "join")
            srv.send_json({{"id", f.json["id"]}, {"ok", true}, {"room", {{"id", f.json["room_id"]}, {"me", 0x11}}}});
        else if (op == "create")
            srv.send_json({{"id", f.json["id"]}, {"ok", true}, {"room", {{"id", 8}, {"me", 0x10}}}});
        else if (op == "leave")
            srv.send_json({{"id", f.json["id"]}, {"ok", true}});
        else if (op == "hang")
            srv.close_client();
    };
    s.start();
    infra::Client c(s.address(), fast());
    std::mutex mu;
    std::vector<int64_t> destroyed;
    std::atomic<int> answers{0};
    c.on_event([&](const nlohmann::json& e) {
        std::lock_guard<std::mutex> l(mu);
        if (e.value("ev", "") == "room_destroyed") destroyed.push_back(e.value("room_id", int64_t{0}));
    });
    CHECK(c.connect("P3A", 2000), "connect");
    auto ok = [&](const nlohmann::json& j) { if (j.value("ok", false)) answers++; };
    c.request({{"op", "join"}, {"room_id", 5}}, ok, 2000);
    c.request({{"op", "create"}, {"max_slot", 4}}, ok, 2000);
    c.request({{"op", "join"}, {"room_id", 9}}, ok, 2000);
    c.request({{"op", "leave"}, {"room_id", 9}}, ok, 2000);
    const auto t = Clock::now();
    while (answers < 4 && ms_since(t) < 2000) sleep_ms(10);
    CHECK(answers == 4, "joined 5, created 8, joined and left 9");
    c.request({{"op", "hang"}}, [](const nlohmann::json&) {}, 2000);
    const auto t2 = Clock::now();
    while (c.connected() && ms_since(t2) < 2000) sleep_ms(10);
    sleep_ms(100);
    std::lock_guard<std::mutex> l(mu);
    std::sort(destroyed.begin(), destroyed.end());
    CHECK(destroyed == (std::vector<int64_t>{5, 8}), "one room_destroyed per room still joined, lost() once");
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

static void stream_segments_in_order() {
    FakeInfraServer s;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000), "connect");
    for (int i = 0; i < 600; i++) {
        const uint8_t b[3] = {3, static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)};
        s.send_raw(infra::encode_segment(infra::kKindStream, 0x0A500002, static_cast<uint16_t>(i % 2 ? 7000 : 7001), 9,
                                         b, 3));
    }
    sleep_ms(400);
    infra::Client::Datagram g;
    int n = 0;
    bool ordered = true;
    while (c.pop_stream(&g)) {
        ordered &= (g.payload[1] | (g.payload[2] << 8)) == n && g.vip == 0x0A500002 && g.src_port == 9 &&
                   g.dst_port == (n % 2 ? 7000 : 7001);
        n++;
    }
    CHECK(n == 600 && ordered, "every stream segment, in order, any port (no 256 cap)");
    CHECK(!c.pop_datagram(7000, &g), "segments are not datagrams");

    const uint8_t out[2] = {1, 0};
    c.send_stream(0x0A500002, 7000, 50000, out, 2);
    sleep_ms(200);
    std::lock_guard<std::mutex> lock(s.mu);
    bool seen = false;
    for (const auto& f : s.got)
        seen |= f.kind == infra::kKindStream && f.vip == 0x0A500002 && f.dst_port == 7000 && f.src_port == 50000 &&
                f.payload.size() == 2;
    CHECK(seen, "send_stream reaches the server as kind 3");
}

static void queues_bounded_in_bytes() {
    FakeInfraServer s;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000), "connect");
    std::vector<uint8_t> big(4000, 1);
    for (int i = 0; i < 600; i++)  // 2.4 MB spread over 600 ports nobody reads
        s.send_raw(infra::encode_datagram(0x0A500002, static_cast<uint16_t>(20000 + i), 1, big.data(), big.size()));
    for (int i = 0; i < 1200; i++)  // 4.8 MB of stream segments nobody reads
        s.send_raw(infra::encode_segment(infra::kKindStream, 0x0A500002, 7000, 1, big.data(), big.size()));
    sleep_ms(500);
    CHECK(c.queued_bytes() <= infra::Client::kMaxQueuedBytes, "datagram queues capped in bytes");
    CHECK(c.queued_stream_bytes() <= infra::Client::kMaxStreamBytes, "stream queue capped in bytes");
    infra::Client::Datagram g;
    CHECK(c.pop_datagram(20000, &g) && g.payload.size() == 4000, "early datagrams still delivered");
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

// A real TCP transport whose sends can be held: each send waits delay_ms, then
// while blocked; fail_at = n fails the n-th send. close() wakes a held send.
struct SlowTransport : infra::Transport {
    std::unique_ptr<infra::Transport> inner = infra::make_transport(infra::Address::Tcp);
    std::mutex mu;
    std::condition_variable cv;
    bool closed = false, blocked = false;
    int delay_ms = 0, fail_at = 0;
    std::atomic<int> sends{0}, waiting{0};

    bool open(const infra::Address& a, int timeout_ms) override { return inner->open(a, timeout_ms); }
    bool send_frame(const std::vector<uint8_t>& f) override {
        const int n = ++sends;
        {
            std::unique_lock<std::mutex> l(mu);
            if (fail_at && n == fail_at) return false;
            waiting++;
            if (delay_ms) cv.wait_for(l, std::chrono::milliseconds(delay_ms), [&] { return closed; });
            cv.wait(l, [&] { return !blocked || closed; });
            waiting--;
            if (closed) return false;
        }
        return inner->send_frame(f);
    }
    bool recv(std::vector<uint8_t>* bytes, int timeout_ms) override { return inner->recv(bytes, timeout_ms); }
    void close() override {
        {
            std::lock_guard<std::mutex> l(mu);
            closed = true;
        }
        cv.notify_all();
        inner->close();
    }
    void set(bool block, int delay = 0) {
        {
            std::lock_guard<std::mutex> l(mu);
            blocked = block;
            delay_ms = delay;
        }
        cv.notify_all();
    }
};

// Options whose transports are SlowTransports; *last is the newest one.
static infra::Client::Options slow_opts(SlowTransport** last, int fail_first_at = 0) {
    infra::Client::Options o = fast();
    auto made = std::make_shared<int>(0);
    o.transport_factory = [last, made, fail_first_at](infra::Address::Kind) {
        auto t = std::make_unique<SlowTransport>();
        if ((*made)++ == 0) t->fail_at = fail_first_at;
        *last = t.get();
        return std::unique_ptr<infra::Transport>(std::move(t));
    };
    return o;
}

static bool wait_until(const std::function<bool()>& f, int ms) {
    const auto t = Clock::now();
    while (!f()) {
        if (ms_since(t) > ms) return false;
        sleep_ms(2);
    }
    return true;
}

static void send_never_blocks() {
    FakeInfraServer s;
    s.start();
    SlowTransport* slow = nullptr;
    infra::Client c(s.address(), slow_opts(&slow));
    CHECK(c.connect("P3A", 2000), "connect");
    slow->set(false, 300);
    const uint8_t d[8] = {};
    const auto t = Clock::now();
    for (int i = 0; i < 50; i++) c.send_datagram(0x0A500002, 3658, 3658, d, sizeof(d));
    CHECK(ms_since(t) < 20, "50 sends over a 300 ms transport return at once");
}

static void outbox_keeps_order() {
    FakeInfraServer s;
    s.on_frame = [](FakeInfraServer&, const infra::Frame&) {};
    s.start();
    SlowTransport* slow = nullptr;
    infra::Client c(s.address(), slow_opts(&slow));
    CHECK(c.connect("P3A", 2000), "connect");
    slow->set(true);
    for (int i = 0; i < 30; i++) {
        const uint8_t b[2] = {static_cast<uint8_t>(i), 0};
        if (i % 3 == 0) c.send_datagram(0x0A500002, 3658, 3658, b, 2);
        if (i % 3 == 1) c.send_stream(0x0A500002, 7000, 7001, b, 2);
        if (i % 3 == 2) c.request({{"op", "x"}, {"i", i}}, [](const nlohmann::json&) {}, 5000);
    }
    slow->set(false);
    CHECK(wait_until([&] { std::lock_guard<std::mutex> l(s.mu); return s.got.size() >= 31; }, 2000), "all arrive");
    std::lock_guard<std::mutex> l(s.mu);
    int next = 0;
    bool ordered = true;
    for (const auto& f : s.got) {
        if (f.kind == infra::kKindJson && f.json.value("op", "") != "x") continue;  // login, pings
        const int i = f.kind == infra::kKindJson ? f.json.value("i", -1) : f.payload[0];
        const uint8_t want = static_cast<uint8_t>(i % 3 == 0 ? infra::kKindDatagram
                                                  : i % 3 == 1 ? infra::kKindStream : infra::kKindJson);
        ordered &= i == next && f.kind == want;
        next++;
    }
    CHECK(ordered && next == 30, "datagrams, stream segments and requests leave in call order");
}

static void outbox_drops_oldest_datagrams() {
    FakeInfraServer s;
    s.start();
    SlowTransport* slow = nullptr;
    infra::Client c(s.address(), slow_opts(&slow));
    CHECK(c.connect("P3A", 2000), "connect");
    slow->set(true);
    std::vector<uint8_t> big(1024, 0);
    big[2] = 1;  // the probe the writer holds
    c.send_datagram(0x0A500002, 3658, 3658, big.data(), big.size());
    CHECK(wait_until([&] { return slow->waiting == 1; }, 2000), "the writer holds a send");
    big[2] = 0;
    for (int i = 0; i < 300; i++) {  // 300 KiB of datagrams queued behind it
        big[0] = static_cast<uint8_t>(i);
        big[1] = static_cast<uint8_t>(i >> 8);
        c.send_datagram(0x0A500002, 3658, 3658, big.data(), big.size());
    }
    const uint8_t seg[3] = {9, 9, 9};
    c.send_stream(0x0A500002, 7000, 7001, seg, 3);
    slow->set(false);
    CHECK(wait_until([&] {
              std::lock_guard<std::mutex> l(s.mu);
              for (const auto& f : s.got) if (f.kind == infra::kKindStream) return true;
              return false;
          }, 3000), "the stream segment arrives");
    std::lock_guard<std::mutex> l(s.mu);
    std::vector<int> seen;
    for (const auto& f : s.got)
        if (f.kind == infra::kKindDatagram && f.payload.size() == 1024 && f.payload[2] == 0)
            seen.push_back(f.payload[0] | (f.payload[1] << 8));
    CHECK(!seen.empty() && seen.front() > 0 && seen.back() == 299, "the oldest datagrams dropped, the newest kept");
    CHECK(seen.size() < 300 && seen.size() >= 200, "about 256 KiB of datagrams kept");
}

static void disconnect_unblocks_writer() {
    FakeInfraServer s;
    s.start();
    SlowTransport* slow = nullptr;
    infra::Client c(s.address(), slow_opts(&slow));
    CHECK(c.connect("P3A", 2000), "connect");
    slow->set(true);
    const uint8_t d[4] = {};
    c.send_datagram(0x0A500002, 3658, 3658, d, 4);
    CHECK(wait_until([&] { return slow->waiting == 1; }, 2000), "a send is held");
    const auto t = Clock::now();
    c.disconnect();
    CHECK(ms_since(t) < 1000, "disconnect returns while the writer is blocked in a send");
    CHECK(!c.connected(), "disconnected");
}

static void outbox_reconnect_after_loss() {
    FakeInfraServer s;
    s.reaccept = true;
    s.on_frame = [](FakeInfraServer& srv, const infra::Frame& f) {
        if (f.kind == infra::kKindJson && f.json["op"] == "join")
            srv.send_json({{"id", f.json["id"]}, {"ok", true}, {"room", {{"id", 5}, {"me", 0x11}}}});
    };
    s.start();
    SlowTransport* slow = nullptr;
    infra::Client::Options o = slow_opts(&slow, 3);  // 1 login, 2 join, 3 fails
    o.ping_interval_ms = 100000;
    infra::Client c(s.address(), o);
    std::atomic<int> destroyed{0};
    c.on_event([&](const nlohmann::json& e) { if (e.value("ev", "") == "room_destroyed") destroyed++; });
    CHECK(c.connect("P3A", 2000), "connect");
    std::atomic<int> joined{0};
    c.request({{"op", "join"}, {"room_id", 5}}, [&](const nlohmann::json& j) { if (j.value("ok", false)) joined++; },
              2000);
    CHECK(wait_until([&] { return joined == 1; }, 2000), "joined");
    const uint8_t d[4] = {1, 2, 3, 4};
    c.send_datagram(0x0A500002, 3658, 3658, d, 4);
    CHECK(wait_until([&] { return !c.connected(); }, 2000), "a failed send loses the connection");
    sleep_ms(200);
    CHECK(destroyed == 1, "lost() ran once (one room_destroyed)");
    CHECK(c.connect("P3A", 2000), "a second connect works");
    const uint8_t e[4] = {5, 6, 7, 8};
    c.send_datagram(0x0A500002, 3658, 3658, e, 4);
    CHECK(wait_until([&] {
              std::lock_guard<std::mutex> l(s.mu);
              for (const auto& f : s.got)
                  if (f.kind == infra::kKindDatagram && f.payload == std::vector<uint8_t>({5, 6, 7, 8})) return true;
              return false;
          }, 2000), "and sends");
    CHECK(destroyed == 1, "still one room_destroyed");
}

static std::vector<nlohmann::json> link_frames(FakeInfraServer& s) {
    std::lock_guard<std::mutex> l(s.mu);
    std::vector<nlohmann::json> v;
    for (const auto& f : s.got)
        if (f.kind == infra::kKindJson && f.json.value("op", "") == "link_stats") v.push_back(f.json);
    return v;
}

// The session's link numbers go to the server before a disconnect; a server that never answers
// holds the disconnect 1 s at most.
static void link_stats_sent_on_disconnect() {
    FakeInfraServer s;
    s.answer_link_stats = false;  // never answered
    s.start();
    infra::Client::Options o = fast();
    o.ping_interval_ms = 50;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    CHECK(wait_until([&] { return c.link_summary().pings >= 3; }, 2000), "three pings answered");
    const auto t = Clock::now();
    c.disconnect();
    const long took = ms_since(t);
    CHECK(took <= 1500, "disconnect within 1.5 s with an unanswered link_stats");
    const auto v = link_frames(s);
    CHECK(v.size() == 1, "one link_stats frame");
    if (v.empty()) return;
    const nlohmann::json& j = v[0];
    CHECK(j.value("pings", 0) >= 3, "carries the answered pings");
    bool keys = true;
    for (const char* k : {"pings", "slow_pings", "rtt_p50", "rtt_p95", "rtt_max", "max_rx_gap_ms"})
        keys &= j.contains(k) && j[k].is_number_integer();
    CHECK(keys, "all six numbers");
    CHECK(!j.contains("addr") && !j.contains("ip") && j.size() == 8, "op, id and the six numbers only");
}

static void no_link_stats_after_loss() {
    FakeInfraServer s;
    s.start();
    infra::Client::Options o = fast();
    o.ping_interval_ms = 50;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    CHECK(wait_until([&] { return c.link_summary().pings >= 2; }, 2000), "pings answered");
    s.close_client();
    CHECK(wait_until([&] { return !c.connected(); }, 2000), "loss seen");
    c.disconnect();
    CHECK(link_frames(s).empty(), "a lost connection sends no link_stats");
}

static void no_link_stats_without_pings() {
    FakeInfraServer s;
    s.answer_pings = false;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000), "connect");
    sleep_ms(150);
    const auto t = Clock::now();
    c.disconnect();
    CHECK(ms_since(t) < 500, "nothing to report: disconnect does not wait");
    CHECK(link_frames(s).empty(), "no link_stats without an answered ping");
}

// Players often close the window instead of leaving online: the numbers are also refreshed while
// connected (the server overwrites the session's row).
static void link_stats_refreshed_while_connected() {
    FakeInfraServer s;
    s.start();
    infra::Client::Options o = fast();
    o.ping_interval_ms = 50;
    o.link_report_ms = 200;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    CHECK(wait_until([&] { return link_frames(s).size() >= 2; }, 2000), "refreshed every link_report_ms");
    c.disconnect();
}

// A server without op_link_stats (BAD_REQUEST) is not asked again in that session.
static void link_stats_stop_on_old_server() {
    FakeInfraServer s;
    s.answer_link_stats = false;
    s.on_frame = [](FakeInfraServer& srv, const infra::Frame& f) {
        if (f.json.value("op", "") == "link_stats")
            srv.send_json({{"id", f.json["id"]}, {"ok", false}, {"error", 0x80550D01u}});
    };
    s.start();
    infra::Client::Options o = fast();
    o.ping_interval_ms = 50;
    o.link_report_ms = 100;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    CHECK(wait_until([&] { return link_frames(s).size() >= 1; }, 2000), "first report");
    sleep_ms(500);
    c.disconnect();
    CHECK(link_frames(s).size() == 1, "refused once, never sent again");
}

// The gap counts from the oldest unanswered ping, not from the previous frame: on a quiet link a
// ping answer after 100 ms is a 100 ms gap, not the ping interval.
static void link_gap_counts_from_the_ping() {
    FakeInfraServer s;
    s.ping_delay_ms = 100;
    s.start();
    infra::Client::Options o = fast();
    o.ping_interval_ms = 800;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    CHECK(wait_until([&] { return c.link_summary().pings >= 3; }, 4000), "three pings answered");
    const infra::LinkSummary l = c.link_summary();
    CHECK(l.max_rx_gap_ms >= 90 && l.max_rx_gap_ms < 400, "gap near the ping delay, not the interval");
    CHECK(l.rtt_p50 >= 90 && l.rtt_max < 400, "RTTs near the ping delay");
}

// Optional: INFRA_WS_URL=ws://127.0.0.1:27321/infra with tools/infra_server.py
// running exercises the WebSocket transport against the real server.
// ---- direct path (protocol 3) ----

static void set_env(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static nlohmann::json login_frame(FakeInfraServer& s) {
    std::lock_guard<std::mutex> l(s.mu);
    for (const auto& f : s.got)
        if (f.kind == infra::kKindJson && f.json.value("op", "") == "login") return f.json;
    return {};
}

static void direct_off_reasons() {
    using A = infra::Address;
    const A loop{A::Tcp, "127.0.0.1", 27320, ""}, wss{A::Wss, "online.montick.app", 443, "/infra"};
    auto is = [](const char* got, const char* want) { return got && std::string(got) == want; };
    CHECK(infra::direct_off_reason(loop, nullptr, true) == nullptr, "loopback tcp: direct allowed");
    CHECK(infra::direct_off_reason(A{A::Ws, "localhost", 80, "/infra"}, "1", true) == nullptr, "localhost ws");
    CHECK(infra::direct_off_reason(wss, "true", true) == nullptr, "wss: allowed");
    CHECK(is(infra::direct_off_reason(A{A::Tcp, "192.0.2.10", 27320, ""}, nullptr, true), "plain-text server"),
          "no_caps_plaintext_remote");
    CHECK(is(infra::direct_off_reason(A{A::Ws, "192.0.2.10", 80, "/infra"}, nullptr, true), "plain-text server"),
          "remote ws is plain text");
    CHECK(is(infra::direct_off_reason(wss, "0", true), "opted out"), "env 0: opted out");
    CHECK(is(infra::direct_off_reason(wss, "false", true), "opted out"), "env false: opted out");
    CHECK(is(infra::direct_off_reason(wss, nullptr, false), "no randomness"), "no randomness");
}

static void caps_on_loopback() {
    set_env("PSPRECOMP_INFRA_DIRECT", nullptr);
    FakeInfraServer s;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000, infra::login_fields("02000000000a", nullptr)), "connect");
    const nlohmann::json j = login_frame(s);
    CHECK(j.value("proto", 0) == 3 && j.contains("caps") && j["caps"].value("direct", 0) == 1,
          "caps_on_loopback: caps.direct 1 and proto 3");
    // An old server answers without "direct": the client stays on the relay.
    CHECK(c.direct_reason() && std::string(c.direct_reason()) == "server off", "old server: off");
}

static void no_caps_when_opted_out() {
    set_env("PSPRECOMP_INFRA_DIRECT", "0");
    FakeInfraServer s;
    s.start();
    infra::Client c(s.address(), fast());
    CHECK(c.connect("P3A", 2000, infra::login_fields("02000000000a", nullptr)), "connect");
    CHECK(!login_frame(s).contains("caps"), "opted out: no caps");
    CHECK(c.direct_reason() && std::string(c.direct_reason()) == "opted out", "opted out: the reason");
    set_env("PSPRECOMP_INFRA_DIRECT", nullptr);
}

static void direct_events_not_forwarded() {
    FakeInfraServer s;
    s.start();
    infra::Client c(s.address(), fast());
    std::mutex mu;
    std::vector<std::string> seen;
    c.on_event([&](const nlohmann::json& e) {
        std::lock_guard<std::mutex> l(mu);
        seen.push_back(e.value("ev", ""));
    });
    CHECK(c.connect("P3A", 2000, infra::login_fields("02000000000a", nullptr)), "connect");
    s.send_json({{"ev", "direct_peer"}, {"vip", 0x0A500009}, {"pair", 77}, {"key", std::string(64, 'a')},
                 {"cands", nlohmann::json::array()}});
    s.send_json({{"ev", "direct_drop"}, {"vip", 0x0A500009}, {"pair", 77}});
    s.send_json({{"ev", "member_joined"}, {"room_id", 1}});
    CHECK(wait_until([&] { std::lock_guard<std::mutex> l(mu); return !seen.empty(); }, 2000), "an event arrived");
    sleep_ms(100);
    std::lock_guard<std::mutex> l(mu);
    CHECK(seen == std::vector<std::string>{"member_joined"}, "direct_peer / direct_drop never reach the game");
}

static void loss_drops_peers() {
    set_env("PSPRECOMP_INFRA_DIRECT_TEST_HOSTS", "127.0.0.1");
    FakeInfraServer s;
    s.login_extra = {{"direct", {{"on", true}, {"stun", nlohmann::json::array()}}}};
    std::mutex mu;
    nlohmann::json offer;
    s.on_frame = [&](FakeInfraServer& srv, const infra::Frame& f) {
        if (f.kind != infra::kKindJson || f.json.value("op", "") != "direct_offer") return;
        std::lock_guard<std::mutex> l(mu);
        offer = f.json;
        srv.send_json({{"id", f.json["id"]}, {"ok", true}});
    };
    s.start();
    infra::Client::Options o = fast();
    o.lab_hooks = true;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000, infra::login_fields("02000000000a", nullptr)), "connect");
    CHECK(c.direct_reason() == nullptr, "direct on");
    CHECK(wait_until([&] { std::lock_guard<std::mutex> l(mu); return offer.contains("cands"); }, 3000), "offer sent");
    std::vector<direct::Cand> client_cands;
    std::string text;
    {
        std::lock_guard<std::mutex> l(mu);
        text = offer.dump();
        for (const auto& cd : offer["cands"]) {
            const std::string a = cd.value("a", "");
            if (a.rfind("127.0.0.1:", 0) == 0)
                client_cands.push_back(
                    {{0x7F000001, static_cast<uint16_t>(std::stoi(a.substr(10)))}, direct::CandType::Host});
        }
    }
    CHECK(!client_cands.empty(), "the offer carries the loopback host candidate");
    CHECK(text.find("srflx") == std::string::npos, "no STUN servers: no srflx candidate");
    // The peer: a test-side Link with the same pair key.
    constexpr uint32_t kPeer = 0x0A500009;
    std::mutex gmu;
    std::vector<std::vector<uint8_t>> got;
    direct::Link peer;
    direct::LinkOptions lo;
    lo.test_host_ips = {0x7F000001};
    CHECK(peer.start(lo,
                     [&](uint32_t, direct::Type, uint16_t, uint16_t, std::vector<uint8_t> p) {
                         std::lock_guard<std::mutex> l(gmu);
                         got.push_back(std::move(p));
                     },
                     nullptr),
          "peer link");
    nlohmann::json cands = nlohmann::json::array();
    for (const auto& pc : peer.local_candidates(200))
        if (pc.addr.ip == 0x7F000001) cands.push_back({{"t", "host"}, {"a", "127.0.0.1:" + std::to_string(pc.addr.port)}});
    uint8_t key[32];
    std::string key_hex;
    for (int i = 0; i < 32; i++) {
        key[i] = static_cast<uint8_t>(0x40 + i);
        char h[3];
        std::snprintf(h, sizeof h, "%02x", key[i]);
        key_hex += h;
    }
    peer.add_peer(kPeer, c.vip(), 0xABCD, key, client_cands);
    s.send_json({{"ev", "direct_peer"}, {"vip", kPeer}, {"pair", 0xABCD}, {"key", key_hex}, {"cands", cands}});
    CHECK(wait_until([&] { return c.direct_up(kPeer) && peer.is_up(c.vip()); }, 2000), "client <-> peer Up");
    const uint8_t hello[5] = {'h', 'e', 'l', 'l', 'o'};
    c.send_datagram(kPeer, 3658, 3659, hello, 5);
    CHECK(wait_until([&] { std::lock_guard<std::mutex> l(gmu); return got.size() == 1; }, 1000),
          "a game datagram goes over the direct path");
    peer.send_dgram(c.vip(), 3659, 3658, hello, 5);
    infra::Client::Datagram d;
    CHECK(wait_until([&] { return c.pop_datagram(3659, &d); }, 1000) && d.vip == kPeer && d.payload.size() == 5,
          "a direct datagram reaches the same per-port queue");
    s.close_client();
    CHECK(wait_until([&] { return !c.connected(); }, 3000), "the server connection is lost");
    CHECK(!c.direct_up(kPeer), "loss_drops_peers: no peer after the loss");
    c.send_datagram(kPeer, 3658, 3659, hello, 5);
    sleep_ms(200);
    {
        std::lock_guard<std::mutex> l(gmu);
        CHECK(got.size() == 1, "send_datagram goes nowhere after the loss");
    }
    peer.stop();
    set_env("PSPRECOMP_INFRA_DIRECT_TEST_HOSTS", nullptr);
}

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

// At quit main() gives disconnect a short budget: a server that never answers holds the quit that long at most,
// and the stats still go out.
static void disconnect_budget_bounds_quit() {
    FakeInfraServer s;
    s.answer_link_stats = false;
    s.start();
    infra::Client::Options o = fast();
    o.ping_interval_ms = 50;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    CHECK(wait_until([&] { return c.link_summary().pings >= 3; }, 2000), "pings answered");
    const auto t = Clock::now();
    CHECK(c.disconnect_if_idle(400), "an idle session is closed");
    CHECK(ms_since(t) <= 700, "disconnect_if_idle(400) within 0.7 s with an unanswered link_stats");
    CHECK(link_frames(s).size() == 1, "the link_stats frame still went out");
}

// A connect or disconnect already running (the NP worker) is not waited for at quit.
static void quit_skips_a_busy_session() {
    FakeInfraServer s;
    s.answer_link_stats = false;
    s.start();
    infra::Client::Options o = fast();
    o.ping_interval_ms = 50;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    CHECK(wait_until([&] { return c.link_summary().pings >= 3; }, 2000), "pings answered");
    std::thread worker([&] { c.disconnect(); });  // waits for the unanswered link_stats
    sleep_ms(100);
    const auto t = Clock::now();
    CHECK(!c.disconnect_if_idle(400), "busy: not closed at quit");
    CHECK(ms_since(t) <= 100, "busy: quit does not wait for the running disconnect");
    worker.join();
}

// Both of the session's numbers go out before any wait: a slow server still gets direct_stats within the budget.
static void quit_sends_both_stats_at_once() {
    set_env("PSPRECOMP_INFRA_DIRECT_TEST_HOSTS", "127.0.0.1");
    FakeInfraServer s;
    s.answer_link_stats = false;  // never answered: the wait for it must not hold back direct_stats
    s.login_extra = {{"direct", {{"on", true}, {"stun", nlohmann::json::array()}}}};
    std::mutex mu;
    bool offered = false;
    std::map<std::string, Clock::time_point> at;
    s.on_frame = [&](FakeInfraServer& srv, const infra::Frame& f) {
        if (f.kind != infra::kKindJson) return;
        const std::string op = f.json.value("op", "");
        std::lock_guard<std::mutex> l(mu);
        at.emplace(op, Clock::now());
        if (op == "direct_offer") offered = true, srv.send_json({{"id", f.json["id"]}, {"ok", true}});
    };
    s.start();
    infra::Client::Options o = fast();
    o.lab_hooks = true;
    o.ping_interval_ms = 50;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000, infra::login_fields("02000000000a", nullptr)), "connect");
    CHECK(wait_until([&] { std::lock_guard<std::mutex> l(mu); return offered; }, 3000), "offer sent");
    CHECK(wait_until([&] { return c.link_summary().pings >= 3; }, 2000), "pings answered");
    s.send_json({{"ev", "direct_peer"}, {"vip", 0x0A500009}, {"pair", 77}, {"key", std::string(64, 'a')},
                 {"cands", nlohmann::json::array()}});
    sleep_ms(200);
    CHECK(c.disconnect_if_idle(400), "closed");
    sleep_ms(100);
    std::lock_guard<std::mutex> l(mu);
    CHECK(at.count("link_stats") && at.count("direct_stats"), "both stats frames reached the server");
    if (at.count("link_stats") && at.count("direct_stats"))
        CHECK(at["direct_stats"] - at["link_stats"] < std::chrono::milliseconds(150),
              "direct_stats did not wait for the link_stats answer");
    set_env("PSPRECOMP_INFRA_DIRECT_TEST_HOSTS", nullptr);
}

int main() {
    parse_address_forms();
    login_sets_vip();
    login_carries_extra_fields();
    login_fields_carry_proto_and_invite();
    request_answer_matched_by_id();
    request_times_out();
    disconnect_fails_pending();
    loss_destroys_every_room();
    datagram_queue_per_port();
    stream_segments_in_order();
    queues_bounded_in_bytes();
    rtt_from_ping();
    silent_server_is_dead();
    send_never_blocks();
    outbox_keeps_order();
    outbox_drops_oldest_datagrams();
    disconnect_unblocks_writer();
    outbox_reconnect_after_loss();
    link_stats_sent_on_disconnect();
    disconnect_budget_bounds_quit();
    quit_skips_a_busy_session();
    quit_sends_both_stats_at_once();
    no_link_stats_after_loss();
    no_link_stats_without_pings();
    link_stats_refreshed_while_connected();
    link_stats_stop_on_old_server();
    link_gap_counts_from_the_ping();
    direct_off_reasons();
    caps_on_loopback();
    no_caps_when_opted_out();
    direct_events_not_forwarded();
    loss_drops_peers();
    ws_against_real_server();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("infra_client: all tests passed\n");
    return 0;
}
