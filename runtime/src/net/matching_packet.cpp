// PSP ad hoc matching packets. Layouts and receive-side length checks follow
// PPSSPP Core/HLE/sceNetAdhocMatching.cpp (GPL-2.0-or-later).
#include "net/matching_packet.h"

#include <cstring>

namespace matching {

static void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

static int32_t get_i32(const uint8_t* p) {
    uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    int32_t s;
    std::memcpy(&s, &v, 4);
    return s;
}

std::vector<uint8_t> encode(const Packet& p) {
    std::vector<uint8_t> out;
    out.push_back(p.type);
    switch (p.type) {
    case HELLO:
    case JOIN:
    case CANCEL:
    case BULK:
        put_u32(out, static_cast<uint32_t>(p.opt.size()));
        out.insert(out.end(), p.opt.begin(), p.opt.end());
        break;
    case ACCEPT:
        put_u32(out, static_cast<uint32_t>(p.opt.size()));
        put_u32(out, static_cast<uint32_t>(p.siblings.size()));
        out.insert(out.end(), p.opt.begin(), p.opt.end());
        for (const auto& m : p.siblings) out.insert(out.end(), m.begin(), m.end());
        break;
    case BIRTH:
    case DEATH:
        out.insert(out.end(), p.mac.begin(), p.mac.end());
        break;
    default:  // PING, BYE, BULK_ABORT: opcode only
        break;
    }
    return out;
}

bool decode(const uint8_t* data, size_t len, Packet* out) {
    if (data == nullptr || len == 0) return false;
    Packet p;
    p.type = static_cast<PacketType>(data[0]);
    switch (data[0]) {
    case PING:
    case BYE:
    case BULK_ABORT:
        break;
    case HELLO:
    case JOIN:
    case CANCEL:
    case BULK: {
        if (len < 5) return false;
        const int32_t n = get_i32(data + 1);
        if (n < 0 || static_cast<uint64_t>(n) > len - 5) return false;
        p.opt.assign(data + 5, data + 5 + n);
        break;
    }
    case ACCEPT: {
        if (len < 9) return false;
        const int32_t n = get_i32(data + 1);
        const int32_t siblings = get_i32(data + 5);
        if (n < 0 || siblings < 0 ||
            static_cast<uint64_t>(len) < 9ULL + static_cast<uint64_t>(n) + 6ULL * static_cast<uint64_t>(siblings)) {
            return false;
        }
        p.opt.assign(data + 9, data + 9 + n);
        const uint8_t* m = data + 9 + n;
        for (int32_t i = 0; i < siblings; i++, m += 6) {
            pro::Mac mac;
            std::memcpy(mac.data(), m, 6);
            p.siblings.push_back(mac);
        }
        break;
    }
    case BIRTH:
    case DEATH:
        if (len < 7) return false;
        std::memcpy(p.mac.data(), data + 1, 6);
        break;
    default:
        return false;
    }
    *out = std::move(p);
    return true;
}

}  // namespace matching
