#pragma once
// In-process fake of tools/infra_server.py for the infra_client tests: one
// connection, net/infra_protocol frames over TCP on a free loopback port.
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "net/infra_client.h"
#include "net/infra_protocol.h"
#include "psp_socket_compat.h"

inline void fake_sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// One-connection fake server. Answers login, ping and link_stats by itself; every other
// frame goes to on_frame (on the server thread).
struct FakeInfraServer {
    int listen_fd = -1, client = -1;
    uint16_t port = 0;
    std::thread th;
    std::atomic<bool> stop{false};
    std::function<void(FakeInfraServer&, const infra::Frame&)> on_frame;
    int ping_delay_ms = 0;
    bool answer_pings = true;
    bool answer_link_stats = true;  // false: link_stats goes to on_frame like any other request
    uint32_t vip = 0x0A500003;
    nlohmann::json login_extra = nlohmann::json::object();  // merged into the login answer (e.g. "direct")
    std::mutex mu;
    std::vector<infra::Frame> got;

    FakeInfraServer() {
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
    ~FakeInfraServer() {
        stop = true;
        close_client();
        if (listen_fd >= 0) psp_sock_close(listen_fd);
        if (th.joinable()) th.join();
    }
    infra::Address address() const {
        infra::Address a;
        a.kind = infra::Address::Tcp;
        a.host = "127.0.0.1";
        a.port = port;
        return a;
    }
    void start() {
        th = std::thread([this] {
            do {
                serve(static_cast<int>(::accept(listen_fd, nullptr, nullptr)));
            } while (reaccept && !stop);
        });
    }
    // reaccept: after a connection ends, accept the next one (a client that reconnects).
    bool reaccept = false;
    void serve(int fd) {
        client = fd;
        if (client < 0) return;
        {
            infra::FrameReader r;
            uint8_t buf[8192];
            while (!stop && client >= 0) {
                const int n = ::recv(client, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n <= 0) break;
                r.feed(buf, static_cast<size_t>(n));
                infra::Frame f;
                while (r.next(&f)) {
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        got.push_back(f);
                    }
                    const std::string op = f.kind == infra::kKindJson ? f.json.value("op", "") : "";
                    if (op == "login") {
                        nlohmann::json answer = {{"id", f.json["id"]}, {"ok", true}, {"session", "s1"}, {"vip", vip}};
                        answer.update(login_extra);
                        send_json(answer);
                    } else if (op == "ping") {
                        if (!answer_pings) continue;
                        if (ping_delay_ms) fake_sleep_ms(ping_delay_ms);
                        send_json({{"id", f.json["id"]}, {"ok", true}, {"t", f.json["t"]}});
                    } else if (op == "link_stats" && answer_link_stats) {
                        send_json({{"id", f.json["id"]}, {"ok", true}});
                    } else if (on_frame) {
                        on_frame(*this, f);
                    }
                }
            }
        }
        if (reaccept) close_client();
    }
    void send_raw(const std::vector<uint8_t>& v) {
        if (client >= 0) ::send(client, reinterpret_cast<const char*>(v.data()), static_cast<int>(v.size()), 0);
    }
    void send_json(const nlohmann::json& j) { send_raw(infra::encode_json(j)); }
    void close_client() {
        if (client >= 0) {
            ::shutdown(client, SHUT_RDWR);
            psp_sock_close(client);
            client = -1;
        }
    }
};
