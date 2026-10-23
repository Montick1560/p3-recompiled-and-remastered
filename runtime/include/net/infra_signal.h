#pragma once
// sceNetApctlGetInfo(STRENGTH) in infrastructure mode: the Wi-Fi signal in %,
// taken from the round trip to the infrastructure server (net/infra_client.h
// rtt_ms). Patapon 3 does not ask for it in the sessions recorded so far (its
// team-totem meter comes from elsewhere, see the design spec); other code
// paths get a value that follows the real connection. Pure.
#include <cstdint>

namespace infra {

inline uint8_t strength_from_rtt(int rtt_ms) {
    if (rtt_ms < 0 || rtt_ms <= 80) return 99;
    if (rtt_ms <= 150) return 75;
    if (rtt_ms <= 250) return 50;
    if (rtt_ms <= 400) return 25;
    return 5;
}

}  // namespace infra
