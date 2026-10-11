#include "net/relay_conn.h"

#include <chrono>
#include <cstdio>

#include "psp_socket_compat.h"
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#endif

namespace relay {

namespace {

bool would_block() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
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

// 1 ready, 0 timeout, -1 error (Windows reports a failed connect as an exception).
int wait_fd(int fd, bool write, int ms) {
    fd_set rw, ex;
    FD_ZERO(&rw);
    FD_ZERO(&ex);
    FD_SET(static_cast<unsigned>(fd), &rw);
    FD_SET(static_cast<unsigned>(fd), &ex);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int r = ::select(fd + 1, write ? nullptr : &rw, write ? &rw : nullptr, &ex, &tv);
    if (r <= 0) return 0;
    return FD_ISSET(static_cast<unsigned>(fd), &ex) ? -1 : 1;
}

}  // namespace

int open_session(const std::string& host, uint16_t port, const std::array<uint8_t, kInitSize>& init,
                 int timeout_ms) {
    psp_sock_startup();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || res == nullptr) {
        std::fprintf(stderr, "[ADHOC] relay: cannot resolve %s\n", host.c_str());
        return -1;
    }
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) {
        ::freeaddrinfo(res);
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof(one));
    set_nonblocking(fd);
    const int r = ::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen));
    ::freeaddrinfo(res);
    if (r != 0 && !(would_block() && wait_fd(fd, true, timeout_ms) > 0)) {
        std::fprintf(stderr, "[ADHOC] relay: cannot connect to %s:%u\n", host.c_str(), port);
        psp_sock_close(fd);
        return -1;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    if (err != 0 || !send_all(fd, init.data(), init.size(), timeout_ms)) {
        std::fprintf(stderr, "[ADHOC] relay: connection to %s:%u failed\n", host.c_str(), port);
        psp_sock_close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const uint8_t* data, size_t len, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    size_t done = 0;
    while (done < len) {
        const int n = ::send(fd, reinterpret_cast<const char*>(data + done), static_cast<int>(len - done), 0);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && !would_block()) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        if (wait_fd(fd, true, static_cast<int>(left)) < 0) return false;
    }
    return true;
}

bool read_available(int fd, std::vector<uint8_t>* out) {
    uint8_t buf[16384];
    for (;;) {
        const int n = ::recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n > 0) {
            out->insert(out->end(), buf, buf + n);
            continue;
        }
        if (n == 0) return false;  // closed by the relay
        return would_block();
    }
}

bool wait_readable(int fd, int ms) { return wait_fd(fd, false, ms) != 0; }

}  // namespace relay
