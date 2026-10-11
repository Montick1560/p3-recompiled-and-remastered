// PRO ad hoc server packets. Layouts follow PPSSPP Core/HLE/proAdhoc.h
// (GPL-2.0-or-later): every field has a fixed size, strings are NUL-padded
// and are not guaranteed to be terminated.
#include "net/pro_protocol.h"

#include <cstring>

namespace pro {

static void put_fixed(std::vector<uint8_t>& out, const std::string& s, size_t len,
                      bool keep_nul) {
    const size_t max = keep_nul ? len - 1 : len;
    const size_t n = s.size() < max ? s.size() : max;
    out.insert(out.end(), s.begin(), s.begin() + n);
    out.insert(out.end(), len - n, 0);
}

static std::string get_fixed(const uint8_t* p, size_t len) {
    size_t n = 0;
    while (n < len && p[n] != 0) n++;
    return std::string(reinterpret_cast<const char*>(p), n);
}

std::vector<uint8_t> encode_login(const Mac& mac, const std::string& nick,
                                  const std::string& product) {
    std::vector<uint8_t> out;
    out.reserve(1 + 6 + kNickLen + kProductLen);
    out.push_back(LOGIN);
    out.insert(out.end(), mac.begin(), mac.end());
    put_fixed(out, nick, kNickLen, true);
    put_fixed(out, product, kProductLen, false);
    return out;
}

std::vector<uint8_t> encode_connect(const std::string& group) {
    std::vector<uint8_t> out;
    out.push_back(CONNECT);
    put_fixed(out, group, kGroupLen, false);
    return out;
}

std::vector<uint8_t> encode_simple(Opcode op) { return {static_cast<uint8_t>(op)}; }

std::vector<uint8_t> encode_chat(const std::string& message) {
    std::vector<uint8_t> out;
    out.push_back(CHAT);
    put_fixed(out, message, kChatLen, true);
    return out;
}

size_t s2c_size(uint8_t opcode) {
    switch (opcode) {
    case PING: return 1;
    case CONNECT: return 1 + kNickLen + 6 + 4;
    case DISCONNECT: return 1 + 4;
    case SCAN: return 1 + kGroupLen + 6;
    case SCAN_COMPLETE: return 1;
    case CONNECT_BSSID: return 1 + 6;
    case CHAT: return 1 + kChatLen + kNickLen;
    default: return 0;
    }
}

static uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void Decoder::feed(const uint8_t* data, size_t n) {
    if (error_) return;
    if (pos_ > 0 && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    } else if (pos_ > 4096) {
        buf_.erase(buf_.begin(), buf_.begin() + pos_);
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data, data + n);
}

bool Decoder::next(Event* out) {
    if (error_ || pos_ >= buf_.size()) return false;
    const uint8_t op = buf_[pos_];
    const size_t size = s2c_size(op);
    if (size == 0) {
        error_ = true;
        return false;
    }
    if (buf_.size() - pos_ < size) return false;
    const uint8_t* p = buf_.data() + pos_ + 1;
    Event e;
    e.op = static_cast<Opcode>(op);
    switch (op) {
    case CONNECT:
        e.nick = get_fixed(p, kNickLen);
        std::memcpy(e.mac.data(), p + kNickLen, 6);
        e.ip = get_u32(p + kNickLen + 6);
        break;
    case DISCONNECT:
        e.ip = get_u32(p);
        break;
    case SCAN:
        e.group = get_fixed(p, kGroupLen);
        std::memcpy(e.mac.data(), p + kGroupLen, 6);
        break;
    case CONNECT_BSSID:
        std::memcpy(e.mac.data(), p, 6);
        break;
    case CHAT:
        e.message = get_fixed(p, kChatLen);
        e.nick = get_fixed(p + kChatLen, kNickLen);
        break;
    default:
        break;
    }
    pos_ += size;
    *out = std::move(e);
    return true;
}

}  // namespace pro
