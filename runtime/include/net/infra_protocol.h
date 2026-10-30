#pragma once
// Wire format of the infrastructure-mode room server (tools/infra_server.py,
// docs/superpowers/specs/2026-10-22-p3-infrastructure-mode-design.md):
// frames `u32 length (LE, of kind + body) | u8 kind | body`. Kind 1 is a
// UTF-8 JSON object, kind 2 a game datagram `u32 vip | u16 dst_port |
// u16 src_port | payload` (to the server: destination vip; from it: source);
// kind 3 a segment of a game stream socket (TCP P2P) in the same layout,
// payload[0] being the segment type (net/p2p_stream.h); kind 4 a direct-path
// packet relayed between paired players, `u32 vip | sealed bytes` (net/direct_path.h).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

namespace infra {

constexpr uint8_t kKindJson = 1;
constexpr uint8_t kKindDatagram = 2;
constexpr uint8_t kKindStream = 3;
constexpr uint8_t kKindChannel = 4;
constexpr uint32_t kMaxFrame = 64 * 1024;  // kind + body, as the server enforces

struct Frame {
    uint8_t kind = 0;
    nlohmann::json json;            // kind 1
    uint32_t vip = 0;               // kinds 2, 3 and 4
    uint16_t dst_port = 0, src_port = 0;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> encode_json(const nlohmann::json& j);
std::vector<uint8_t> encode_datagram(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t* data,
                                     size_t len);
// Kind 2 or 3 frame: u32 vip | u16 dst_port | u16 src_port | data.
std::vector<uint8_t> encode_segment(uint8_t kind, uint32_t vip, uint16_t dst_port, uint16_t src_port,
                                    const uint8_t* data, size_t len);

// Kind 4 frame: u32 vip | sealed.
std::vector<uint8_t> encode_channel(uint32_t vip, const uint8_t* sealed, size_t len);

// Reassembles frames from a byte stream. A malformed frame (bad JSON, a JSON
// value that is not an object, a short datagram, an unknown kind) is skipped
// and counted; a length over kMaxFrame breaks the stream for good.
class FrameReader {
public:
    void feed(const uint8_t* data, size_t len);
    bool next(Frame* out);
    int bad_frames() const { return bad_; }
    bool broken() const { return broken_; }

private:
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    int bad_ = 0;
    bool broken_ = false;
};

std::string to_hex(const uint8_t* data, size_t len);
std::vector<uint8_t> from_hex(const std::string& hex);  // empty on malformed input

}  // namespace infra
