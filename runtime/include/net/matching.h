#pragma once
// PSP ad hoc matching context (sceNetAdhocMatching): the lobby protocol
// games run over PDP. A port of PPSSPP's state machine
// (Core/HLE/sceNetAdhocMatching.cpp actOn*/send*, proAdhoc.cpp helpers).
// Pure: packets leave through Io::send, game events through Io::notify,
// time comes from the caller. Not thread-safe; the runtime driver locks.
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "net/matching_packet.h"
#include "net/pro_protocol.h"

namespace matching {

enum Mode : int { PARENT = 1, CHILD = 2, P2P = 3 };

enum Event : int {
    EV_HELLO = 1,
    EV_REQUEST = 2,
    EV_LEAVE = 3,
    EV_DENY = 4,
    EV_CANCEL = 5,
    EV_ACCEPT = 6,
    EV_ESTABLISHED = 7,
    EV_TIMEOUT = 8,
    EV_ERROR = 9,
    EV_BYE = 10,
    EV_DATA = 11,
};

enum PeerState : int {
    PEER_SELF = 0,  // a CHILD's own entry after being accepted
    PEER_OFFER = 1,
    PEER_PARENT = 2,
    PEER_CHILD = 3,
    PEER_P2P = 4,
    PEER_INCOMING_REQUEST = 5,
    PEER_OUTGOING_REQUEST = 6,
    PEER_CANCEL_IN_PROGRESS = 7,
};

constexpr int32_t kErrUnknownTarget = (int32_t)0x8041080C;
constexpr int32_t kErrTargetNotReady = (int32_t)0x8041080D;
constexpr int32_t kErrExceedMaxnum = (int32_t)0x8041080E;
constexpr int32_t kErrRequestInProgress = (int32_t)0x8041080F;
constexpr int32_t kErrAlreadyEstablished = (int32_t)0x80410810;
constexpr int32_t kErrInvalidMode = (int32_t)0x80410801;
constexpr int32_t kErrNotRunning = (int32_t)0x8041080B;
constexpr uint32_t kDefaultKeepaliveUs = 2000000;  // PSP_ADHOCCTL_PING_TIMEOUT

struct Io {
    std::function<void(const pro::Mac& to, uint16_t port, const std::vector<uint8_t>& packet)> send;
    std::function<std::vector<pro::Mac>()> group_peers;  // everyone in the adhocctl group
    std::function<void(int event, const pro::Mac& peer, const std::vector<uint8_t>& opt)> notify;
};

class Context {
public:
    Context(Mode mode, int maxpeers, uint16_t port, uint32_t hello_us, int32_t keepalive_us,
            int init_count, uint32_t rexmt_us, const pro::Mac& self, Io io);

    void start(std::vector<uint8_t> hello_opt);
    void stop();  // BYE to established peers, then forget them
    bool running() const { return running_; }

    int set_hello_opt(std::vector<uint8_t> opt);
    const std::vector<uint8_t>& hello_opt() const { return hello_; }
    int select_target(const pro::Mac& peer, const std::vector<uint8_t>& opt, uint64_t now_us);
    int cancel_target(const pro::Mac& peer, const std::vector<uint8_t>& opt, uint64_t now_us);

    void on_packet(const pro::Mac& from, uint16_t from_port, const uint8_t* data, size_t len,
                   uint64_t now_us);
    void tick(uint64_t now_us);  // hello and keepalive broadcasts, timeouts

    // sceNetAdhocMatchingGetMembers order (PPSSPP).
    std::vector<pro::Mac> members() const;
    int connected_count() const;  // countConnectedPeers
    int peer_state(const pro::Mac& mac) const;  // -1 if unknown

    Mode mode() const { return mode_; }
    uint16_t port() const { return port_; }

private:
    struct Member {
        pro::Mac mac;
        int state;
        uint64_t lastping;
    };

    Member* find(const pro::Mac& mac);
    const Member* find(const pro::Mac& mac) const;
    Member* find_state(int state);
    const Member* find_state(int state) const;
    int count_children() const;
    void erase(const pro::Mac& mac);
    void add_front(const pro::Mac& mac, int state, uint64_t now);
    uint16_t port_of(const pro::Mac& mac) const;
    void send_to(const pro::Mac& mac, const Packet& p);
    void notify(int event, const pro::Mac& mac, const std::vector<uint8_t>& opt = {});

    void send_accept(const pro::Mac& mac, const std::vector<uint8_t>& opt);
    void send_join(const pro::Mac& mac, const std::vector<uint8_t>& opt);
    void send_cancel(const pro::Mac& mac, const std::vector<uint8_t>& opt, uint64_t now);
    void send_birth(const pro::Mac& mac);
    void send_death(const pro::Mac& mac);
    void handle_timeout(uint64_t now);

    void on_hello(const pro::Mac& from, const Packet& p, uint64_t now);
    void on_join(const pro::Mac& from, const Packet* p, uint64_t now);
    void on_accept(const pro::Mac& from, const Packet& p, uint64_t now);
    void on_cancel(const pro::Mac& from, const Packet& p);
    void on_bulk(const pro::Mac& from, const Packet& p);
    void on_birth(const pro::Mac& from, const Packet& p, uint64_t now);
    void on_death(const pro::Mac& from, const Packet& p);
    void on_bye(const pro::Mac& from);

    Mode mode_;
    int maxpeers_;
    uint16_t port_;
    uint32_t hello_us_;
    uint32_t keepalive_us_;
    uint64_t timeout_us_;
    pro::Mac self_;
    Io io_;
    bool running_ = false;
    std::vector<uint8_t> hello_;
    // Newest first, like PPSSPP's linked list (order matters for ACCEPT
    // sibling lists and GetMembers).
    std::vector<Member> peers_;
    std::map<pro::Mac, uint16_t> peer_port_;
    uint64_t last_hello_ = 0, last_ping_ = 0;
    bool first_tick_ = true;
};

}  // namespace matching
