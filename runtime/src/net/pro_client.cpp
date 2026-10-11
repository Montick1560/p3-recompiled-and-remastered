// PRO ad hoc server client. Connection and ping behaviour follow PPSSPP
// Core/HLE/proAdhoc.cpp (initNetwork, friendFinder; GPL-2.0-or-later).
#include "net/pro_client.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "psp_socket_compat.h"
#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#endif

namespace {

constexpr int kPingIntervalMs = 2000;  // PSP_ADHOCCTL_PING_TIMEOUT
constexpr int kPollMs = 100;

void set_nonblocking(int fd, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode);
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

bool resolve_ipv4(const std::string& host, uint16_t port, sockaddr_in* out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return false;
    *out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    out->sin_port = htons(port);
    ::freeaddrinfo(res);
    return true;
}

// Waits until fd is readable (want_write false) or writable; false on timeout.
bool wait_socket(int fd, bool want_write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<unsigned>(fd), &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int r = ::select(fd + 1, want_write ? nullptr : &set, want_write ? &set : nullptr,
                           nullptr, &tv);
    return r > 0;
}

}  // namespace

bool ProClient::start(const std::string& host, uint16_t port, const pro::Mac& mac,
                      const std::string& nick, const std::string& product,
                      const std::string& bind_ip, int connect_timeout_ms) {
    stop();
    psp_sock_startup();
    sockaddr_in server{};
    if (!resolve_ipv4(host, port, &server)) {
        std::fprintf(stderr, "[ADHOC] cannot resolve server %s\n", host.c_str());
        return false;
    }
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) return false;
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof(one));
    if (!bind_ip.empty()) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        if (::inet_pton(AF_INET, bind_ip.c_str(), &local.sin_addr) == 1 &&
            ::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            std::fprintf(stderr, "[ADHOC] bind to %s failed\n", bind_ip.c_str());
        }
    }
    set_nonblocking(fd, true);
    bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == 0;
    if (!ok && wait_socket(fd, true, connect_timeout_ms)) {
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        ok = err == 0;
    }
    if (!ok) {
        std::fprintf(stderr, "[ADHOC] cannot connect to server %s:%u\n", host.c_str(), port);
        psp_sock_close(fd);
        return false;
    }
    set_nonblocking(fd, false);
    fd_ = fd;
    {
        std::lock_guard<std::mutex> lock(mu_);
        events_.clear();
        peers_.clear();
    }
    connected_ = true;
    if (!send_bytes(pro::encode_login(mac, nick, product))) {
        stop();
        return false;
    }
    running_ = true;
    thread_ = std::thread(&ProClient::run, this);
    std::fprintf(stderr, "[ADHOC] logged in to %s:%u as \"%s\" (%s)\n", host.c_str(), port,
                 nick.c_str(), product.c_str());
    return true;
}

void ProClient::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(send_mu_);
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        psp_sock_close(fd_);
        fd_ = -1;
    }
    connected_ = false;
}

bool ProClient::send_bytes(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(send_mu_);
    if (!connected_ || fd_ < 0) return false;
    size_t sent = 0;
    while (sent < data.size()) {
        const int r = ::send(fd_, reinterpret_cast<const char*>(data.data() + sent),
                             static_cast<int>(data.size() - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

bool ProClient::send_connect(const std::string& group) { return send_bytes(pro::encode_connect(group)); }
bool ProClient::send_disconnect() { return send_bytes(pro::encode_simple(pro::DISCONNECT)); }
bool ProClient::send_scan() { return send_bytes(pro::encode_simple(pro::SCAN)); }
bool ProClient::send_chat(const std::string& message) { return send_bytes(pro::encode_chat(message)); }

bool ProClient::wait_event(pro::Event* out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !events_.empty(); })) {
        return false;
    }
    *out = std::move(events_.front());
    events_.pop_front();
    return true;
}

std::vector<ProClient::Peer> ProClient::peers() const {
    std::lock_guard<std::mutex> lock(mu_);
    return peers_;
}

void ProClient::clear_peers() {
    std::lock_guard<std::mutex> lock(mu_);
    peers_.clear();
}

void ProClient::on_event(const pro::Event& e) {
    std::lock_guard<std::mutex> lock(mu_);
    if (e.op == pro::CONNECT) {
        Peer p{e.mac, e.nick, e.ip};
        bool replaced = false;
        for (auto& q : peers_) {
            if (q.mac == e.mac) {
                q = p;
                replaced = true;
            }
        }
        if (!replaced) peers_.push_back(p);
    } else if (e.op == pro::DISCONNECT) {
        // Like PPSSPP deleteFriendByIP: the server identifies leavers by IP.
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            if (it->ip == e.ip) {
                peers_.erase(it);
                break;
            }
        }
    }
    events_.push_back(e);
    cv_.notify_all();
}

void ProClient::lost(const char* why) {
    if (!connected_.exchange(false)) return;
    std::fprintf(stderr, "[ADHOC] server connection lost (%s)\n", why);
    pro::Event e;
    e.op = pro::DISCONNECT;
    e.ip = 0;
    std::lock_guard<std::mutex> lock(mu_);
    peers_.clear();
    events_.push_back(e);
    cv_.notify_all();
}

void ProClient::run() {
    pro::Decoder decoder;
    auto last_ping = std::chrono::steady_clock::now();
    uint8_t buf[2048];
    while (running_ && connected_) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ping >= std::chrono::milliseconds(kPingIntervalMs)) {
            if (!send_bytes(pro::encode_simple(pro::PING))) {
                lost("ping failed");
                break;
            }
            last_ping = now;
        }
        if (!wait_socket(fd_, false, kPollMs)) continue;
        const int r = ::recv(fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (r <= 0) {
            lost(r == 0 ? "closed by server" : "receive error");
            break;
        }
        decoder.feed(buf, static_cast<size_t>(r));
        pro::Event e;
        while (decoder.next(&e)) on_event(e);
        if (decoder.error()) {
            lost("unknown packet");
            break;
        }
    }
}
