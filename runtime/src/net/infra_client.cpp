// Client of the infrastructure-mode room server (see net/infra_client.h).
#include "net/infra_client.h"

#include "psp_socket_compat.h"  // before windows.h (winsock2 first)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "monocypher.h"
#include "net/infra_stats.h"
#include "net/relay_conn.h"
#include "net/secure_random.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace infra {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kErrBadRequest = 0x80550D01;  // tools/infra_server.py BAD_REQUEST (unknown op on old servers)
constexpr int kLinkStatsWaitMs = 750;  // disconnect waits this long at most for the link_stats answer
constexpr int kDirectStatsWaitMs = 1000;
constexpr int kGatherMs = 2000;  // STUN gathering before direct_offer

std::atomic<bool> g_lab_hooks{false};

// "a.b.c.d" -> host-order IPv4; false otherwise.
bool parse_ipv4(const std::string& s, uint32_t* ip) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 ||
        d > 255)
        return false;
    *ip = a << 24 | b << 16 | c << 8 | d;
    return true;
}

std::string ipv4_text(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) +
           "." + std::to_string(ip & 255);
}

// A wait for one answer, shared with the callback so a late answer after the wait is harmless.
struct AnswerWait {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    void finish() {
        std::lock_guard<std::mutex> l(mu);
        done = true;
        cv.notify_all();
    }
    void wait(int ms) {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return done; });
    }
};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

bool parse_port(const std::string& s, uint16_t* out) {
    if (s.empty() || s.size() > 5) return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v == 0 || v > 65535) return false;
    *out = static_cast<uint16_t>(v);
    return true;
}

class TcpTransport : public Transport {
public:
    ~TcpTransport() override {
        if (fd_ >= 0) psp_sock_close(fd_);
    }
    bool open(const Address& a, int timeout_ms) override {
        fd_ = relay::connect_tcp(a.host, a.port, timeout_ms, "[INFRA]");
        return fd_ >= 0;
    }
    bool send_frame(const std::vector<uint8_t>& f) override {
        return !closed_ && relay::send_all(fd_, f.data(), f.size(), 5000);
    }
    bool recv(std::vector<uint8_t>* bytes, int timeout_ms) override {
        if (closed_) return false;
        if (!relay::wait_readable(fd_, timeout_ms)) return !closed_;
        return relay::read_available(fd_, bytes) && !closed_;
    }
    void close() override {
        if (closed_.exchange(true)) return;
        ::shutdown(fd_, SHUT_RDWR);
    }

private:
    int fd_ = -1;
    std::atomic<bool> closed_{false};
};

