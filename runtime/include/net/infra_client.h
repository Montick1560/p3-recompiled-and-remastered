#pragma once
// Client of the infrastructure-mode room server (tools/infra_server.py): one
// connection (raw TCP, or WebSocket through WinHTTP on Windows for the public
// server behind a Cloudflare Tunnel), JSON request/answer with timeouts,
// pushed events, game datagrams queued per local port and the RTT from pings.
// All network waits happen on the client's own threads, never on the guest
// CPU: sends only queue a frame in the outbox, which a writer thread drains in
// order. Callbacks run on the client's threads. Thread-safe.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "net/direct_link.h"
#include "net/infra_protocol.h"
#include "net/link_stats.h"

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

// Wire protocol version sent in the login ("proto"); bumped when the protocol changes. 3: direct path.
constexpr int kProto = 3;
// The login's extra fields: console id, protocol, and the beta invite code
// (PSPRECOMP_INFRA_INVITE, set by the launcher) when there is one.
nlohmann::json login_fields(const std::string& console_hex, const char* invite);
// Name of a beta-gate login refusal of tools/infra_server.py (INVITE_UNKNOWN...), nullptr for others.
const char* login_error_name(uint32_t code);
// Why this client stays relay-only ("opted out", "plain-text server", "no randomness"), nullptr when the direct
// path may be offered: PSPRECOMP_INFRA_DIRECT (env_direct) is not 0/false, the server is wss:// or on loopback
// (keys travel only over TLS), and the OS gives randomness.
const char* direct_off_reason(const Address& a, const char* env_direct, bool have_random);
// Lab hooks (PSPRECOMP_INFRA_DIRECT_IMPAIR, PSPRECOMP_INFRA_DIRECT_TEST_HOSTS) for the process-wide client():
// main() turns them on only outside beta builds. Call before client() is first used.
void set_lab_hooks(bool on);

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
        int link_report_ms = 60000;  // link_stats refresh while connected (the server overwrites the row)
        bool lab_hooks = false;  // honour the direct path's lab env vars (never in beta builds)
        // Makes the transport of each connection; empty -> make_transport (tests inject slow ones).
        std::function<std::unique_ptr<Transport>(Address::Kind)> transport_factory;
    };
    struct Datagram {
        uint32_t vip = 0;
        uint16_t src_port = 0;
        uint16_t dst_port = 0;
        std::vector<uint8_t> payload;
        int64_t rx_us = 0;  // infra::stats::now_us() when the reader got it
    };
    using Done = std::function<void(const nlohmann::json& answer)>;
    using EventFn = std::function<void(const nlohmann::json& event)>;
    static constexpr size_t kQueueCap = 256;
    static constexpr size_t kStreamQueueCap = 16384;  // stream segments must not be lost: far above any burst
    // Bytes a room peer can make this client hold: datagrams nobody reads (all ports together) and stream
    // segments not yet taken by the game. Over the cap new data is dropped.
    static constexpr size_t kMaxQueuedBytes = 1024 * 1024;
    static constexpr size_t kMaxStreamBytes = 8 * 1024 * 1024;
    // Outbox while the transport is slower than the game: over the datagram cap the oldest queued
    // datagrams are dropped (as a full socket buffer would); stream segments and requests are never
    // dropped, and a backlog of them over its cap loses the connection.
    static constexpr size_t kMaxOutboxDgramBytes = 256 * 1024;
    static constexpr size_t kMaxOutboxStreamBytes = 8 * 1024 * 1024;

    explicit Client(Address addr) : Client(std::move(addr), Options()) {}
    Client(Address addr, Options opt);
    ~Client();

    bool configured() const { return addr_.kind != Address::Off; }
    const Address& address() const { return addr_; }
    // Opens the connection and logs in (blocking up to timeout_ms; call it off
    // the guest CPU). true when logged in, also when already connected.
    // login_extra's fields are merged into the login request (e.g. "console").
    bool connect(const std::string& online_id, int timeout_ms,
                 const nlohmann::json& login_extra = nlohmann::json::object());
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
    // Stream socket segments (kind 3), one queue for every port, in arrival order.
    void send_stream(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t* data, size_t len);
    bool pop_stream(Datagram* out);
    size_t queued_bytes() const;
    size_t queued_stream_bytes() const;

    // The direct path: nullptr while it may run, else why it is off (see direct_off_reason, or "server off").
    const char* direct_reason() const { return direct_reason_; }
    bool direct_up(uint32_t peer_vip) const { return direct_.is_up(peer_vip); }

    int rtt_ms() const;  // median of the last 5 pings, -1 before the first answer
    LinkSummary link_summary() const { return link_.summary(); }  // this session's relay-link numbers
    uint32_t vip() const { return vip_; }
    std::string session() const;
    std::string online_id() const;

private:
    struct Pending {
        Done done;
        std::chrono::steady_clock::time_point deadline;
        std::string op;
        int64_t room_id = 0;  // of a leave
    };
    struct Outgoing {
        std::vector<uint8_t> bytes;
        uint8_t kind = 0;  // kKindDatagram / kKindStream (stats rows) / kKindChannel, 0 for JSON
        uint32_t vip = 0;
        uint16_t src = 0, dst = 0;
    };
    static constexpr size_t kFrameHeader = 13;  // u32 length | u8 kind | u32 vip | u16 dst | u16 src
    void reader();
    void ticker();
    void writer();
    void send(Outgoing o);
    void on_json(const nlohmann::json& j);
    void lost(const char* why);
    // Sends this session's link numbers as link_stats; false (done not run) when there is
    // nothing to report or the server refused the op before (an old server).
    bool report_link(Done done, int timeout_ms);
    void log_link() const;
    void fail(Done& done);
    // A received game datagram into its per-port queue (relayed or direct), with the same caps.
    void enqueue_datagram(Datagram d);
    void start_direct(const nlohmann::json& answer);
    void on_direct_event(const nlohmann::json& j);

    Address addr_;
    Options opt_;
    std::unique_ptr<Transport> transport_;
    std::thread reader_, ticker_, writer_;
    std::atomic<bool> running_{false}, connected_{false};
    std::atomic<uint32_t> vip_{0};
    std::atomic<int64_t> last_rx_ms_{0};
    std::mutex connect_mu_;  // one connect / disconnect at a time
    std::mutex out_mu_;  // the outbox; never held while taking mu_
    std::condition_variable out_cv_;
    std::deque<Outgoing> out_;
    size_t out_dgram_bytes_ = 0, out_stream_bytes_ = 0;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    uint32_t next_id_ = 0;
    std::map<uint32_t, Pending> pending_;
    EventFn on_event_;
    std::map<uint16_t, std::deque<Datagram>> queues_;
    std::deque<Datagram> streams_;
    size_t queued_bytes_ = 0, stream_bytes_ = 0;
    std::deque<int> rtts_;
    LinkStats link_;
    std::multiset<int64_t> pings_out_;  // send times (now_ms) of the unanswered pings
    std::atomic<bool> link_refused_{false};
    std::string session_, online_id_;
    std::set<int64_t> rooms_;  // joined rooms (hideout + VS), for the room_destroyed synthesised on loss
    direct::Link direct_;
    std::atomic<const char*> direct_reason_{"not connected"};
    std::thread offer_;  // gathers candidates and sends direct_offer after the login
};

// Process-wide client for PSPRECOMP_INFRA_SERVER (read once).
Client& client();

}  // namespace infra
