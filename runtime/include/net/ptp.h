#pragma once
// PSP ad hoc PTP (reliable stream) sockets carried as host TCP, PPSSPP
// style: a socket is bound to PSP port + port offset on this instance's
// address and connects to the peer's IP at its PSP port + offset; an
// accepted connection is only kept if its source IP belongs to a peer.
#include <array>
#include <cstdint>
#include <mutex>

#include "net/pdp.h"  // Resolver, Peer, shared error codes

namespace ptp {

constexpr int32_t kErrInvalidSocketId = pdp::kErrInvalidSocketId;
constexpr int32_t kErrInvalidAddr = pdp::kErrInvalidAddr;
constexpr int32_t kErrInvalidArg = pdp::kErrInvalidArg;
constexpr int32_t kErrWouldBlock = pdp::kErrWouldBlock;
constexpr int32_t kErrPortInUse = pdp::kErrPortInUse;
constexpr int32_t kErrNotConnected = (int32_t)0x8041070B;
constexpr int32_t kErrDisconnected = (int32_t)0x8041070C;
constexpr int32_t kErrNotOpened = (int32_t)0x8040070D;
constexpr int32_t kErrNotListened = (int32_t)0x8040070E;
constexpr int32_t kErrSocketIdNotAvail = (int32_t)0x8041070F;
constexpr int32_t kErrPortNotAvail = pdp::kErrPortNotAvail;
constexpr int32_t kErrTimeout = pdp::kErrTimeout;

enum State { CLOSED = 0, LISTEN = 1, SYN_SENT = 2, ESTABLISHED = 4 };

class Sockets {
public:
    void set_resolver(pdp::Resolver r);

    // Open a socket that will connect to (peer, dport). sport 0 = any port.
    int open(uint16_t sport, const pro::Mac& peer, uint16_t dport, int bufsize);
    int listen(uint16_t sport, int bufsize, int backlog);
    // Blocking: retries (also refused attempts) until timeout_us, 0 = no
    // limit. Nonblocking: WOULD_BLOCK while the connection is in progress.
    int connect(int id, uint32_t timeout_us, bool nonblock);
    // New socket id, peer MAC and PSP port of the connecting side.
    int accept(int id, pro::Mac* peer, uint16_t* peer_port, uint32_t timeout_us, bool nonblock);
    int send(int id, const uint8_t* data, int* len, uint32_t timeout_us, bool nonblock);
    int recv(int id, uint8_t* buf, int* len, uint32_t timeout_us, bool nonblock);
    int flush(int id);
    int close(int id);
    void close_all();

    int state(int id) const;
    uint16_t local_port(int id) const;

private:
    struct Slot {
        int fd = -1;
        int state = CLOSED;
        uint16_t lport = 0;
        pro::Mac peer{};
        uint16_t pport = 0;
        int bufsize = 0;
    };
    bool peer_ip(const pro::Mac& mac, uint32_t* ip) const;
    bool peer_mac(uint32_t ip, pro::Mac* mac) const;
    int new_socket(uint16_t sport, int bufsize, uint16_t* bound_port) const;

    mutable std::mutex mu_;
    pdp::Resolver resolver_;
    std::array<Slot, pdp::kMaxSockets> slots_{};
};

Sockets& instance();

}  // namespace ptp