#ifdef _WIN32
// WebSocket through WinHTTP: the OS brings TLS (wss://) and the upgrade.
class WinHttpWsTransport : public Transport {
public:
    ~WinHttpWsTransport() override {
        close();
        if (connect_) WinHttpCloseHandle(connect_);
        if (session_) WinHttpCloseHandle(session_);
    }
    bool open(const Address& a, int timeout_ms) override {
        const std::wstring host(a.host.begin(), a.host.end()), path(a.path.begin(), a.path.end());
        session_ = WinHttpOpen(L"psprecomp-infra/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) return fail("WinHttpOpen");
        WinHttpSetTimeouts(session_, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        connect_ = WinHttpConnect(session_, host.c_str(), a.port, 0);
        if (!connect_) return fail("WinHttpConnect");
        HINTERNET req = WinHttpOpenRequest(connect_, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           a.kind == Address::Wss ? WINHTTP_FLAG_SECURE : 0);
        if (!req) return fail("WinHttpOpenRequest");
        DWORD status = 0, size = sizeof(status);
        const bool ok = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(req, nullptr) &&
                        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (!ok || status != 101) {
            std::fprintf(stderr, "[INFRA] WebSocket upgrade failed (HTTP %lu, error %lu)\n",
                         static_cast<unsigned long>(status), static_cast<unsigned long>(GetLastError()));
            WinHttpCloseHandle(req);
            return false;
        }
        ws_ = WinHttpWebSocketCompleteUpgrade(req, 0);
        WinHttpCloseHandle(req);
        if (!ws_) return fail("WinHttpWebSocketCompleteUpgrade");
        DWORD forever = 0;  // the reader waits until data or close(); the client detects a silent server
        WinHttpSetOption(ws_, WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof(forever));
        return true;
    }
    bool send_frame(const std::vector<uint8_t>& f) override {
        if (closed_) return false;
        return WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                    const_cast<uint8_t*>(f.data()), static_cast<DWORD>(f.size())) == NO_ERROR;
    }
    bool recv(std::vector<uint8_t>* bytes, int) override {
        if (closed_) return false;
        uint8_t buf[16384];
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD err = WinHttpWebSocketReceive(ws_, buf, sizeof(buf), &got, &type);
        if (err == ERROR_WINHTTP_TIMEOUT) return !closed_;
        if (err != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
        bytes->insert(bytes->end(), buf, buf + got);  // messages carry whole frames: the stream stays aligned
        return !closed_;
    }
    void close() override {
        if (closed_.exchange(true) || !ws_) return;
        WinHttpCloseHandle(ws_);  // aborts a receive blocked on another thread
    }

private:
    bool fail(const char* what) {
        std::fprintf(stderr, "[INFRA] %s failed (error %lu)\n", what, static_cast<unsigned long>(GetLastError()));
        return false;
    }
    HINTERNET session_ = nullptr, connect_ = nullptr, ws_ = nullptr;
    std::atomic<bool> closed_{false};
};
#else
class NoWsTransport : public Transport {
public:
    bool open(const Address& a, int) override {
        std::fprintf(stderr, "[INFRA] %s: WebSocket servers need WinHTTP (Windows); use tcp://\n",
                     to_string(a).c_str());
        return false;
    }
    bool send_frame(const std::vector<uint8_t>&) override { return false; }
    bool recv(std::vector<uint8_t>*, int) override { return false; }
    void close() override {}
};
#endif

}  // namespace

Address parse_address(const char* text) {
    Address a;
    std::string s = text ? text : "";
    if (s.empty()) s = "wss://online.montick.app/infra";
    if (s == "off" || s == "OFF" || s == "0") return a;
    Address::Kind kind = Address::Tcp;
    uint16_t port = 27320;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        const std::string sc = s.substr(0, scheme);
        if (sc == "tcp") {
            kind = Address::Tcp;
        } else if (sc == "ws") {
            kind = Address::Ws;
            port = 80;
        } else if (sc == "wss") {
            kind = Address::Wss;
            port = 443;
        } else {
            std::fprintf(stderr, "[INFRA] PSPRECOMP_INFRA_SERVER=%s: unknown scheme, infrastructure mode off\n",
                         s.c_str());
            return a;
        }
        s = s.substr(scheme + 3);
    }
    std::string path = "/infra";
    const size_t slash = s.find('/');
    if (slash != std::string::npos) {
        if (kind == Address::Tcp) {
            std::fprintf(stderr, "[INFRA] PSPRECOMP_INFRA_SERVER: a tcp address takes no path\n");
            return a;
        }
        path = s.substr(slash);
        s = s.substr(0, slash);
    }
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        if (!parse_port(s.substr(colon + 1), &port)) {
            std::fprintf(stderr, "[INFRA] PSPRECOMP_INFRA_SERVER: bad port, infrastructure mode off\n");
            return a;
        }
        s = s.substr(0, colon);
    }
    if (s.empty()) return a;
    a.kind = kind;
    a.host = s;
    a.port = port;
    a.path = kind == Address::Tcp ? "" : path;
    return a;
}

nlohmann::json login_fields(const std::string& console_hex, const char* invite) {
    nlohmann::json f = {{"console", console_hex}, {"proto", kProto}};
    if (invite && invite[0]) f["invite"] = invite;
    return f;
}

