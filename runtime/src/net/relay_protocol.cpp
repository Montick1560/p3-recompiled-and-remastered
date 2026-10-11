#include "net/relay_protocol.h"

#include <algorithm>
#include <cstring>

namespace relay {

namespace {

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

uint16_t wire_port(uint16_t psp, uint16_t offset) {
    if (psp == 0) return 0;
    const auto p = static_cast<uint16_t>(psp + offset);
    return p == 0 ? 65535 : p;
}

uint16_t psp_port(uint16_t wire, uint16_t offset) { return static_cast<uint16_t>(wire - offset); }

std::array<uint8_t, kInitSize> encode_init(InitType type, const pro::Mac& src, uint16_t sport,
                                           const pro::Mac& dst, uint16_t dport) {
    std::array<uint8_t, kInitSize> p{};
    put32(p.data(), static_cast<uint32_t>(type));
    std::memcpy(p.data() + 4, src.data(), 6);
    put16(p.data() + 12, sport);
    std::memcpy(p.data() + 14, dst.data(), 6);
    put16(p.data() + 22, dport);
    return p;
}

std::array<uint8_t, kPdpHeaderSize> encode_pdp_header(const pro::Mac& mac, uint16_t port, uint32_t size) {
    std::array<uint8_t, kPdpHeaderSize> p{};
    std::memcpy(p.data(), mac.data(), 6);
    put16(p.data() + 8, port);
    put32(p.data() + 10, size);
    return p;
}

std::array<uint8_t, kPtpHeaderSize> encode_ptp_header(uint32_t size) {
    std::array<uint8_t, kPtpHeaderSize> p{};
    put32(p.data(), size);
    return p;
}

void parse_connect(const uint8_t* raw, pro::Mac* mac, uint16_t* port) {
    std::memcpy(mac->data(), raw, 6);
    *port = get16(raw + 8);
}

void PdpFrames::feed(const uint8_t* data, int len) {
    if (len > 0) buf_.insert(buf_.end(), data, data + len);
}

int PdpFrames::next_size() const {
    if (buf_.size() < static_cast<size_t>(kPdpHeaderSize)) return -1;
    const uint32_t n = get32(buf_.data() + 10);
    if (buf_.size() < kPdpHeaderSize + static_cast<size_t>(n)) return -1;
    return static_cast<int>(n);
}

int PdpFrames::pop(pro::Mac* from, uint16_t* port, uint8_t* buf, int* len) {
    const int n = next_size();
    if (n < 0) return -1;
    if (n > *len) {
        *len = n;
        return kTooSmall;
    }
    std::memcpy(from->data(), buf_.data(), 6);
    *port = get16(buf_.data() + 8);
    if (n > 0) std::memcpy(buf, buf_.data() + kPdpHeaderSize, static_cast<size_t>(n));
    buf_.erase(buf_.begin(), buf_.begin() + kPdpHeaderSize + n);
    *len = n;
    return 0;
}

void PtpFrames::feed(const uint8_t* data, int len) {
    if (len > 0) buf_.insert(buf_.end(), data, data + len);
}

bool PtpFrames::pending() const {
    if (frame_left_ > 0) return true;
    if (buf_.size() < static_cast<size_t>(kPtpHeaderSize)) return false;
    return buf_.size() >= kPtpHeaderSize + static_cast<size_t>(get32(buf_.data()));
}

int PtpFrames::read(uint8_t* out, int cap) {
    while (frame_left_ == 0) {
        if (!pending()) return 0;
        frame_left_ = get32(buf_.data());
        buf_.erase(buf_.begin(), buf_.begin() + kPtpHeaderSize);  // empty frames are skipped
    }
    const size_t k = std::min(static_cast<size_t>(cap > 0 ? cap : 0), frame_left_);
    std::memcpy(out, buf_.data(), k);
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(k));
    frame_left_ -= k;
    return static_cast<int>(k);
}

}  // namespace relay
