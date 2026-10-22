// Frame codec of the infrastructure-mode server (see net/infra_protocol.h).
#include "net/infra_protocol.h"

#include <cstring>

namespace infra {

static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
static void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
static uint32_t get_u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

std::vector<uint8_t> encode_json(const nlohmann::json& j) {
    const std::string text = j.dump();
    std::vector<uint8_t> v;
    v.reserve(5 + text.size());
    put_u32(v, static_cast<uint32_t>(1 + text.size()));
    v.push_back(kKindJson);
    v.insert(v.end(), text.begin(), text.end());
    return v;
}

std::vector<uint8_t> encode_datagram(uint32_t vip, uint16_t dst_port, uint16_t src_port, const uint8_t* data,
                                     size_t len) {
    std::vector<uint8_t> v;
    v.reserve(13 + len);
    put_u32(v, static_cast<uint32_t>(1 + 8 + len));
    v.push_back(kKindDatagram);
    put_u32(v, vip);
    put_u16(v, dst_port);
    put_u16(v, src_port);
    if (len) v.insert(v.end(), data, data + len);
    return v;
}

void FrameReader::feed(const uint8_t* data, size_t len) {
    if (broken_ || !len) return;
    if (pos_ && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data, data + len);
}

bool FrameReader::next(Frame* out) {
    while (!broken_ && buf_.size() - pos_ >= 4) {
        const uint32_t n = get_u32(buf_.data() + pos_);
        if (n == 0 || n > kMaxFrame) {
            broken_ = true;
            buf_.clear();
            pos_ = 0;
            return false;
        }
        if (buf_.size() - pos_ < 4 + static_cast<size_t>(n)) break;
        const uint8_t* body = buf_.data() + pos_ + 5;
        const size_t blen = n - 1;
        const uint8_t kind = buf_[pos_ + 4];
        pos_ += 4 + n;
        Frame f;
        f.kind = kind;
        if (kind == kKindJson) {
            f.json = nlohmann::json::parse(body, body + blen, nullptr, false);
            if (f.json.is_discarded() || !f.json.is_object()) {
                bad_++;
                continue;
            }
        } else if (kind == kKindDatagram && blen >= 8) {
            f.vip = get_u32(body);
            f.dst_port = get_u16(body + 4);
            f.src_port = get_u16(body + 6);
            f.payload.assign(body + 8, body + blen);
        } else {
            bad_++;
            continue;
        }
        if (pos_ > 64 * 1024) {  // keep the buffer from growing with consumed bytes
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
            pos_ = 0;
        }
        *out = std::move(f);
        return true;
    }
    return false;
}

std::string to_hex(const uint8_t* data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 15]);
    }
    return s;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> v;
    if (hex.size() % 2) return v;
    v.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        v.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return v;
}

}  // namespace infra