const char* direct_off_reason(const Address& a, const char* env_direct, bool have_random) {
    if (env_direct && (std::strcmp(env_direct, "0") == 0 || std::strcmp(env_direct, "false") == 0)) return "opted out";
    const bool loopback = a.host == "127.0.0.1" || a.host == "localhost" || a.host == "::1";
    if (a.kind != Address::Wss && !loopback) return "plain-text server";
    if (!have_random) return "no randomness";
    return nullptr;
}

void set_lab_hooks(bool on) { g_lab_hooks = on; }

const char* login_error_name(uint32_t code) {
    switch (code) {
    case 0x80550DA1: return "INVITE_UNKNOWN";
    case 0x80550DA2: return "INVITE_DISABLED";
    case 0x80550DA3: return "BETA_CLOSED";
    case 0x80550DA4: return "OUTDATED";
    case 0x80550DA5: return "INVITE_IN_USE";
    default: return nullptr;
    }
}

std::string to_string(const Address& a) {
    switch (a.kind) {
    case Address::Off: return "off";
    case Address::Tcp: return "tcp://" + a.host + ":" + std::to_string(a.port);
    case Address::Ws: return "ws://" + a.host + ":" + std::to_string(a.port) + a.path;
    case Address::Wss: return "wss://" + a.host + ":" + std::to_string(a.port) + a.path;
    }
    return "?";
}

std::unique_ptr<Transport> make_transport(Address::Kind kind) {
    if (kind == Address::Tcp) return std::make_unique<TcpTransport>();
#ifdef _WIN32
    return std::make_unique<WinHttpWsTransport>();
#else
    return std::make_unique<NoWsTransport>();
#endif
}

Client::Client(Address addr, Options opt) : addr_(std::move(addr)), opt_(opt) {}

Client::~Client() { disconnect(); }

bool Client::connect(const std::string& online_id, int timeout_ms, const nlohmann::json& login_extra) {
    std::lock_guard<std::mutex> clock(connect_mu_);
    if (connected_) return true;
    if (!configured()) return false;
    // Threads of a connection lost earlier are done or finishing: reap them.
    running_ = false;
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> olock(out_mu_);  // the writer checks running_ under out_mu_
    }
    out_cv_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (ticker_.joinable()) ticker_.join();
    if (writer_.joinable()) writer_.join();
    if (offer_.joinable()) offer_.join();
    transport_ = opt_.transport_factory ? opt_.transport_factory(addr_.kind) : make_transport(addr_.kind);
    if (!transport_->open(addr_, timeout_ms)) {
        std::fprintf(stderr, "[INFRA] cannot reach %s\n", to_string(addr_).c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        rooms_.clear();
        rtts_.clear();
        pings_out_.clear();
        link_.reset();
        link_refused_ = false;
        connected_ = true;
    }
    {
        std::lock_guard<std::mutex> olock(out_mu_);  // frames queued after the last loss belong to no one
        out_.clear();
        out_dgram_bytes_ = out_stream_bytes_ = 0;
    }
    running_ = true;
    last_rx_ms_ = now_ms();
    reader_ = std::thread([this] { reader(); });
    ticker_ = std::thread([this] { ticker(); });
    writer_ = std::thread([this] { writer(); });  // the login below goes through the outbox

    std::mutex wmu;
    std::condition_variable wcv;
    bool done = false;
    nlohmann::json answer;
    nlohmann::json login = login_extra.is_object() ? login_extra : nlohmann::json::object();
    login["op"] = "login";
    login["online_id"] = online_id;
    uint8_t probe[8];
    const char* off = direct_off_reason(addr_, std::getenv("PSPRECOMP_INFRA_DIRECT"), net::secure_random(probe, 8));
    if (!off) login["caps"] = {{"direct", 1}};
    request(std::move(login),
            [&](const nlohmann::json& j) {
                std::lock_guard<std::mutex> l(wmu);
                answer = j;
                done = true;
                wcv.notify_all();
            },
            timeout_ms);
    {
        std::unique_lock<std::mutex> l(wmu);
        wcv.wait(l, [&] { return done; });
    }
    if (!answer.value("ok", false)) {
        const char* name = login_error_name(answer.value("error", 0u));
        std::fprintf(stderr, "[INFRA] login to %s as %s failed: %s%s%s\n", to_string(addr_).c_str(),
                     online_id.c_str(), name ? name : "", name ? " " : "", answer.dump().c_str());
        lost("login failed");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        session_ = answer.value("session", "");
        online_id_ = online_id;
    }
    vip_ = answer.value("vip", 0u);
    std::fprintf(stderr, "[INFRA] connected to %s as %s, virtual IP %u.%u.%u.%u\n", to_string(addr_).c_str(),
                 online_id.c_str(), vip_ >> 24, (vip_ >> 16) & 255, (vip_ >> 8) & 255, vip_ & 255u);
    direct_reason_ = off;
    if (!off) start_direct(answer);
    if (direct_reason_) std::fprintf(stderr, "[INFRA] direct: off (%s)\n", direct_reason_.load());
    return true;
}

