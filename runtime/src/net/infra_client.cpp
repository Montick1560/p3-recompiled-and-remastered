// Client of the infrastructure-mode room server (see net/infra_client.h).
#include "net/infra_client.h"

#include "psp_socket_compat.h"  // before windows.h (winsock2 first)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "net/relay_conn.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace infra {

namespace {

using Clock = std::chrono::steady_clock;

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
    if (reader_.joinable()) reader_.join();
    if (ticker_.joinable()) ticker_.join();
    {
        std::lock_guard<std::mutex> slock(send_mu_);
        transport_ = make_transport(addr_.kind);
    }
    if (!transport_->open(addr_, timeout_ms)) {
        std::fprintf(stderr, "[INFRA] cannot reach %s\n", to_string(addr_).c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        room_id_ = 0;
        rtts_.clear();
        connected_ = true;
    }
    running_ = true;
    last_rx_ms_ = now_ms();
    reader_ = std::thread([this] { reader(); });
    ticker_ = std::thread([this] { ticker(); });

    std::mutex wmu;
    std::condition_variable wcv;
    bool done = false;
    nlohmann::json answer;
    nlohmann::json login = login_extra.is_object() ? login_extra : nlohmann::json::object();
    login["op"] = "login";
    login["online_id"] = online_id;
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
        std::fprintf(stderr, "[INFRA] login to %s as %s failed: %s\n", to_string(addr_).c_str(), online_id.c_str(),
                     answer.dump().c_str());
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
    return true;
}

void Client::disconnect() {
    std::lock_guard<std::mutex> clock(connect_mu_);
    lost("disconnect");
    if (reader_.joinable()) reader_.join();
    if (ticker_.joinable()) ticker_.join();
    std::lock_guard<std::mutex> slock(send_mu_);
    transport_.reset();
}

void Client::fail(Done& done) {
    if (done) done(nlohmann::json{{"ok", false}, {"error", kErrTimeout}});
}

void Client::lost(const char* why) {
    std::map<uint32_t, Pending> pending;
    int64_t room = 0;
    EventFn fn;
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        if (!connected_) return;
        connected_ = false;
        pending.swap(pending_);
        room = room_id_;
        room_id_ = 0;
        fn = on_event_;
        queues_.clear();
        streams_.clear();
        queued_bytes_ = stream_bytes_ = 0;
    }
    cv_.notify_all();
    if (transport_) transport_->close();
    if (std::strcmp(why, "disconnect") != 0) std::fprintf(stderr, "[INFRA] connection lost: %s\n", why);
    for (auto& [id, p] : pending) fail(p.done);
    if (room && fn) fn(nlohmann::json{{"ev", "room_destroyed"}, {"room_id", room}, {"cause", kCauseConnectionError}});
}

bool Client::send(const std::vector<uint8_t>& frame) {
    bool ok;
    {
        std::lock_guard<std::mutex> lock(send_mu_);
        ok = transport_ && transport_->send_frame(frame);
    }
    if (!ok) lost("send failed");
    return ok;
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
        }
    }
    if (id == 0) {
        fail(done);
        return;
    }
    req["id"] = id;
    send(encode_json(req));
}

void Client::on_event(EventFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    on_event_ = std::move(fn);
}

void Client::send_datagram(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t* data, size_t len) {
    if (connected_) send(encode_datagram(vip, dst_port, src_port, data, len));
}

bool Client::pop_datagram(uint16_t local_port, Datagram* out) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(local_port);
    if (it == queues_.end() || it->second.empty()) return false;
    *out = std::move(it->second.front());
    it->second.pop_front();
    queued_bytes_ -= out->payload.size();
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
    if (connected_) send(encode_segment(kKindStream, vip, dst_port, src_port, data, len));
}

bool Client::pop_stream(Datagram* out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (streams_.empty()) return false;
    *out = std::move(streams_.front());
    streams_.pop_front();
    stream_bytes_ -= out->payload.size();
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
        EventFn fn;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const std::string ev = j.value("ev", "");
            if ((ev == "room_destroyed" || ev == "kicked") && j.value("room_id", int64_t{0}) == room_id_)
                room_id_ = 0;
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
            room_id_ = j["room"].value("id", int64_t{0});
        if (ok && it->second.op == "leave") room_id_ = 0;
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
        last_rx_ms_ = now_ms();
        r.feed(bytes.data(), bytes.size());
        Frame f;
        while (r.next(&f)) {
            if (f.kind == kKindJson) {
                on_json(f.json);
                continue;
            }
            std::lock_guard<std::mutex> lock(mu_);
            if (f.kind == kKindStream) {
                if (streams_.size() >= kStreamQueueCap || stream_bytes_ + f.payload.size() > kMaxStreamBytes) {
                    static int logged = 0;
                    if (logged++ < 4) std::fprintf(stderr, "[INFRA] stream queue full: a game stream lost data\n");
                    continue;
                }
                stream_bytes_ += f.payload.size();
                streams_.push_back(Datagram{f.vip, f.src_port, f.dst_port, std::move(f.payload)});
                continue;
            }
            if (queued_bytes_ + f.payload.size() > kMaxQueuedBytes) continue;  // nobody reads them: drop new
            auto& q = queues_[f.dst_port];
            queued_bytes_ += f.payload.size();
            q.push_back(Datagram{f.vip, f.src_port, f.dst_port, std::move(f.payload)});
            if (q.size() > kQueueCap) {
                queued_bytes_ -= q.front().payload.size();
                q.pop_front();
            }
        }
        if (r.broken()) {
            lost("bad frame from the server");
            return;
        }
    }
}

void Client::ticker() {
    int64_t last_ping = 0;
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
            request({{"op", "ping"}, {"t", t}},
                    [this, t](const nlohmann::json& j) {
                        if (!j.value("ok", false)) return;
                        std::lock_guard<std::mutex> lock(mu_);
                        rtts_.push_back(static_cast<int>(now_ms() - t));
                        if (rtts_.size() > 5) rtts_.pop_front();
                    },
                    opt_.dead_after_ms);
        }
    }
}

Client& client() {
    // Leaked on purpose: no destructor joining network threads during exit.
    static Client* c = new Client(parse_address(std::getenv("PSPRECOMP_INFRA_SERVER")));
    return *c;
}

}  // namespace infra
