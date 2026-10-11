#pragma once
// PSP ad hoc matching packets as PPSSPP sends them over PDP
// (Core/HLE/sceNetAdhocMatching.cpp send*/actOn*): opcode byte, then
// little-endian lengths and data. Decoding applies PPSSPP's length checks;
// a packet that fails them is dropped by the caller.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "net/pro_protocol.h"

namespace matching {

enum PacketType : uint8_t {
    PING = 0,
    HELLO = 1,
    JOIN = 2,
    ACCEPT = 3,
    CANCEL = 4,
    BULK = 5,
    BULK_ABORT = 6,
    BIRTH = 7,
    DEATH = 8,
    BYE = 9,
};

struct Packet {
    PacketType type = PING;
    std::vector<uint8_t> opt;        // HELLO/JOIN/ACCEPT/CANCEL optional data, BULK data
    std::vector<pro::Mac> siblings;  // ACCEPT (parent mode: the other children)
    pro::Mac mac{};                  // BIRTH/DEATH: the child that joined/left
};

std::vector<uint8_t> encode(const Packet& p);
// False when the datagram is empty, of an unknown type, or shorter than its
// own length fields say.
bool decode(const uint8_t* data, size_t len, Packet* out);

}  // namespace matching