void Client::disconnect() {
    std::lock_guard<std::mutex> clock(connect_mu_);
    if (connected_) {
        log_link();
        // The session's numbers go out first; the answer (or a lost connection) ends the wait early.
        struct Wait {
            std::mutex mu;
            std::condition_variable cv;
            bool done = false;
        };
        auto w = std::make_shared<Wait>();
        const bool sent = report_link(
            [w](const nlohmann::json&) {
                std::lock_guard<std::mutex> l(w->mu);
                w->done = true;
                w->cv.notify_all();
            },
            kLinkStatsWaitMs);
        if (sent) {
            std::unique_lock<std::mutex> l(w->mu);
            w->cv.wait_for(l, std::chrono::milliseconds(kLinkStatsWaitMs), [&] { return w->done; });
        }
        const nlohmann::json pairs = direct_.running() ? direct_.stats_json() : nlohmann::json::array();
        if (!pairs.empty()) {
            auto d = std::make_shared<AnswerWait>();
            request({{"op", "direct_stats"}, {"pairs", pairs}}, [d](const nlohmann::json&) { d->finish(); },
                    kDirectStatsWaitMs);
            d->wait(kDirectStatsWaitMs);
        }
    }
    lost("disconnect");  // closing the transport also unblocks a writer stuck in a send
    if (reader_.joinable()) reader_.join();
    if (ticker_.joinable()) ticker_.join();
    if (writer_.joinable()) writer_.join();
    if (offer_.joinable()) offer_.join();
    transport_.reset();
}

void Client::fail(Done& done) {
    if (done) done(nlohmann::json{{"ok", false}, {"error", kErrTimeout}});
}

void Client::lost(const char* why) {
    direct_.stop();  // first: once connected() is false no peer is left; keys wiped, nothing sent to the server
    direct_reason_ = "not connected";
    std::map<uint32_t, Pending> pending;
    std::set<int64_t> rooms;
    EventFn fn;
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        if (!connected_) return;
        connected_ = false;
        pending.swap(pending_);
        rooms.swap(rooms_);
        fn = on_event_;
        queues_.clear();
        streams_.clear();
        queued_bytes_ = stream_bytes_ = 0;
    }
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> olock(out_mu_);
        out_.clear();
        out_dgram_bytes_ = out_stream_bytes_ = 0;
    }
    out_cv_.notify_all();
    if (transport_) transport_->close();
    if (std::strcmp(why, "disconnect") != 0) {
        std::fprintf(stderr, "[INFRA] connection lost: %s\n", why);
        log_link();  // a lost connection sends nothing; the numbers stay in the log
    }
    for (auto& [id, p] : pending) fail(p.done);
    if (fn)
        for (int64_t room : rooms)
            fn(nlohmann::json{{"ev", "room_destroyed"}, {"room_id", room}, {"cause", kCauseConnectionError}});
}

