#pragma once
// Ad hoc multiplayer settings: which PRO ad hoc server to use, the player's
// nickname, the console MAC the game sees and the host port offset for peer
// sockets. Values come from environment variables; a MAC generated on the
// first run is persisted so the player keeps the same identity.
#include <array>
#include <cstdint>
#include <functional>
#include <string>

using AdhocMac = std::array<uint8_t, 6>;

struct AdhocConfig {
    std::string server_host = "127.0.0.1";
    uint16_t server_port = 27312;
    std::string nickname = "Patapon";  // at most 127 bytes (PRO field is 128)
    AdhocMac mac{};
    uint16_t port_offset = 10000;
};

// Environment variables read through getenv_fn:
//   PSPRECOMP_ADHOC_SERVER       host or host:port
//   PSPRECOMP_ADHOC_NICK         nickname
//   PSPRECOMP_ADHOC_MAC          aa:bb:cc:dd:ee:ff (wins over mac_file)
//   PSPRECOMP_ADHOC_PORT_OFFSET  0..65535
// Without a valid PSPRECOMP_ADHOC_MAC the MAC is read from mac_file; if that
// is missing or invalid a MAC is generated from random_seed and written there.
AdhocConfig adhoc_config_load(const std::function<const char*(const char*)>& getenv_fn,
                              const std::string& mac_file, uint32_t random_seed);

// "aa:bb:cc:dd:ee:ff" (hex, ':' or '-'); rejects all-zero and broadcast.
bool adhoc_parse_mac(const std::string& s, AdhocMac* out);
std::string adhoc_format_mac(const AdhocMac& mac);
