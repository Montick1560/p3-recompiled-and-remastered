// PSP ad hoc PTP over host TCP. States, return codes and the retry of
// refused connects follow PPSSPP Core/HLE/sceNetAdhoc.cpp
// (sceNetAdhocPtpOpen/Listen/Accept/Connect/Send/Recv/Flush/Close;
// GPL-2.0-or-later). In relay (aemu_postoffice) mode the stream goes through
// the relay instead (net/relay_protocol.h).
// Difference: an incoming connection from a non-peer IP is dropped and the
// accept keeps waiting instead of failing.
#include "net/ptp.h"
#include "net/adhoc_ids.h"
#include "net/relay_conn.h"

#include <algorithm>
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
constexpr int kRelayTimeoutMs = 3000;

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
    uint16_t bound = sport;
    const int fd = resolver_.relay ? static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
                                   : new_socket(sport, bufsize, &bound);
    if (fd < 0) return kErrPortNotAvail;
    const int id = adhoc_ids::shared().alloc();
    if (id < 0) {
        psp_sock_close(fd);
        return kErrSocketIdNotAvail;
    }
    Slot& slot = slots_[id - 1];
    slot = Slot{};
    slot.fd = fd;
    slot.state = CLOSED;
    slot.lport = bound;
    slot.peer = peer;
    slot.pport = dport;
    slot.bufsize = bufsize;
    slot.relay = resolver_.relay;
    return id;
}

bool Sockets::is_relay(int id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return id >= 1 && id <= pdp::kMaxSockets && slots_[id - 1].fd >= 0 && slots_[id - 1].relay;
}

int Sockets::listen_relay(uint16_t sport, int bufsize) {
    pdp::Resolver r;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& s : slots_) {
            if (s.fd >= 0 && s.state == LISTEN && s.lport == sport && sport != 0) return kErrPortInUse;
        }
        r = resolver_;
    }
    const auto init = relay::encode_init(relay::INIT_PTP_LISTEN, r.self_mac, relay::wire_port(sport, r.port_offset),
                                         pro::Mac{}, 0);
    const int fd = relay::open_session(r.relay_host, r.relay_port, init, kRelayTimeoutMs);
    if (fd < 0) return kErrPortNotAvail;
    std::lock_guard<std::mutex> lock(mu_);
    const int id = adhoc_ids::shared().alloc();
    if (id < 0) {
        psp_sock_close(fd);
        return kErrSocketIdNotAvail;
    }
    Slot& slot = slots_[id - 1];
    slot = Slot{};
    slot.fd = fd;
    slot.state = LISTEN;
    slot.lport = sport;
    slot.bufsize = bufsize;
    slot.relay = true;
    return id;
}

