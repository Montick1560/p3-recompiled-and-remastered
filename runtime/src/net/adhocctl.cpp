// sceNetAdhocctl core. States, return codes and event rules follow PPSSPP
// Core/HLE/sceNetAdhoc.cpp (NetAdhocctl_Create/Disconnect/Scan,
// __NetTriggerCallbacks) and proAdhoc.cpp (friendFinder); GPL-2.0-or-later.
#include "net/adhocctl.h"

#include <cstdio>

namespace adhocctl {

static const char* event_name(int event) {
    switch (event) {
    case EVENT_ERROR: return "ERROR";
    case EVENT_CONNECT: return "CONNECT";
    case EVENT_DISCONNECT: return "DISCONNECT";
    case EVENT_SCAN: return "SCAN";
    case EVENT_GAME: return "GAME";
    default: return "?";
    }
}

void Adhocctl::set_notifier(Notifier n) {
    std::lock_guard<std::mutex> lock(mu_);
    notifier_ = std::move(n);
}

bool Adhocctl::valid_group(const std::string& g) {
    for (size_t i = 0; i < g.size() && i < pro::kGroupLen; i++) {
        const char c = g[i];
        if (c == 0) break;
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum) return false;
    }
    return true;
}

int Adhocctl::init(const AdhocConfig& cfg, const std::string& product) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (inited_) return kErrAlreadyInitialized;
        inited_ = true;
        running_ = true;
        cfg_ = cfg;
        product_ = product;
        busy_ = false;
        pending_join_ = false;
        state_ = STATE_DISCONNECTED;
        group_.clear();
        scan_new_.clear();
        scan_results_.clear();
    }
    if (!client_.start(cfg.server_host, cfg.server_port, cfg.mac, cfg.nickname, product, cfg.bind_ip)) {
        std::fprintf(stderr, "[ADHOC] adhocctl initialised without a server (%s:%u unreachable)\n",
                     cfg.server_host.c_str(), cfg.server_port);
    }
    pump_thread_ = std::thread(&Adhocctl::pump, this);
    return 0;
}

int Adhocctl::term() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!inited_) return 0;
        if (state_ == STATE_CONNECTED || busy_) {
            if (!group_.empty() || state_ == STATE_CONNECTED) client_.send_disconnect();
        }
        running_ = false;
    }
    if (pump_thread_.joinable()) pump_thread_.join();
    client_.stop();
    std::lock_guard<std::mutex> lock(mu_);
    handlers_.clear();
    state_ = STATE_DISCONNECTED;
    busy_ = false;
    pending_join_ = false;
    group_.clear();
    inited_ = false;
    return 0;
}

bool Adhocctl::inited() const {
    std::lock_guard<std::mutex> lock(mu_);
    return inited_;
}

int Adhocctl::connect(const std::string& group, Mode mode) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!inited_) return kErrNotInitialized;
        if (!valid_group(group)) return kAdhocErrInvalidArg;
        if (state_ == STATE_DISCONNECTED && !busy_) {
            busy_ = true;
            pending_join_ = false;
            mode_ = mode;
            group_ = group.substr(0, pro::kGroupLen);
            if (!client_.connected()) {
                // PPSSPP isAdhocctlNeedLogin: log in again after a lost connection.
                client_.start(cfg_.server_host, cfg_.server_port, cfg_.mac, cfg_.nickname, product_, cfg_.bind_ip);
            }
            const bool sent = client_.send_connect(group_);
            std::fprintf(stderr, "[ADHOC] %s group \"%s\"%s\n",
                         mode == Mode::Create ? "create" : mode == Mode::Join ? "join" : "connect",
                         group_.c_str(), sent ? "" : " (no server)");
            return 0;
        }
        if (state_ != STATE_CONNECTED) return kErrBusy;
    }
    // Already in a group: PPSSPP reports ALREADY_CONNECTED through the handlers.
    fire(EVENT_ERROR, kErrAlreadyConnected);
    return 0;
}

int Adhocctl::disconnect() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!inited_) return kErrNotInitialized;
        // The server logs out a client that leaves without being in a group.
        if (state_ == STATE_CONNECTED || (busy_ && state_ != STATE_SCANNING)) client_.send_disconnect();
        group_.clear();
        busy_ = false;
        pending_join_ = false;
        state_ = STATE_DISCONNECTED;
        client_.clear_peers();
    }
    std::fprintf(stderr, "[ADHOC] disconnect\n");
    fire(EVENT_DISCONNECT, 0);  // even when not connected (PPSSPP: God Eater)
    return 0;
}

