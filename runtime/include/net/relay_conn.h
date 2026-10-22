#pragma once
// TCP sessions to an aemu_postoffice relay (see net/relay_protocol.h): open a
// session with its init packet, send whole frames, read what has arrived.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "net/relay_protocol.h"

namespace relay {

// Connects to host:port (up to timeout_ms): a nonblocking TCP_NODELAY socket,
// or -1. log_tag prefixes the failure lines ("[ADHOC] relay", "[INFRA]").
int connect_tcp(const std::string& host, uint16_t port, int timeout_ms, const char* log_tag);
// Connects to host:port (up to timeout_ms) and sends the init packet. A
// nonblocking socket, or -1.
int open_session(const std::string& host, uint16_t port, const std::array<uint8_t, kInitSize>& init,
                 int timeout_ms);
// Sends every byte, waiting for the socket to drain up to timeout_ms in total.
bool send_all(int fd, const uint8_t* data, size_t len, int timeout_ms);
// Appends the bytes that can be read without blocking. false once the relay
// closed the session or it failed.
bool read_available(int fd, std::vector<uint8_t>* out);
bool wait_readable(int fd, int ms);

}  // namespace relay
