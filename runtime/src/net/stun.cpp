#include "net/stun.h"

#include <cstring>

namespace stun {

namespace {

uint32_t be(const uint8_t* p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

// An IPv4 (MAPPED or XOR-MAPPED) address value: reserved, family 1, port, address.
bool read_addr(const uint8_t* v, size_t len, bool xored, uint32_t* ip, uint16_t* port) {
    if (len != 8 || v[1] != 0x01) return false;
    *port = static_cast<uint16_t>(be(v + 2, 2) ^ (xored ? kMagicCookie >> 16 : 0));
    *ip = be(v + 4, 4) ^ (xored ? kMagicCookie : 0);
    return true;
}

}  // namespace

std::vector<uint8_t> binding_request(const uint8_t txid[12]) {
    std::vector<uint8_t> r = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
    r.insert(r.end(), txid, txid + 12);
    return r;
}

bool looks_like_stun(const uint8_t* pkt, size_t len) {
    return len >= 20 && (pkt[0] & 0xC0) == 0 && be(pkt + 4, 4) == kMagicCookie;
}

bool parse_binding_success(const uint8_t* pkt, size_t len, const uint8_t txid[12], uint32_t* ip, uint16_t* port) {
    if (!looks_like_stun(pkt, len) || be(pkt, 2) != 0x0101 || std::memcmp(pkt + 8, txid, 12) != 0) return false;
    const size_t end = 20 + be(pkt + 2, 2);
    if (end > len || (end & 3)) return false;
    bool have_plain = false, have_xor = false;
    uint32_t pip = 0, xip = 0;
    uint16_t pport = 0, xport = 0;
    for (size_t at = 20; at < end;) {
        if (end - at < 4) return false;
        const uint32_t type = be(pkt + at, 2), alen = be(pkt + at + 2, 2);
        if (alen > end - at - 4) return false;
        const uint8_t* v = pkt + at + 4;
        if (type == 0x0020 && !have_xor) have_xor = read_addr(v, alen, true, &xip, &xport);
        if (type == 0x0001 && !have_plain) have_plain = read_addr(v, alen, false, &pip, &pport);
        at += 4 + ((alen + 3) & ~size_t{3});
    }
    if (have_xor) *ip = xip, *port = xport;
    else if (have_plain) *ip = pip, *port = pport;
    return have_xor || have_plain;
}

}  // namespace stun