int Adhocctl::scan() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!inited_) return kErrNotInitialized;
        if (state_ == STATE_DISCONNECTED && !busy_) {
            busy_ = true;
            state_ = STATE_SCANNING;
            scan_new_.clear();
            client_.send_scan();
            return 0;
        }
        if (state_ != STATE_CONNECTED) return kErrBusy;
    }
    fire(EVENT_ERROR, kErrAlreadyConnected);
    return 0;
}

int Adhocctl::add_handler(uint32_t entry, uint32_t arg, bool entry_valid) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& kv : handlers_) {
        if (kv.second.entry == entry) return 0;  // PPSSPP fakes success
    }
    if (!entry_valid) return kErrInvalidArg;
    if (handlers_.size() >= kMaxHandlers) return kErrTooManyHandlers;
    int id = 0;
    while (handlers_.count(id)) id++;
    handlers_[id] = Handler{entry, arg};
    return id;
}

int Adhocctl::del_handler(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!inited_) return kErrNotInitialized;
    handlers_.erase(id);
    return 0;
}

State Adhocctl::state() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

std::string Adhocctl::group() const {
    std::lock_guard<std::mutex> lock(mu_);
    return group_;
}

std::vector<ProClient::Peer> Adhocctl::peers() const { return client_.peers(); }

std::vector<ScanResult> Adhocctl::scan_results() const {
    std::lock_guard<std::mutex> lock(mu_);
    return scan_results_;
}

bool Adhocctl::resolve_mac(const pro::Mac& mac, uint32_t* ip) const {
    for (const auto& p : client_.peers()) {
        if (p.mac == mac) {
            *ip = p.ip;
            return true;
        }
    }
    return false;
}

bool Adhocctl::resolve_ip(uint32_t ip, pro::Mac* mac) const {
    for (const auto& p : client_.peers()) {
        if (p.ip == ip) {
            *mac = p.mac;
            return true;
        }
    }
    return false;
}

pro::Mac Adhocctl::local_mac() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_.mac;
}

uint16_t Adhocctl::port_offset() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_.port_offset;
}

std::string Adhocctl::bind_ip() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_.bind_ip;
}

void Adhocctl::fire(int event, int error) {
    std::vector<Handler> hs;
    Notifier n;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& kv : handlers_) hs.push_back(kv.second);
        n = notifier_;
    }
    std::fprintf(stderr, "[ADHOC] event %s error=0x%08X handlers=%zu\n", event_name(event),
                 static_cast<uint32_t>(error), hs.size());
    if (!n) return;
    for (const auto& h : hs) n(h, event, error);
}

void Adhocctl::pump() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
        }
        pro::Event e;
        const bool got = client_.wait_event(&e, 100);
        int fire_event = -1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const bool join_done =
                pending_join_ &&
                (!client_.peers().empty() ||
                 std::chrono::steady_clock::now() - join_start_ > std::chrono::milliseconds(kJoinWaitMs));
            if (got) {
                switch (e.op) {
                case pro::CONNECT_BSSID:
                    if (busy_ && state_ != STATE_SCANNING) {
                        if (mode_ == Mode::Join && client_.peers().empty()) {
                            pending_join_ = true;
                            join_start_ = std::chrono::steady_clock::now();
                        } else {
                            fire_event = EVENT_CONNECT;
                        }
                    }
                    break;
                case pro::SCAN:
                    scan_new_.push_back(ScanResult{e.group, e.mac});
                    break;
                case pro::SCAN_COMPLETE:
                    if (state_ == STATE_SCANNING) {
                        scan_results_ = scan_new_;
                        scan_new_.clear();
                        fire_event = EVENT_SCAN;
                    }
                    break;
                case pro::CONNECT:
                    std::fprintf(stderr, "[ADHOC] peer \"%s\" joined\n", e.nick.c_str());
                    break;
                case pro::DISCONNECT:
                    if (e.ip == 0) std::fprintf(stderr, "[ADHOC] lost the ad hoc server\n");
                    break;
                default:
                    break;
                }
            }
            if (fire_event < 0 && (join_done || (pending_join_ && !client_.peers().empty()))) {
                fire_event = EVENT_CONNECT;
            }
            if (fire_event == EVENT_CONNECT) {
                pending_join_ = false;
                busy_ = false;
                state_ = STATE_CONNECTED;
                std::fprintf(stderr, "[ADHOC] connected group=%s peers=%zu\n", group_.c_str(),
                             client_.peers().size());
            } else if (fire_event == EVENT_SCAN) {
                busy_ = false;
                state_ = STATE_DISCONNECTED;
            }
        }
        if (fire_event >= 0) fire(fire_event, 0);
    }
}

Adhocctl& instance() {
    static Adhocctl a;
    return a;
}

}  // namespace adhocctl
