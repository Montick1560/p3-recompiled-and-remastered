// PSP ad hoc PDP over host UDP. Addressing, return codes and the dropping of
// datagrams from non-peers follow PPSSPP Core/HLE/sceNetAdhoc.cpp
// (sceNetAdhocPdpCreate/Send/Recv/Delete; GPL-2.0-or-later). In relay
// (aemu_postoffice) mode the datagrams go through the relay instead.
#include "net/pdp.h"
#include "net/adhoc_ids.h"
#include "net/relay_conn.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "psp_socket_compat.h"
#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <fcntl.h>
#include <sys/select.h>
#endif

namespace pdp {

namespace {

constexpr int kSliceMs = 100;
constexpr int kRelayTimeoutMs = 3000;

void set_nonblocking(int fd) {
#ifdef _WIN32
    u_long on = 1;
    ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &on);
    // An ICMP "port unreachable" must not make the next recvfrom fail.
    BOOL report = FALSE;
    DWORD bytes = 0;
    ::WSAIoctl(static_cast<SOCKET>(fd), SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes,
               nullptr, nullptr);
#else
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

bool wait_readable(int fd, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<unsigned>(fd), &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(fd + 1, &set, nullptr, nullptr, &tv) > 0;
}

uint8_t* peek_buffer() {
    static thread_local uint8_t buf[65536];
    return buf;
}

}  // namespace

void Sockets::set_resolver(Resolver r) {
    std::lock_guard<std::mutex> lock(mu_);
    resolver_ = std::move(r);
}

int Sockets::fd_of(int id) const {
    if (id < 1 || id > kMaxSockets) return -1;
    return slots_[id - 1].fd;
}

bool Sockets::peer_of_ip(uint32_t ip, pro::Mac* mac) const {
    std::function<std::vector<Peer>()> peers;
    {
        std::lock_guard<std::mutex> lock(mu_);
        peers = resolver_.peers;
    }
    if (!peers) return false;
    for (const auto& p : peers()) {
        if (p.ip == ip) {
            *mac = p.mac;
            return true;
        }
    }
    return false;
}

int Sockets::create_relay(uint16_t port, const Resolver& r) {
    const auto init = relay::encode_init(relay::INIT_PDP, r.self_mac, relay::wire_port(port, r.port_offset),
                                         pro::Mac{}, 0);
    const int fd = relay::open_session(r.relay_host, r.relay_port, init, kRelayTimeoutMs);
    if (fd < 0) return kErrPortNotAvail;
    std::lock_guard<std::mutex> lock(mu_);
    const int id = adhoc_ids::shared().alloc();
    if (id < 0) {
        psp_sock_close(fd);
        return kErrPortNotAvail;
    }
    Slot& slot = slots_[id - 1];
    slot = Slot{};
    slot.fd = fd;
    slot.port = port;
    slot.relay = true;
    return id;
}

int Sockets::create(uint16_t port, int bufsize) {
    if (bufsize <= 0) return kErrInvalidArg;
    psp_sock_startup();
    Resolver relay_resolver;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (resolver_.relay) {
            if (port != 0) {
                for (const auto& s : slots_) {
                    if (s.fd >= 0 && s.port == port) return kErrPortInUse;
                }
            }
            relay_resolver = resolver_;
        }
    }
    if (relay_resolver.relay) return create_relay(port, relay_resolver);
    std::lock_guard<std::mutex> lock(mu_);
    if (port != 0) {
        for (const auto& s : slots_) {
            if (s.fd >= 0 && s.port == port) return kErrPortInUse;
        }
    }
    const int id = adhoc_ids::shared().alloc();
    if (id < 0) return kErrPortNotAvail;
    const int index = id - 1;
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd < 0) {
        adhoc_ids::shared().release(id);
        return kErrPortNotAvail;
    }
    int rcv = bufsize * 10, snd = bufsize * 5;  // PPSSPP buffer sizing
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&snd), sizeof(snd));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!resolver_.bind_ip.empty()) ::inet_pton(AF_INET, resolver_.bind_ip.c_str(), &addr.sin_addr);
    addr.sin_port = htons(port == 0 ? 0 : static_cast<uint16_t>(port + resolver_.port_offset));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        psp_sock_close(fd);
        adhoc_ids::shared().release(id);
        std::fprintf(stderr, "[ADHOC] PDP port %u (host %u) not available\n", port,
                     static_cast<unsigned>(port + resolver_.port_offset));
        return kErrPortNotAvail;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    set_nonblocking(fd);
    slots_[index] = Slot{};
    slots_[index].fd = fd;
    slots_[index].port = static_cast<uint16_t>(ntohs(addr.sin_port) - resolver_.port_offset);
    return index + 1;
}

int Sockets::remove(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    const int fd = fd_of(id);
    if (fd < 0) return kErrInvalidSocketId;
    psp_sock_close(fd);
    slots_[id - 1] = Slot{};
    adhoc_ids::shared().release(id);
    return 0;
}

void Sockets::close_all() {
    std::lock_guard<std::mutex> lock(mu_);
    for (int i = 0; i < kMaxSockets; i++) {
        if (slots_[i].fd >= 0) {
            psp_sock_close(slots_[i].fd);
            adhoc_ids::shared().release(i + 1);
        }
        slots_[i] = Slot{};
    }
}

uint16_t Sockets::local_port(int id) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_of(id) < 0) return 0;
    return slots_[id - 1].port;
}

