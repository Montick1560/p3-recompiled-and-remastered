// PSP ad hoc PTP over host TCP. States, return codes and the retry of
// refused connects follow PPSSPP Core/HLE/sceNetAdhoc.cpp
// (sceNetAdhocPtpOpen/Listen/Accept/Connect/Send/Recv/Flush/Close;
// GPL-2.0-or-later). Relay (aemu_postoffice) mode is not implemented.
// Difference: an incoming connection from a non-peer IP is dropped and the
// accept keeps waiting instead of failing.
#include "net/ptp.h"
#include "net/adhoc_ids.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "psp_socket_compat.h"
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#endif

namespace ptp {

namespace {

constexpr int kSliceMs = 50;

int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool would_block(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EWOULDBLOCK || e == EAGAIN;
#endif
}

bool in_progress(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return e == EINPROGRESS || e == EALREADY;
#endif
}

bool is_connected_error(int e) {
#ifdef _WIN32
    return e == WSAEISCONN;
#else
    return e == EISCONN;
#endif
}

void set_nonblocking(int fd) {
#ifdef _WIN32
    u_long on = 1;
    ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &on);
#else
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void tune(int fd, int bufsize) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof(one));
    if (bufsize > 0) {
        int snd = bufsize * 5, rcv = bufsize * 10;  // PPSSPP buffer sizing
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&snd), sizeof(snd));
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    }
}

// 1 readable/writable, 0 timeout, -1 error condition (Windows reports a
// failed nonblocking connect through the exception set).
int wait_fd(int fd, bool write, int ms) {
    fd_set rw, ex;
    FD_ZERO(&rw);
    FD_ZERO(&ex);
    FD_SET(static_cast<unsigned>(fd), &rw);
    FD_SET(static_cast<unsigned>(fd), &ex);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int r = ::select(fd + 1, write ? nullptr : &rw, write ? &rw : nullptr, &ex, &tv);
    if (r <= 0) return 0;
    if (FD_ISSET(static_cast<unsigned>(fd), &ex)) return -1;
    return 1;
}

int socket_error(int fd) {
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    return err;
}

}  // namespace

void Sockets::set_resolver(pdp::Resolver r) {
    std::lock_guard<std::mutex> lock(mu_);
    resolver_ = std::move(r);
}

bool Sockets::peer_ip(const pro::Mac& mac, uint32_t* ip) const {
    if (!resolver_.peers) return false;
    for (const auto& p : resolver_.peers()) {
        if (p.mac == mac) {
            *ip = p.ip;
            return true;
        }
    }
    return false;
}

bool Sockets::peer_mac(uint32_t ip, pro::Mac* mac) const {
    if (!resolver_.peers) return false;
    for (const auto& p : resolver_.peers()) {
        if (p.ip == ip) {
            *mac = p.mac;
            return true;
        }
    }
    return false;
}

int Sockets::new_socket(uint16_t sport, int bufsize, uint16_t* bound_port) const {
    psp_sock_startup();
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) return -1;
    const int one = 1;
    // Games listen and connect from the same PSP port (PPSSPP setSockReuseAddrPort).
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    tune(fd, bufsize);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!resolver_.bind_ip.empty()) ::inet_pton(AF_INET, resolver_.bind_ip.c_str(), &addr.sin_addr);
    addr.sin_port = htons(sport == 0 ? 0 : static_cast<uint16_t>(sport + resolver_.port_offset));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        psp_sock_close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    *bound_port = static_cast<uint16_t>(ntohs(addr.sin_port) - resolver_.port_offset);
    set_nonblocking(fd);
    return fd;
}

int Sockets::open(uint16_t sport, const pro::Mac& peer, uint16_t dport, int bufsize) {
    static const pro::Mac kZero{};
    static const pro::Mac kBroadcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (peer == kZero || peer == kBroadcast) return kErrInvalidAddr;
    if (bufsize <= 0) return kErrInvalidArg;
    std::lock_guard<std::mutex> lock(mu_);
    if (sport != 0) {
        for (const auto& s : slots_) {
            if (s.fd >= 0 && s.state != LISTEN && s.lport == sport && s.peer == peer && s.pport == dport) {
                return kErrPortInUse;
            }
        }
    }
    uint16_t bound = 0;
    const int fd = new_socket(sport, bufsize, &bound);
    if (fd < 0) return kErrPortNotAvail;
    const int id = adhoc_ids::shared().alloc();
    if (id < 0) {
        psp_sock_close(fd);
        return kErrSocketIdNotAvail;
    }
    slots_[id - 1] = Slot{fd, CLOSED, bound, peer, dport, bufsize};
    return id;
}

