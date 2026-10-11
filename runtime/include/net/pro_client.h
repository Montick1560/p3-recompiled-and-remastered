#pragma once
// Client side of the PRO ad hoc server connection: one TCP socket, a
// receive thread that decodes server packets into events and keeps the
// table of peers in the current group, and a ping every 2 s (the server
// drops clients silent for 15 s). Thread-safe.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/pro_protocol.h"

class ProClient {
public:
    struct Peer {
        pro::Mac mac{};
        std::string nick;
        uint32_t ip = 0;  // wire order, see pro::Event::ip
    };

    ~ProClient() { stop(); }

    // Connects (bound to bind_ip when not empty, so several instances on one
    // PC show up as distinct loopback addresses), sends LOGIN and starts the
    // receive thread. False if the server cannot be reached in time.
    bool start(const std::string& host, uint16_t port, const pro::Mac& mac,
               const std::string& nick, const std::string& product,
               const std::string& bind_ip = "", int connect_timeout_ms = 3000);
    void stop();  // idempotent; joins the receive thread

    bool send_connect(const std::string& group);
    bool send_disconnect();
    bool send_scan();
    bool send_chat(const std::string& message);

    bool connected() const { return connected_; }
    // Next server event. When the server connection is lost an event with
    // op DISCONNECT and ip 0 is queued once.
    bool wait_event(pro::Event* out, int timeout_ms);
    std::vector<Peer> peers() const;
    void clear_peers();

private:
    void run();
    bool send_bytes(const std::vector<uint8_t>& data);
    void on_event(const pro::Event& e);
    void lost(const char* why);

    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread thread_;
    std::mutex send_mu_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<pro::Event> events_;
    std::vector<Peer> peers_;
};
