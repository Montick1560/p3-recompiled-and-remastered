#pragma once
// PRO ad hoc server protocol (PPSSPP Core/HLE/proAdhoc.h, used by PPSSPP's
// built-in server and community servers): packed little-endian packets over
// one TCP connection. Client->server packets are built by the encode_*
// functions; the server->client stream is split into events by Decoder.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pro {

enum Opcode : uint8_t {
    PING = 0,
    LOGIN = 1,
    CONNECT = 2,
    DISCONNECT = 3,
    SCAN = 4,
    SCAN_COMPLETE = 5,
    CONNECT_BSSID = 6,
    CHAT = 7,
};

constexpr size_t kNickLen = 128;
constexpr size_t kGroupLen = 8;
constexpr size_t kProductLen = 9;
constexpr size_t kChatLen = 64;

using Mac = std::array<uint8_t, 6>;

std::vector<uint8_t> encode_login(const Mac& mac, const std::string& nick,
                                  const std::string& product);  // 144 bytes
std::vector<uint8_t> encode_connect(const std::string& group);  // 9 bytes
std::vector<uint8_t> encode_simple(Opcode op);                  // PING/DISCONNECT/SCAN
std::vector<uint8_t> encode_chat(const std::string& message);   // 65 bytes

// One server->client packet.
struct Event {
    Opcode op = PING;
    std::string nick;     // CONNECT (peer joined), CHAT (sender)
    Mac mac{};            // CONNECT (peer), SCAN (group host), CONNECT_BSSID (group host)
    uint32_t ip = 0;      // CONNECT, DISCONNECT: the 4 wire bytes, a.b.c.d -> a | b<<8 | c<<16 | d<<24
    std::string group;    // SCAN: group name, trailing NULs stripped
    std::string message;  // CHAT
};

// Size of a server->client packet with this opcode, 0 if unknown.
size_t s2c_size(uint8_t opcode);

class Decoder {
public:
    void feed(const uint8_t* data, size_t n);
    // Next complete packet; false if more bytes are needed or after error().
    bool next(Event* out);
    // An unknown opcode was seen: the stream cannot be resynchronised.
    bool error() const { return error_; }

private:
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    bool error_ = false;
};

}  // namespace pro
