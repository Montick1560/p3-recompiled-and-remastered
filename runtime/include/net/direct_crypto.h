// net/direct_crypto.h: sealed packets of the direct P2P path (spec "Security model"). Monocypher only:
// XChaCha20-Poly1305 with the 16-byte header as AD, per-direction keys from BLAKE2b, a 2048-counter replay window.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace direct {

constexpr size_t kHeaderSize = 16, kTagSize = 16, kMaxPacket = 1400;
constexpr uint64_t kCounterLimit = 1ull << 62;

enum class Type : uint8_t { Probe = 1, ProbeAck = 2, Keepalive = 3, Dgram = 4, Seg = 5, Ack = 6, Close = 7 };

using Key = std::array<uint8_t, 32>;
struct Keys {
    Key tx{}, rx{};
    void wipe();
};

// tx = BLAKE2b-256(key = pair_key, "p3d1 dir" || be32(my_vip) || be32(peer_vip)); rx with the vips swapped.
Keys derive_keys(const uint8_t pair_key[32], uint32_t my_vip, uint32_t peer_vip);

// "P3D1" | le32 pair_id | le64 counter | AEAD(type || body) | tag. Nonce = le64 counter || 16 zero bytes,
// AD = the 16-byte header. Empty when counter == 0, counter >= kCounterLimit, an unknown type, or > kMaxPacket.
std::vector<uint8_t> seal(const Key& key, uint32_t pair_id, uint64_t counter, Type type, const uint8_t* body,
                          size_t len);

// Magic and minimum length only; nothing here is authenticated yet.
bool read_header(const uint8_t* pkt, size_t len, uint32_t* pair_id, uint64_t* counter);

struct Opened {
    uint32_t pair_id = 0;
    uint64_t counter = 0;
    Type type{};
    std::vector<uint8_t> body;
};
// false: bad magic / size / tag, counter 0 or over the limit, or an unknown type.
bool open(const Key& key, const uint8_t* pkt, size_t len, Opened* out);

class ReplayWindow {  // the newest counter and the 2047 below it
public:
    bool check(uint64_t counter) const;  // false: 0, duplicate, or older than newest - 2047
    void accept(uint64_t counter);       // only after open() succeeded
private:
    static constexpr uint64_t kSize = 2048;
    uint64_t newest_ = 0;
    std::array<uint64_t, kSize / 64> seen_{};  // bit (c % kSize) set: c accepted
    bool test(uint64_t c) const { return seen_[(c % kSize) / 64] >> (c % 64) & 1; }
};

}  // namespace direct
