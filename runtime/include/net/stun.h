// net/stun.h: STUN Binding (RFC 5389) for the direct P2P path's server-reflexive candidate. IPv4 only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stun {

constexpr uint32_t kMagicCookie = 0x2112A442;

std::vector<uint8_t> binding_request(const uint8_t txid[12]);  // 20 bytes, no attributes

// A Binding Success Response (0x0101) with this transaction id: XOR-MAPPED-ADDRESS (0x0020), else
// MAPPED-ADDRESS (0x0001), IPv4 only, host byte order.
bool parse_binding_success(const uint8_t* pkt, size_t len, const uint8_t txid[12], uint32_t* ip, uint16_t* port);

bool looks_like_stun(const uint8_t* pkt, size_t len);  // top two bits 0 and the magic cookie at bytes 4..7

}  // namespace stun