bool Client::report_link(Done done, int timeout_ms) {
    const LinkSummary s = link_.summary();
    if (s.pings == 0 || link_refused_) return false;  // -1 percentiles carry nothing; old server
    nlohmann::json req = to_json(s);
    req["op"] = "link_stats";
    request(std::move(req),
            [this, done](const nlohmann::json& j) {
                if (!j.value("ok", false) && j.value("error", 0u) == kErrBadRequest) link_refused_ = true;
                if (done) done(j);
            },
            timeout_ms);
    return true;
}

void Client::log_link() const {
    const LinkSummary s = link_.summary();
    if (s.pings == 0) return;
    std::fprintf(stderr, "[INFRA] link: %d pings, p50 %d ms, p95 %d ms, max %d ms, %d slow, longest gap %d ms\n",
                 s.pings, s.rtt_p50, s.rtt_p95, s.rtt_max, s.slow_pings, s.max_rx_gap_ms);
}

void Client::send(Outgoing o) {
    bool backlog = false;
    {
        std::lock_guard<std::mutex> lock(out_mu_);
        const bool dgram = o.kind == kKindDatagram;
        (dgram ? out_dgram_bytes_ : out_stream_bytes_) += o.bytes.size();
        out_.push_back(std::move(o));
        while (dgram && out_dgram_bytes_ > kMaxOutboxDgramBytes) {  // a full socket buffer drops datagrams
            auto it = std::find_if(out_.begin(), out_.end(), [](const Outgoing& x) { return x.kind == kKindDatagram; });
            stats::record(stats::Ev::Drop, it->kind, it->vip, it->src, it->dst, it->bytes.data() + kFrameHeader,
                          it->bytes.size() - kFrameHeader);
            out_dgram_bytes_ -= it->bytes.size();
            out_.erase(it);
        }
        backlog = out_stream_bytes_ > kMaxOutboxStreamBytes;
    }
    out_cv_.notify_one();
    if (backlog) lost("send backlog");
}

void Client::writer() {
    for (;;) {
        Outgoing o;
        {
            std::unique_lock<std::mutex> lock(out_mu_);
            out_cv_.wait(lock, [this] { return !out_.empty() || !running_; });
            if (!running_) return;
            o = std::move(out_.front());
            out_.pop_front();
            (o.kind == kKindDatagram ? out_dgram_bytes_ : out_stream_bytes_) -= o.bytes.size();
        }
        if (!transport_->send_frame(o.bytes)) {
            lost("send failed");
            return;
        }
        if (o.kind)
            stats::record(stats::Ev::TxDone, o.kind, o.vip, o.src, o.dst, o.bytes.data() + kFrameHeader,
                          o.bytes.size() - kFrameHeader);
    }
}

void Client::request(nlohmann::json req, Done done, int timeout_ms) {
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!connected_) {
            id = 0;
        } else {
            id = ++next_id_;
            Pending& p = pending_[id];
            p.done = std::move(done);
            p.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
            p.op = req.value("op", "");
            if (p.op == "leave") p.room_id = req.value("room_id", int64_t{0});
        }
    }
    if (id == 0) {
        fail(done);
        return;
    }
    req["id"] = id;
    send(Outgoing{encode_json(req)});
}

void Client::on_event(EventFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    on_event_ = std::move(fn);
}

void Client::send_datagram(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t* data, size_t len) {
    if (!connected_) return;
    stats::record(stats::Ev::TxCall, kKindDatagram, vip, src_port, dst_port, data, len);
    if (direct_.send_dgram(vip, dst_port, src_port, data, len)) {
        stats::record(stats::Ev::TxDone, kKindDatagram, vip, src_port, dst_port, data, len);
        return;
    }
    send(Outgoing{encode_datagram(vip, dst_port, src_port, data, len), kKindDatagram, vip, src_port, dst_port});
}