int Sockets::send_relay(int fd, const pro::Mac& to, uint16_t port, const uint8_t* data, int len) {
    uint16_t offset;
    {
        std::lock_guard<std::mutex> lock(mu_);
        offset = resolver_.port_offset;
    }
    const auto header = relay::encode_pdp_header(to, relay::wire_port(port, offset), static_cast<uint32_t>(len));
    std::vector<uint8_t> frame(header.begin(), header.end());
    if (len > 0) frame.insert(frame.end(), data, data + len);
    std::lock_guard<std::mutex> lock(relay_send_mu_);
    if (!relay::send_all(fd, frame.data(), frame.size(), kRelayTimeoutMs)) {
        std::fprintf(stderr, "[ADHOC] relay: PDP send failed\n");
    }
    return 0;  // like a lost datagram
}

int Sockets::send(int id, const pro::Mac& to, uint16_t port, const uint8_t* data, int len) {
    if (port == 0) return kErrInvalidPort;
    if (len < 0) return kErrInvalidDatalen;
    int fd;
    uint16_t offset;
    bool via_relay;
    std::function<std::vector<Peer>()> peers;
    {
        std::lock_guard<std::mutex> lock(mu_);
        fd = fd_of(id);
        offset = resolver_.port_offset;
        peers = resolver_.peers;
        via_relay = fd >= 0 && slots_[id - 1].relay;
    }
    if (fd < 0) return kErrInvalidSocketId;
    if (data == nullptr && len > 0) return kErrInvalidArg;
    static const pro::Mac kZero{};
    if (to == kZero) return kErrInvalidAddr;
    static const pro::Mac kBroadcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const bool broadcast = to == kBroadcast;
    if (via_relay) {
        if (len > relay::kPdpMax) return kErrInvalidDatalen;
        if (!broadcast) return send_relay(fd, to, port, data, len);
        if (peers) {
            for (const auto& p : peers()) send_relay(fd, p.mac, port, data, len);
        }
        return 0;
    }
    if (!peers) return 0;
    for (const auto& p : peers()) {
        if (!broadcast && p.mac != to) continue;
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = p.ip;
        target.sin_port = htons(static_cast<uint16_t>(port + offset));
        ::sendto(fd, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<sockaddr*>(&target),
                 sizeof(target));
        if (!broadcast) break;
    }
    return 0;  // also for an unknown MAC: PPSSPP fakes success
}

int Sockets::recv_relay(int id, pro::Mac* from, uint16_t* from_port, uint8_t* buf, int* len,
                        uint32_t timeout_us, bool nonblock) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        int fd;
        bool dead;
        {
            std::lock_guard<std::mutex> lock(mu_);
            fd = fd_of(id);
            if (fd < 0 || !slots_[id - 1].relay) return kErrInvalidSocketId;
            Slot& s = slots_[id - 1];
            if (!s.dead) {
                std::vector<uint8_t> in;
                if (!relay::read_available(fd, &in)) {
                    s.dead = true;
                    std::fprintf(stderr, "[ADHOC] relay closed the PDP session of port %u\n", s.port);
                }
                s.rx.feed(in.data(), static_cast<int>(in.size()));
            }
            dead = s.dead;
            const int n = s.rx.next_size();
            if (n >= 0) {
                if (n > *len) {
                    *len = n;
                    return kErrNotEnoughSpace;
                }
                uint16_t wire = 0;
                s.rx.pop(from, &wire, buf, len);
                *from_port = relay::psp_port(wire, resolver_.port_offset);
                return 0;
            }
        }
        if (nonblock) return kErrWouldBlock;
        int wait_ms = kSliceMs;
        if (timeout_us != 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return kErrTimeout;
            if (left < wait_ms) wait_ms = static_cast<int>(left);
        }
        if (dead) {
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        } else {
            relay::wait_readable(fd, wait_ms);
        }
    }
}

int Sockets::recv(int id, pro::Mac* from, uint16_t* from_port, uint8_t* buf, int* len,
                  uint32_t timeout_us, bool nonblock) {
    int fd;
    uint16_t offset;
    bool via_relay;
    {
        std::lock_guard<std::mutex> lock(mu_);
        fd = fd_of(id);
        offset = resolver_.port_offset;
        via_relay = fd >= 0 && slots_[id - 1].relay;
    }
    if (fd < 0) return kErrInvalidSocketId;
    if (from == nullptr || from_port == nullptr || buf == nullptr || len == nullptr) return kErrInvalidArg;
    if (via_relay) return recv_relay(id, from, from_port, buf, len, timeout_us, nonblock);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    uint8_t* peek = peek_buffer();
    for (;;) {
        sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        const int n = ::recvfrom(fd, reinterpret_cast<char*>(peek), 65536, MSG_PEEK,
                                 reinterpret_cast<sockaddr*>(&src), &src_len);
        if (n >= 0) {
            pro::Mac mac{};
            if (!peer_of_ip(src.sin_addr.s_addr, &mac)) {
                ::recvfrom(fd, reinterpret_cast<char*>(peek), 65536, 0, nullptr, nullptr);  // drop
                continue;
            }
            if (n > *len) {
                *len = n;
                return kErrNotEnoughSpace;
            }
            ::recvfrom(fd, reinterpret_cast<char*>(buf), *len, 0, nullptr, nullptr);
            *len = n;
            *from = mac;
            *from_port = static_cast<uint16_t>(ntohs(src.sin_port) - offset);
            return 0;
        }
        if (nonblock) return kErrWouldBlock;
        int wait_ms = kSliceMs;
        if (timeout_us != 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return kErrTimeout;
            if (left < wait_ms) wait_ms = static_cast<int>(left);
        }
        wait_readable(fd, wait_ms);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (fd_of(id) != fd) return kErrInvalidSocketId;  // deleted while waiting
        }
    }
}

Sockets& instance() {
    static Sockets s;
    return s;
}

}  // namespace pdp
