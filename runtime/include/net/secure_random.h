// net/secure_random.h: bytes from the OS CSPRNG (direct P2P keys' companions: challenges, txids, nonces).
#pragma once

#include <cstddef>
#include <cstdint>

namespace net {
// false when the OS gives no randomness; callers then keep the direct path off.
bool secure_random(uint8_t* out, size_t len);
}  // namespace net