int Sockets::listen(uint16_t sport, int bufsize, int backlog) {
    if (bufsize <= 0) return kErrInvalidArg;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& s : slots_) {
        if (s.fd >= 0 && s.state == LISTEN && s.lport == sport && sport != 0) return kErrPortInUse;
    }
    uint16_t bound = 0;
    const int fd = new_socket(sport, bufsize, &bound);
    if (fd < 0) return kErrPortNotAvail;
    if (::listen(fd, backlog > 0 ? backlog : 1) != 0) {
        psp_sock_close(fd);
        return kErrPortNotAvail;
    }
    const int id = adhoc_ids::shared().alloc();
    if (id < 0) {
        psp_sock_close(fd);
        return kErrSocketIdNotAvail;
    }
    slots_[id - 1] = Slot{fd, LISTEN, bound, pro::Mac{}, 0, bufsize};
    return id;
}

int Sockets::connect(int id, uint32_t timeout_us, bool nonblock) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        int fd, state;
        sockaddr_in to{};
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
            Slot& s = slots_[id - 1];
            if (s.state == ESTABLISHED) return 0;
            if (s.state != CLOSED && s.state != SYN_SENT) return kErrNotOpened;
            uint32_t ip = 0;
            if (!peer_ip(s.peer, &ip)) return kErrInvalidAddr;
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = ip;
            to.sin_port = htons(static_cast<uint16_t>(s.pport + resolver_.port_offset));
            if (s.state == CLOSED) {
                const int r = ::connect(s.fd, reinterpret_cast<sockaddr*>(&to), sizeof(to));
                const int e = r == 0 ? 0 : last_error();
                if (r == 0 || is_connected_error(e)) {
                    s.state = ESTABLISHED;
                    return 0;
                }
                if (in_progress(e)) s.state = SYN_SENT;
            }
            fd = s.fd;
            state = s.state;
        }
        int done = 0;
        if (state == SYN_SENT) {
            const int w = wait_fd(fd, true, nonblock ? 0 : kSliceMs);
            if (w != 0) done = (w > 0 && socket_error(fd) == 0) ? 1 : -1;
        } else {
            done = -1;  // refused at once
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            Slot& s = slots_[id - 1];
            if (s.fd != fd) return kErrInvalidSocketId;  // closed meanwhile
            if (done > 0) {
                s.state = ESTABLISHED;
                return 0;
            }
            if (done < 0) {
                // A refused attempt leaves the socket unusable: make a new one
                // on the same port and try again (PPSSPP RecreatePtpSocket).
                uint16_t bound = 0;
                psp_sock_close(s.fd);
                s.fd = new_socket(s.lport, s.bufsize, &bound);
                s.state = CLOSED;
                if (s.fd < 0) {
                    s.fd = -1;
                    adhoc_ids::shared().release(id);
                    s = Slot{};
                    return kErrDisconnected;
                }
            }
        }
        if (nonblock) return kErrWouldBlock;
        if (timeout_us != 0 && std::chrono::steady_clock::now() >= deadline) return kErrTimeout;
        if (done < 0) std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
    }
}

