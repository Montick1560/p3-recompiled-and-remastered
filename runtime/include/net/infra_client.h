#pragma once
// Client of the infrastructure-mode room server (tools/infra_server.py): one
// connection (raw TCP, or WebSocket through WinHTTP on Windows for the public
// server behind a Cloudflare Tunnel), JSON request/answer with timeouts,
// pushed events, game datagrams queued per local port and the RTT from pings.
// All network waits happen on the client's own threads, never on the guest
// CPU; callbacks run on those threads. Thread-safe.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "net/infra_protocol.h"

namespace infra {

// PSP NP Matching2 error a request gets on timeout or when the server is gone.
constexpr uint32_t kErrTimeout = 0x80550C3C;
// SCE_NP_MATCHING2_EVENT_CAUSE_CONNECTION_ERROR, for the synthesised room_destroyed.
constexpr int kCauseConnectionError = 7;

struct Address {
    enum Kind { Off, Tcp, Ws, Wss } kind = Off;
    std::string host;
    uint16_t port = 0;
    std::string path;
};

// PSPRECOMP_INFRA_SERVER: unset/empty -> wss://online.montick.app/infra;
// "off" -> Off; "host[:port]" / "tcp://host[:port]" -> Tcp (27320);
// "ws://host[:port][/path]" (80) and "wss://host[:port][/path]" (443), path
// "/infra" by default. Anything malformed -> Off (logged).
Address parse_address(const char* text);
std::string to_string(const Address& a);

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool open(const Address& a, int timeout_ms) = 0;
    virtual bool send_frame(const std::vector<uint8_t>& frame) = 0;
    // Waits up to timeout_ms and appends what arrived (possibly nothing).
    // false once the connection is closed or failed.
    virtual bool recv(std::vector<uint8_t>* bytes, int timeout_ms) = 0;
    // Unblocks recv from another thread; the connection is unusable after it.
    virtual void close() = 0;
};
std::unique_ptr<Transport> make_transport(Address::Kind kind);

class Client {
public:
    struct Options {
        int ping_interval_ms = 2000;
        int dead_after_ms = 10000;  // no frame from the server for this long -> connection lost
    };
    struct Datagram {
        uint32_t vip = 0;
        uint16_t src_port = 0;
        std::vector<uint8_t> payload;
    };
    using Done = std::function<void(const nlohmann::json& answer)>;
    using EventFn = std::function<void(const nlohmann::json& event)>;
    static constexpr size_t kQueueCap = 256;

    explicit Client(Address addr) : Client(std::move(addr), Options()) {}
    Client(Address addr, Options opt);
    ~Client();

    bool configured() const { return addr_.kind != Address::Off; }
    const Address& address() const { return addr_; }
    // Opens the connection and logs in (blocking up to timeout_ms; call it off
    // the guest CPU). true when logged in, also when already connected.
    bool connect(const std::string& online_id, int timeout_ms);
    void disconnect();
    bool connected() const { return connected_; }

    // Sends req with a fresh "id". done runs exactly once with the server's
    // answer, or {"ok":false,"error":kErrTimeout} on timeout / connection loss
    // (inline when not connected).
    void request(nlohmann::json req, Done done, int timeout_ms);
    void on_event(EventFn fn);

    void send_datagram(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t* data, size_t len);
    bool pop_datagram(uint16_t local_port, Datagram* out);
    bool has_datagram(uint16_t local_port);

    int rtt_ms() const;  // median of the last 5 pings, -1 before the first answer
    uint32_t vip() const { return vip_; }
    std::string session() const;
    std::string online_id() const;

private:
    struct Pending {
        Done done;
        std::chrono::steady_clock::time_point deadline;
        std::string op;
    };
    void reader();
    void ticker();
    bool send(const std::vector<uint8_t>& frame);
    void on_json(const nlohmann::json& j);
    void lost(const char* why);
    void fail(Done& done);

    Address addr_;
    Options opt_;
    std::unique_ptr<Transport> transport_;
    std::thread reader_, ticker_;
    std::atomic<bool> running_{false}, connected_{false};
    std::atomic<uint32_t> vip_{0};
    std::atomic<int64_t> last_rx_ms_{0};
    std::mutex connect_mu_;  // one connect / disconnect at a time
    std::mutex send_mu_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    uint32_t next_id_ = 0;
    std::map<uint32_t, Pending> pending_;
    EventFn on_event_;
    std::map<uint16_t, std::deque<Datagram>> queues_;
    std::deque<int> rtts_;
    std::string session_, online_id_;
    int64_t room_id_ = 0;  // joined room, for the room_destroyed synthesised on loss
};

// Process-wide client for PSPRECOMP_INFRA_SERVER (read once).
Client& client();

}  // namespace infra