bool Client::pop_datagram(uint16_t local_port, Datagram* out) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(local_port);
    if (it == queues_.end() || it->second.empty()) return false;
    *out = std::move(it->second.front());
    it->second.pop_front();
    queued_bytes_ -= out->payload.size();
    if (stats::enabled())
        stats::record(stats::Ev::Pop, kKindDatagram, out->vip, out->src_port, out->dst_port, out->payload.data(),
                      out->payload.size(), stats::now_us() - out->rx_us);
    return true;
}

size_t Client::queued_bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queued_bytes_;
}

size_t Client::queued_stream_bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stream_bytes_;
}

void Client::send_stream(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t* data, size_t len) {
    if (!connected_) return;
    stats::record(stats::Ev::TxCall, kKindStream, vip, src_port, dst_port, data, len);
    send(Outgoing{encode_segment(kKindStream, vip, dst_port, src_port, data, len), kKindStream, vip, src_port,
                  dst_port});
}

bool Client::pop_stream(Datagram* out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (streams_.empty()) return false;
    *out = std::move(streams_.front());
    streams_.pop_front();
    stream_bytes_ -= out->payload.size();
    if (stats::enabled())
        stats::record(stats::Ev::Pop, kKindStream, out->vip, out->src_port, out->dst_port, out->payload.data(),
                      out->payload.size(), stats::now_us() - out->rx_us);
    return true;
}

bool Client::has_datagram(uint16_t local_port) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(local_port);
    return it != queues_.end() && !it->second.empty();
}

int Client::rtt_ms() const {
    std::vector<int> v;
    {
        std::lock_guard<std::mutex> lock(mu_);
        v.assign(rtts_.begin(), rtts_.end());
    }
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::string Client::session() const {
    std::lock_guard<std::mutex> lock(mu_);
    return session_;
}

std::string Client::online_id() const {
    std::lock_guard<std::mutex> lock(mu_);
    return online_id_;
}

void Client::on_json(const nlohmann::json& j) {
    if (j.contains("ev")) {
        const std::string ev = j.value("ev", "");
        if (ev == "direct_peer" || ev == "direct_drop") return on_direct_event(j);  // never the game's
        EventFn fn;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const std::string ev = j.value("ev", "");
            if (ev == "room_destroyed" || ev == "kicked") rooms_.erase(j.value("room_id", int64_t{0}));
            fn = on_event_;
        }
        if (fn) fn(j);
        return;
    }
    if (!j.contains("id") || !j["id"].is_number_unsigned()) return;
    Done done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_.find(j["id"].get<uint32_t>());
        if (it == pending_.end()) return;  // answered after its timeout
        const bool ok = j.value("ok", false);
        if (ok && (it->second.op == "create" || it->second.op == "join") && j.contains("room") &&
            j["room"].is_object())
            rooms_.insert(j["room"].value("id", int64_t{0}));
        if (ok && it->second.op == "leave") rooms_.erase(it->second.room_id);
        done = std::move(it->second.done);
        pending_.erase(it);
    }
    if (done) done(j);
}

void Client::reader() {
    FrameReader r;
    std::vector<uint8_t> bytes;
    while (running_) {
        bytes.clear();
        if (!transport_->recv(&bytes, 200)) {
            lost("connection closed by the server");
            return;
        }
        if (bytes.empty()) continue;
        const int64_t rx = now_ms();
        {
            // A stall while a ping is out: count from the later of the previous frame and the oldest ping.
            std::lock_guard<std::mutex> lock(mu_);
            if (!pings_out_.empty())
                link_.rx_gap(static_cast<int>(rx - std::max<int64_t>(last_rx_ms_, *pings_out_.begin())));
        }
        last_rx_ms_ = rx;
        r.feed(bytes.data(), bytes.size());
        Frame f;
        while (r.next(&f)) {
            if (f.kind == kKindJson) {
                on_json(f.json);
                continue;
            }
            const int64_t rx_us = stats::now_us();
            stats::record(stats::Ev::Rx, f.kind, f.vip, f.src_port, f.dst_port, f.payload.data(), f.payload.size());
            if (f.kind != kKindStream) {
                enqueue_datagram(Datagram{f.vip, f.src_port, f.dst_port, std::move(f.payload), rx_us});
                continue;
            }
            std::lock_guard<std::mutex> lock(mu_);
            {
                if (streams_.size() >= kStreamQueueCap || stream_bytes_ + f.payload.size() > kMaxStreamBytes) {
                    static int logged = 0;
                    if (logged++ < 4) std::fprintf(stderr, "[INFRA] stream queue full: a game stream lost data\n");
                    stats::record(stats::Ev::Drop, f.kind, f.vip, f.src_port, f.dst_port, f.payload.data(),
                                  f.payload.size());
                    continue;
                }
                stream_bytes_ += f.payload.size();
                streams_.push_back(Datagram{f.vip, f.src_port, f.dst_port, std::move(f.payload), rx_us});
            }
        }
        if (r.broken()) {
            lost("bad frame from the server");
            return;
        }
    }
}

