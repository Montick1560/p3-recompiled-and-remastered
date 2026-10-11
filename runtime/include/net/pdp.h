#pragma once
// PSP ad hoc PDP (datagram) sockets carried as host UDP, PPSSPP style: a
// socket bound to PSP port + port offset on this instance's address; a peer
// MAC is sent to the IP the PRO server reported for it, and a datagram is
// only accepted from an IP that belongs to a peer (others are dropped).
// In relay mode each socket is instead a TCP session to the aemu_postoffice
// relay, which forwards datagrams by (MAC, port).
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "net/pro_protocol.h"
#include "net/relay_protocol.h"

namespace pdp {

constexpr int32_t kErrInvalidSocketId = (int32_t)0x80410701;
constexpr int32_t kErrInvalidAddr = (int32_t)0x80410702;
constexpr int32_t kErrInvalidPort = (int32_t)0x80410703;
constexpr int32_t kErrInvalidDatalen = (int32_t)0x80410705;
constexpr int32_t kErrNotEnoughSpace = (int32_t)0x80400706;
constexpr int32_t kErrWouldBlock = (int32_t)0x80410709;
constexpr int32_t kErrPortInUse = (int32_t)0x8041070A;
constexpr int32_t kErrPortNotAvail = (int32_t)0x80410710;
constexpr int32_t kErrInvalidArg = (int32_t)0x80410711;
constexpr int32_t kErrTimeout = (int32_t)0x80410715;
constexpr int kMaxSockets = 255;

struct Peer {
    pro::Mac mac;
    uint32_t ip;  // wire order
};

// Where peers are; the runtime wires these to adhocctl, tests to fakes.
struct Resolver {
    std::function<std::vector<Peer>()> peers;
    std::string bind_ip;      // empty = any address
    uint16_t port_offset = 10000;
    // Relay mode (aemu_postoffice): every socket is a TCP session to
    // relay_host:relay_port that registers this console's MAC and the port.
    bool relay = false;
    std::string relay_host;
    uint16_t relay_port = 27313;
    pro::Mac self_mac{};
};

class Sockets {
public:
    void set_resolver(Resolver r);

    // id 1..255, or PORT_IN_USE / PORT_NOT_AVAIL / INVALID_ARG. Port 0 picks a free one.
    int create(uint16_t port, int bufsize);
    int remove(int id);
    // Unknown peers are skipped silently (PPSSPP fakes success); the
    // broadcast MAC sends to every peer.
    int send(int id, const pro::Mac& to, uint16_t port, const uint8_t* data, int len);
    // nonblock: WOULD_BLOCK when nothing is queued. Blocking: waits up to
    // timeout_us (0 = no limit) then TIMEOUT. *len: in buffer size, out
    // datagram size; NOT_ENOUGH_SPACE leaves the datagram queued.
    int recv(int id, pro::Mac* from, uint16_t* from_port, uint8_t* buf, int* len,
             uint32_t timeout_us, bool nonblock);
    uint16_t local_port(int id) const;  // PSP port (host port - offset)
    void close_all();

private:
    struct Slot {
        int fd = -1;
        uint16_t port = 0;
        bool relay = false;  // fd is a relay session
        bool dead = false;   // the relay closed it
        relay::PdpFrames rx;
    };
    int fd_of(int id) const;
    bool peer_of_ip(uint32_t ip, pro::Mac* mac) const;
    int create_relay(uint16_t port, const Resolver& r);
    int send_relay(int fd, const pro::Mac& to, uint16_t port, const uint8_t* data, int len);
    int recv_relay(int id, pro::Mac* from, uint16_t* from_port, uint8_t* buf, int* len,
                   uint32_t timeout_us, bool nonblock);

    mutable std::mutex mu_;
    std::mutex relay_send_mu_;  // one frame at a time on a relay session
    Resolver resolver_;
    std::array<Slot, kMaxSockets> slots_{};
};

Sockets& instance();

}  // namespace pdp
