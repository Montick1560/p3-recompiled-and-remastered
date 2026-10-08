#pragma once
// Minimal BSD-socket portability layer for the debug socket and its test.
// POSIX: plain BSD sockets. Windows: Winsock2 (link ws2_32).
// Socket handles are kept in `int` like the POSIX code; Winsock SOCKET values
// fit in practice for the handful of sockets the debug server opens.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

inline int psp_sock_close(int fd) {
    return ::closesocket(static_cast<SOCKET>(fd));
}

/// Initialise Winsock once per process. Safe to call repeatedly.
inline bool psp_sock_startup() {
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

inline int psp_sock_close(int fd) { return ::close(fd); }
inline bool psp_sock_startup() { return true; }
#endif