int Sockets::accept(int id, pro::Mac* peer, uint16_t* peer_port, uint32_t timeout_us, bool nonblock) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        int fd, bufsize;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
            const Slot& s = slots_[id - 1];
            if (s.state != LISTEN) return kErrNotListened;
            fd = s.fd;
            bufsize = s.bufsize;
        }
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const int nfd = static_cast<int>(::accept(fd, reinterpret_cast<sockaddr*>(&from), &from_len));
        if (nfd >= 0) {
            std::lock_guard<std::mutex> lock(mu_);
            pro::Mac mac{};
            if (!peer_mac(from.sin_addr.s_addr, &mac)) {
                psp_sock_close(nfd);  // not a group member
                continue;
            }
            const int nid = adhoc_ids::shared().alloc();
            if (nid < 0) {
                psp_sock_close(nfd);
                return kErrSocketIdNotAvail;
            }
            set_nonblocking(nfd);
            tune(nfd, bufsize);
            sockaddr_in local{};
            socklen_t local_len = sizeof(local);
            ::getsockname(nfd, reinterpret_cast<sockaddr*>(&local), &local_len);
            Slot ns;
            ns.fd = nfd;
            ns.state = ESTABLISHED;
            ns.lport = static_cast<uint16_t>(ntohs(local.sin_port) - resolver_.port_offset);
            ns.peer = mac;
            ns.pport = static_cast<uint16_t>(ntohs(from.sin_port) - resolver_.port_offset);
            ns.bufsize = bufsize;
            slots_[nid - 1] = ns;
            if (peer) *peer = mac;
            if (peer_port) *peer_port = ns.pport;
            return nid;
        }
        if (nonblock) return kErrWouldBlock;
        int wait_ms = kSliceMs;
        if (timeout_us != 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return kErrTimeout;
            if (left < wait_ms) wait_ms = static_cast<int>(left);
        }
        wait_fd(fd, false, wait_ms);
    }
}

int Sockets::send(int id, const uint8_t* data, int* len, uint32_t timeout_us, bool nonblock) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        int fd;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
            Slot& s = slots_[id - 1];
            if (s.state != ESTABLISHED && s.state != SYN_SENT) return kErrNotConnected;
            if (data == nullptr || len == nullptr || *len <= 0) return kErrInvalidArg;
            fd = s.fd;
        }
        const int r = ::send(fd, reinterpret_cast<const char*>(data), *len, 0);
        const int e = r < 0 ? last_error() : 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            Slot& s = slots_[id - 1];
            if (s.fd != fd) return kErrInvalidSocketId;
            if (r > 0) {
                *len = r;
                s.state = ESTABLISHED;
                return 0;
            }
            if (!(r < 0 && (would_block(e) || (s.state == SYN_SENT && in_progress(e))))) {
                s.state = CLOSED;
                return kErrDisconnected;
            }
        }
        if (nonblock) return kErrWouldBlock;
        if (timeout_us != 0 && std::chrono::steady_clock::now() >= deadline) return kErrTimeout;
        wait_fd(fd, true, kSliceMs);
    }
}

int Sockets::recv(int id, uint8_t* buf, int* len, uint32_t timeout_us, bool nonblock) {
    if (buf == nullptr || len == nullptr || *len <= 0) return kErrInvalidArg;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        int fd;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
            const Slot& s = slots_[id - 1];
            if (s.state != ESTABLISHED && s.state != SYN_SENT) return kErrNotConnected;
            fd = s.fd;
        }
        const int r = ::recv(fd, reinterpret_cast<char*>(buf), *len, 0);
        const int e = r < 0 ? last_error() : 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            Slot& s = slots_[id - 1];
            if (s.fd != fd) return kErrInvalidSocketId;
            if (r > 0) {
                *len = r;
                s.state = ESTABLISHED;
                return 0;
            }
            if (r == 0 || !(would_block(e) || (s.state == SYN_SENT && in_progress(e)))) {
                s.state = CLOSED;  // peer closed or connection error
                return kErrDisconnected;
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
        wait_fd(fd, false, wait_ms);
    }
}

int Sockets::flush(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
    const Slot& s = slots_[id - 1];
    if (s.state != ESTABLISHED && s.state != SYN_SENT) return kErrNotConnected;
    return 0;  // TCP_NODELAY is always on
}

int Sockets::close(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
    psp_sock_close(slots_[id - 1].fd);
    slots_[id - 1] = Slot{};
    adhoc_ids::shared().release(id);
    return 0;
}

void Sockets::close_all() {
    for (int id = 1; id <= pdp::kMaxSockets; id++) {
        bool open;
        {
            std::lock_guard<std::mutex> lock(mu_);
            open = slots_[id - 1].fd >= 0;
        }
        if (open) close(id);
    }
}

int Sockets::state(int id) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return -1;
    return slots_[id - 1].state;
}

uint16_t Sockets::local_port(int id) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return 0;
    return slots_[id - 1].lport;
}

Sockets& instance() {
    static Sockets s;
    return s;
}

}  // namespace ptp
