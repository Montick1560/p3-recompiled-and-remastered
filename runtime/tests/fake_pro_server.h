#pragma once
// One-connection fake PRO ad hoc server on 127.0.0.1 for net unit tests.
#include <cstdint>
#include <cstring>
#include <vector>

#include "net/pro_protocol.h"
#include "psp_socket_compat.h"

struct FakeProServer {
    int listen_fd = -1;
    int client = -1;
    uint16_t port = 0;

    FakeProServer() {
        psp_sock_startup();
        listen_fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(listen_fd, 1);
        socklen_t len = sizeof(a);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
    }
    ~FakeProServer() {
        if (client >= 0) psp_sock_close(client);
        if (listen_fd >= 0) psp_sock_close(listen_fd);
    }
    void accept_one() { client = static_cast<int>(::accept(listen_fd, nullptr, nullptr)); }
    bool read_exact(uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            const int r = ::recv(client, reinterpret_cast<char*>(buf + got), static_cast<int>(n - got), 0);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }
    // Reads client packets until one with this opcode (skipping pings).
    bool expect(uint8_t opcode, std::vector<uint8_t>* body = nullptr) {
        for (;;) {
            uint8_t op = 0;
            if (!read_exact(&op, 1)) return false;
            size_t size = 0;
            switch (op) {
            case pro::PING: case pro::DISCONNECT: case pro::SCAN: size = 0; break;
            case pro::LOGIN: size = 6 + 128 + 9; break;
            case pro::CONNECT: size = 8; break;
            case pro::CHAT: size = 64; break;
            default: return false;
            }
            std::vector<uint8_t> b(size);
            if (size && !read_exact(b.data(), size)) return false;
            if (op == opcode) {
                if (body) *body = b;
                return true;
            }
        }
    }
    void write(const std::vector<uint8_t>& p) {
        ::send(client, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0);
    }
    void close_client() {
        psp_sock_close(client);
        client = -1;
    }

    static std::vector<uint8_t> s2c_connect(const pro::Mac& mac, const char* nick, uint8_t last_ip) {
        std::vector<uint8_t> p(139, 0);
        p[0] = pro::CONNECT;
        std::memcpy(&p[1], nick, std::strlen(nick));
        std::memcpy(&p[129], mac.data(), 6);
        p[135] = 10;
        p[138] = last_ip;
        return p;
    }
    static std::vector<uint8_t> s2c_bssid(const pro::Mac& mac) {
        std::vector<uint8_t> p{pro::CONNECT_BSSID};
        p.insert(p.end(), mac.begin(), mac.end());
        return p;
    }
};