void Client::enqueue_datagram(Datagram d) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!connected_) return;  // a direct datagram that raced the loss
    if (queued_bytes_ + d.payload.size() > kMaxQueuedBytes) {  // nobody reads them: drop new
        stats::record(stats::Ev::Drop, kKindDatagram, d.vip, d.src_port, d.dst_port, d.payload.data(),
                      d.payload.size());
        return;
    }
    auto& q = queues_[d.dst_port];
    queued_bytes_ += d.payload.size();
    const int64_t rx_us = d.rx_us;
    q.push_back(std::move(d));
    if (q.size() > kQueueCap) {
        const Datagram& old = q.front();
        stats::record(stats::Ev::Drop, kKindDatagram, old.vip, old.src_port, old.dst_port, old.payload.data(),
                      old.payload.size(), rx_us - old.rx_us);
        queued_bytes_ -= old.payload.size();
        q.pop_front();
    }
}

void Client::start_direct(const nlohmann::json& answer) {
    const nlohmann::json d = answer.value("direct", nlohmann::json::object());
    if (!d.is_object() || !d.value("on", false)) {
        direct_reason_ = "server off";
        return;
    }
    direct::LinkOptions lo;
    for (const auto& s : d.value("stun", nlohmann::json::array()))
        if (s.is_string()) lo.stun.push_back(s.get<std::string>());
    const char* impair = std::getenv("PSPRECOMP_INFRA_DIRECT_IMPAIR");
    if (opt_.lab_hooks) {
        if (impair) lo.impair = impair;
        if (const char* hosts = std::getenv("PSPRECOMP_INFRA_DIRECT_TEST_HOSTS")) {
            std::string list = hosts;
            for (size_t at = 0; at <= list.size();) {
                size_t end = list.find(',', at);
                if (end == std::string::npos) end = list.size();
                uint32_t ip;
                if (parse_ipv4(list.substr(at, end - at), &ip)) lo.test_host_ips.push_back(ip);
                at = end + 1;
            }
        }
    } else if (impair && impair[0]) {
        std::fprintf(stderr, "[INFRA] direct: lab impairment ignored in a beta build\n");
    }
    const uint32_t me = vip_;
    if (!direct_.start(lo,
                     [this](uint32_t vip, direct::Type type, uint16_t dst, uint16_t src, std::vector<uint8_t> p) {
                         if (type != direct::Type::Dgram) return;
                         stats::record(stats::Ev::Rx, kKindDatagram, vip, src, dst, p.data(), p.size());
                         enqueue_datagram(Datagram{vip, src, dst, std::move(p), stats::now_us()});
                     },
                     nullptr)) {
        direct_reason_ = "no randomness";
        return;
    }
    std::fprintf(stderr, "[INFRA] direct: on (%zu STUN servers)\n", lo.stun.size());
    offer_ = std::thread([this, me] {
        const std::vector<direct::Cand> cands = direct_.local_candidates(kGatherMs);
        if (cands.empty() || !connected_ || vip_ != me) return;
        nlohmann::json list = nlohmann::json::array();
        for (const direct::Cand& c : cands)
            list.push_back({{"t", c.type == direct::CandType::Host ? "host" : "srflx"},
                            {"a", ipv4_text(c.addr.ip) + ":" + std::to_string(c.addr.port)}});
        request({{"op", "direct_offer"}, {"cands", list}}, [](const nlohmann::json&) {}, 5000);
    });
}