int Sockets::connect_relay(int id, uint32_t timeout_us, bool nonblock) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        int fd;
        bool start = false, retry = false;
        pdp::Resolver r;
        uint16_t lport = 0, pport = 0;
        pro::Mac peer{};
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
            Slot& s = slots_[id - 1];
            if (s.state == ESTABLISHED) return 0;
            if (s.state != CLOSED && s.state != SYN_SENT) return kErrNotOpened;
            fd = s.fd;
            if (!s.relay_waiting) {
                start = true;
                r = resolver_;
                lport = s.lport;
                pport = s.pport;
                peer = s.peer;
            } else {
                std::vector<uint8_t> in;
                const bool open = relay::read_available(fd, &in);
                s.ctl.insert(s.ctl.end(), in.begin(), in.end());
                if (s.ctl.size() >= static_cast<size_t>(relay::kConnectSize)) {
                    // The ack: the listener accepted. Anything after it is data.
                    s.rx.feed(s.ctl.data() + relay::kConnectSize,
                              static_cast<int>(s.ctl.size()) - relay::kConnectSize);
                    s.ctl.clear();
                    s.relay_waiting = false;
                    s.state = ESTABLISHED;
                    return 0;
                }
                if (!open) {  // no listener (yet): ask again, like a refused connect
                    s.relay_waiting = false;
                    s.ctl.clear();
                    retry = true;
                }
            }
        }
        if (start) {
            const auto init = relay::encode_init(relay::INIT_PTP_CONNECT, r.self_mac,
                                                 relay::wire_port(lport, r.port_offset), peer,
                                                 relay::wire_port(pport, r.port_offset));
            const int nfd = relay::open_session(r.relay_host, r.relay_port, init, kRelayTimeoutMs);
            std::lock_guard<std::mutex> lock(mu_);
            Slot& s = slots_[id - 1];
            if (s.fd != fd) {  // closed meanwhile
                if (nfd >= 0) psp_sock_close(nfd);
                return kErrInvalidSocketId;
            }
            if (nfd >= 0) {
                psp_sock_close(s.fd);
                s.fd = fd = nfd;
                s.relay_waiting = true;
                s.state = SYN_SENT;
                continue;  // the ack may already be there
            }
            retry = true;
        }
        if (nonblock) return kErrWouldBlock;
        if (timeout_us != 0 && std::chrono::steady_clock::now() >= deadline) return kErrTimeout;
        if (retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
        } else {
            relay::wait_readable(fd, kSliceMs);
        }
    }
}

int Sockets::accept_relay(int id, pro::Mac* peer, uint16_t* peer_port, uint32_t timeout_us, bool nonblock) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        int fd;
        bool dead, have_request = false;
        pro::Mac req_mac{};
        uint16_t req_port = 0, lport = 0;
        int bufsize = 0;
        pdp::Resolver r;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
            Slot& s = slots_[id - 1];
            if (s.state != LISTEN) return kErrNotListened;
            fd = s.fd;
            if (!s.dead) {
                std::vector<uint8_t> in;
                if (!relay::read_available(fd, &in)) {
                    s.dead = true;
                    std::fprintf(stderr, "[ADHOC] relay closed the PTP listen session of port %u\n", s.lport);
                }
                s.ctl.insert(s.ctl.end(), in.begin(), in.end());
            }
            dead = s.dead;
            if (s.ctl.size() >= static_cast<size_t>(relay::kConnectSize)) {
                relay::parse_connect(s.ctl.data(), &req_mac, &req_port);
                s.ctl.erase(s.ctl.begin(), s.ctl.begin() + relay::kConnectSize);
                have_request = true;
                lport = s.lport;
                bufsize = s.bufsize;
                r = resolver_;
            }
        }
        if (have_request) {
            const auto init = relay::encode_init(relay::INIT_PTP_ACCEPT, r.self_mac,
                                                 relay::wire_port(lport, r.port_offset), req_mac, req_port);
            const int nfd = relay::open_session(r.relay_host, r.relay_port, init, kRelayTimeoutMs);
            std::vector<uint8_t> ack;
            const auto ack_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRelayTimeoutMs);
            bool open = nfd >= 0;
            while (open && ack.size() < static_cast<size_t>(relay::kConnectSize) &&
                   std::chrono::steady_clock::now() < ack_deadline) {
                relay::wait_readable(nfd, kSliceMs);
                open = relay::read_available(nfd, &ack);
            }
            if (ack.size() >= static_cast<size_t>(relay::kConnectSize)) {
                std::lock_guard<std::mutex> lock(mu_);
                const int nid = adhoc_ids::shared().alloc();
                if (nid < 0) {
                    psp_sock_close(nfd);
                    return kErrSocketIdNotAvail;
                }
                Slot& ns = slots_[nid - 1];
                ns = Slot{};
                ns.fd = nfd;
                ns.state = ESTABLISHED;
                ns.lport = lport;
                ns.peer = req_mac;
                ns.pport = relay::psp_port(req_port, r.port_offset);
                ns.bufsize = bufsize;
                ns.relay = true;
                ns.rx.feed(ack.data() + relay::kConnectSize, static_cast<int>(ack.size()) - relay::kConnectSize);
                if (peer) *peer = req_mac;
                if (peer_port) *peer_port = ns.pport;
                return nid;
            }
            if (nfd >= 0) psp_sock_close(nfd);  // the connecting side gave up
            continue;
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

