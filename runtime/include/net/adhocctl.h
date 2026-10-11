#pragma once
// Ad hoc control (sceNetAdhocctl) over a PRO ad hoc server: library state,
// group create/connect/join/scan/disconnect, handler registry and the
// events games receive through their adhocctl handlers. Behaviour follows
// PPSSPP Core/HLE/sceNetAdhoc.cpp. Guest code is never called from here:
// events go to the notifier the HLE layer installs.
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/adhoc_config.h"
#include "net/pro_client.h"

namespace adhocctl {

enum Event : int { EVENT_ERROR = 0, EVENT_CONNECT = 1, EVENT_DISCONNECT = 2, EVENT_SCAN = 3, EVENT_GAME = 4 };
enum State : int { STATE_DISCONNECTED = 0, STATE_CONNECTED = 1, STATE_SCANNING = 2 };
enum class Mode { Create, Connect, Join };

constexpr int32_t kErrAlreadyConnected = (int32_t)0x80410B02;
constexpr int32_t kErrInvalidArg = (int32_t)0x80410B04;
constexpr int32_t kErrAlreadyInitialized = (int32_t)0x80410B07;
constexpr int32_t kErrNotInitialized = (int32_t)0x80410B08;
constexpr int32_t kErrBusy = (int32_t)0x80410B10;
constexpr int32_t kErrTooManyHandlers = (int32_t)0x80410B12;
constexpr int32_t kAdhocErrInvalidArg = (int32_t)0x80410701;
constexpr size_t kMaxHandlers = 32;
constexpr int kJoinWaitMs = 5000;  // JOIN reports CONNECT once a peer is known or after this

struct Handler {
    uint32_t entry;
    uint32_t arg;
};

struct ScanResult {
    std::string group;
    pro::Mac host{};
};

class Adhocctl {
public:
    // Called (on the event thread) for every handler when an event fires.
    using Notifier = std::function<void(const Handler& h, int event, int error)>;

    ~Adhocctl() { term(); }

    void set_notifier(Notifier n);
    // Logs in to the server. Like PPSSPP the library is initialised even if
    // the server cannot be reached; connecting then never completes.
    int init(const AdhocConfig& cfg, const std::string& product);
    int term();
    bool inited() const;

    int connect(const std::string& group, Mode mode);
    int disconnect();
    int scan();

    // PPSSPP: same entry -> 0 without adding; bad entry -> INVALID_ARG.
    int add_handler(uint32_t entry, uint32_t arg, bool entry_valid);
    int del_handler(int id);

    State state() const;
    std::string group() const;
    std::vector<ProClient::Peer> peers() const;
    std::vector<ScanResult> scan_results() const;
    bool server_connected() const { return client_.connected(); }

    // Peer addressing for PDP/PTP: IPs are wire order (see pro::Event::ip).
    bool resolve_mac(const pro::Mac& mac, uint32_t* ip) const;
    bool resolve_ip(uint32_t ip, pro::Mac* mac) const;
    pro::Mac local_mac() const;
    uint16_t port_offset() const;
    std::string bind_ip() const;

private:
    void pump();
    void fire(int event, int error);
    static bool valid_group(const std::string& g);

    mutable std::mutex mu_;
    Notifier notifier_;
    AdhocConfig cfg_;
    std::string product_;
    ProClient client_;
    std::thread pump_thread_;
    bool inited_ = false;
    bool running_ = false;
    bool busy_ = false;
    bool pending_join_ = false;
    std::chrono::steady_clock::time_point join_start_{};
    State state_ = STATE_DISCONNECTED;
    Mode mode_ = Mode::Connect;
    std::string group_;
    std::map<int, Handler> handlers_;
    std::vector<ScanResult> scan_new_, scan_results_;
};

Adhocctl& instance();

}  // namespace adhocctl