void Client::on_direct_event(const nlohmann::json& j) {
    if (!direct_.running()) return;  // relay-only: the keys of a plain-text or opted-out session are never used
    const uint32_t peer = j.value("vip", 0u);
    if (j.value("ev", "") == "direct_drop") return direct_.drop_peer(peer);
    const std::string hex = j.value("key", "");
    if (hex.size() != 64 || hex.find_first_not_of("0123456789abcdef") != std::string::npos) return;
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = static_cast<uint8_t>(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
    std::vector<direct::Cand> cands;
    for (const auto& c : j.value("cands", nlohmann::json::array())) {
        if (!c.is_object() || !c.contains("a") || !c["a"].is_string()) continue;
        const std::string a = c["a"].get<std::string>(), t = c.value("t", "");
        const size_t colon = a.rfind(':');
        uint32_t ip;
        if (colon == std::string::npos || !parse_ipv4(a.substr(0, colon), &ip) || (t != "host" && t != "srflx"))
            continue;
        const int port = std::atoi(a.c_str() + colon + 1);
        if (port <= 0 || port > 65535) continue;
        cands.push_back({{ip, static_cast<uint16_t>(port)}, t == "host" ? direct::CandType::Host : direct::CandType::Srflx});
    }
    if (cands.size() > 4) cands.resize(4);
    direct_.add_peer(vip_, peer, j.value("pair", 0u), key, std::move(cands));
    crypto_wipe(key, sizeof key);
}

void Client::ticker() {
    int64_t last_ping = 0, last_report = now_ms();
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return !running_; });
        }
        if (!running_) return;
        std::vector<Done> expired;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto now = Clock::now();
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (it->second.deadline <= now) {
                    expired.push_back(std::move(it->second.done));
                    it = pending_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (Done& d : expired) fail(d);
        const int64_t t = now_ms();
        if (t - last_rx_ms_ > opt_.dead_after_ms) {
            size_t pending;
            {
                std::lock_guard<std::mutex> lock(mu_);
                pending = pending_.size();
            }
            std::fprintf(stderr, "[INFRA] no frame for %lld ms (last ping sent %lld ms ago, %zu requests pending)\n",
                         static_cast<long long>(t - last_rx_ms_), static_cast<long long>(t - last_ping), pending);
            lost("server silent");
            return;
        }
        if (t - last_ping >= opt_.ping_interval_ms) {
            last_ping = t;
            {
                std::lock_guard<std::mutex> lock(mu_);
                pings_out_.insert(t);
            }
            request({{"op", "ping"}, {"t", t}},
                    [this, t](const nlohmann::json& j) {
                        std::lock_guard<std::mutex> lock(mu_);
                        auto it = pings_out_.find(t);
                        if (it != pings_out_.end()) pings_out_.erase(it);
                        if (!j.value("ok", false)) return;
                        const int rtt = static_cast<int>(now_ms() - t);
                        rtts_.push_back(rtt);
                        if (rtts_.size() > 5) rtts_.pop_front();
                        link_.ping_rtt(rtt);
                    },
                    opt_.dead_after_ms);
        }
        if (opt_.link_report_ms > 0 && t - last_report >= opt_.link_report_ms) {
            last_report = t;
            report_link(nullptr, opt_.dead_after_ms);  // fire and forget: the server overwrites the row
        }
    }
}

Client& client() {
    // Leaked on purpose: no destructor joining network threads during exit.
    static Client* c = [] {
        Client::Options o;
        o.lab_hooks = g_lab_hooks;
        return new Client(parse_address(std::getenv("PSPRECOMP_INFRA_SERVER")), o);
    }();
    return *c;
}

}  // namespace infra
