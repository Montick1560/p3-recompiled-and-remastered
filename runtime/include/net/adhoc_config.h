#pragma once
// Ad hoc multiplayer settings: which PRO ad hoc server to use, the player's
// nickname, the console MAC the game sees and the host port offset for peer
// sockets. Values come from environment variables; a MAC generated on the
// first run is persisted so the player keeps the same identity. The MAC file
// is bound to the machine: a folder copied to another PC gets its own MAC,
// because the game tells players apart by the OpenPSID built from it.
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
    std::string bind_ip;  // local address for this instance (e.g. 127.0.0.2), empty = any
    bool relay = false;   // PDP/PTP through the server's aemu_postoffice relay
    uint16_t relay_port = 27313;
    std::string machine_hash;  // adhoc_machine_hash() of the machine the MAC file belongs to
};

// Environment variables read through getenv_fn:
//   PSPRECOMP_ADHOC_SERVER       host or host:port
//   PSPRECOMP_ADHOC_NICK         nickname
//   PSPRECOMP_ADHOC_MAC          aa:bb:cc:dd:ee:ff (wins over mac_file)
//   PSPRECOMP_ADHOC_PORT_OFFSET  0..65535
//   PSPRECOMP_ADHOC_BIND         local IPv4 address to use (several instances on one PC)
//   PSPRECOMP_ADHOC_RELAY        1/on/true/yes: data through the server's relay (Paragate)
//   PSPRECOMP_ADHOC_RELAY_PORT   relay TCP port (default 27313)
//   PSPRECOMP_ADHOC_MACHINE_ID   replaces machine_id (testing: two "machines" on one PC)
// Without a valid PSPRECOMP_ADHOC_MAC the MAC is read from mac_file, one line
// "aa:bb:cc:dd:ee:ff <hash16>", and used only when the hash is
// adhoc_machine_hash(machine_id). Otherwise (missing, older one-field file,
// other machine) a MAC is generated from random_seed and the hash and written there.
AdhocConfig adhoc_config_load(const std::function<const char*(const char*)>& getenv_fn,
                              const std::string& mac_file, uint32_t random_seed,
                              const std::string& machine_id);

// FNV-1a 64 of machine_id as 16 lowercase hex digits; "" hashes "unknown".
std::string adhoc_machine_hash(const std::string& machine_id);

// "aa:bb:cc:dd:ee:ff" (hex, ':' or '-'); rejects all-zero and broadcast.
bool adhoc_parse_mac(const std::string& s, AdhocMac* out);
std::string adhoc_format_mac(const AdhocMac& mac);