int Sockets::send_relay(int id, const uint8_t* data, int* len, uint32_t timeout_us, bool nonblock) {
    int fd;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
        const Slot& s = slots_[id - 1];
        if (s.state != ESTABLISHED && s.state != SYN_SENT) return kErrNotConnected;
        if (data == nullptr || len == nullptr || *len <= 0) return kErrInvalidArg;
        fd = s.fd;
        if (s.state == SYN_SENT) fd = -1;
    }
    if (fd < 0) {  // still connecting: finish that first
        const int r = connect_relay(id, timeout_us, nonblock);
        if (r != 0) return r;
        std::lock_guard<std::mutex> lock(mu_);
        fd = slots_[id - 1].fd;
    }
    std::vector<uint8_t> frames;
    for (int off = 0; off < *len; off += relay::kPtpMax) {
        const int n = std::min(relay::kPtpMax, *len - off);
        const auto h = relay::encode_ptp_header(static_cast<uint32_t>(n));
        frames.insert(frames.end(), h.begin(), h.end());
        frames.insert(frames.end(), data + off, data + off + n);
    }
    bool ok;
    {
        std::lock_guard<std::mutex> lock(relay_send_mu_);
        ok = relay::send_all(fd, frames.data(), frames.size(), kRelayTimeoutMs);
    }
    if (!ok) {
        std::lock_guard<std::mutex> lock(mu_);
        if (slots_[id - 1].fd == fd) slots_[id - 1].state = CLOSED;
        return kErrDisconnected;
    }
    return 0;
}

int Sockets::recv_relay(int id, uint8_t* buf, int* len, uint32_t timeout_us, bool nonblock) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        int fd;
        bool dead;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (id < 1 || id > pdp::kMaxSockets || slots_[id - 1].fd < 0) return kErrInvalidSocketId;
            Slot& s = slots_[id - 1];
            if (s.state != ESTABLISHED && s.state != SYN_SENT) return kErrNotConnected;
            fd = s.fd;
            if (s.state == ESTABLISHED && !s.dead) {
                std::vector<uint8_t> in;
                if (!relay::read_available(fd, &in)) s.dead = true;
                s.rx.feed(in.data(), static_cast<int>(in.size()));
            }
            const int k = s.rx.read(buf, *len);
            if (k > 0) {
                *len = k;
                return 0;
            }
            if (s.dead) {
                s.state = CLOSED;  // the peer (or the relay) closed the stream
                return kErrDisconnected;
            }
            dead = s.dead;
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

int Sockets::listen(uint16_t sport, int bufsize, int backlog) {
    if (bufsize <= 0) return kErrInvalidArg;
    bool relay_mode;
    {
        std::lock_guard<std::mutex> lock(mu_);
        relay_mode = resolver_.relay;
    }
    if (relay_mode) return listen_relay(sport, bufsize);
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
    Slot& slot = slots_[id - 1];
    slot = Slot{};
    slot.fd = fd;
    slot.state = LISTEN;
    slot.lport = bound;
    slot.bufsize = bufsize;
    return id;
}

int Sockets::connect(int id, uint32_t timeout_us, bool nonblock) {
    if (is_relay(id)) return connect_relay(id, timeout_us, nonblock);
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
    if (is_relay(id)) return accept_relay(id, peer, peer_port, timeout_us, nonblock);
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
            Slot ns{};
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
    if (is_relay(id)) return send_relay(id, data, len, timeout_us, nonblock);
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
    if (is_relay(id)) return recv_relay(id, buf, len, timeout_us, nonblock);
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
