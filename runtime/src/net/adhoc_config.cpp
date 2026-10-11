#include "net/adhoc_config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool adhoc_parse_mac(const std::string& s, AdhocMac* out) {
    if (s.size() != 17) return false;
    AdhocMac mac{};
    for (int i = 0; i < 6; i++) {
        const int hi = hex_digit(s[i * 3]);
        const int lo = hex_digit(s[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && s[i * 3 + 2] != ':' && s[i * 3 + 2] != '-') return false;
        mac[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    bool all_zero = true, all_ff = true;
    for (uint8_t b : mac) {
        all_zero &= b == 0x00;
        all_ff &= b == 0xFF;
    }
    if (all_zero || all_ff) return false;
    *out = mac;
    return true;
}

std::string adhoc_format_mac(const AdhocMac& mac) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

// Like PPSSPP's CreateRandMAC: random bytes with the two low bits of the
// first byte clear (unicast; some games reject other MACs).
static AdhocMac generate_mac(uint32_t seed) {
    uint32_t x = seed ? seed : 0x9E3779B9u;
    AdhocMac mac{};
    for (auto& b : mac) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<uint8_t>(x >> 24);
    }
    mac[0] &= 0xFC;
    if (mac[1] == 0 && mac[2] == 0 && mac[3] == 0 && mac[4] == 0 && mac[5] == 0) {
        mac[5] = 1;  // never the all-zero MAC the server rejects
    }
    return mac;
}

static bool read_mac_file(const std::string& path, AdhocMac* out) {
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line)) return false;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return adhoc_parse_mac(line, out);
}

static void write_mac_file(const std::string& path, const AdhocMac& mac) {
    std::ofstream out(path, std::ios::trunc);
    if (out) out << adhoc_format_mac(mac) << "\n";
}

AdhocConfig adhoc_config_load(const std::function<const char*(const char*)>& getenv_fn,
                              const std::string& mac_file, uint32_t random_seed) {
    AdhocConfig cfg;
    if (const char* server = getenv_fn("PSPRECOMP_ADHOC_SERVER"); server && *server) {
        std::string s = server;
        const size_t colon = s.rfind(':');
        if (colon != std::string::npos) {
            const long port = std::strtol(s.c_str() + colon + 1, nullptr, 10);
            if (port > 0 && port <= 65535) cfg.server_port = static_cast<uint16_t>(port);
            s.resize(colon);
        }
        if (!s.empty()) cfg.server_host = s;
    }
    if (const char* nick = getenv_fn("PSPRECOMP_ADHOC_NICK"); nick && *nick) {
        cfg.nickname = nick;
        if (cfg.nickname.size() > 127) cfg.nickname.resize(127);
    }
    if (const char* off = getenv_fn("PSPRECOMP_ADHOC_PORT_OFFSET"); off && *off) {
        char* end = nullptr;
        const long v = std::strtol(off, &end, 10);
        if (end != off && *end == 0 && v >= 0 && v <= 65535) {
            cfg.port_offset = static_cast<uint16_t>(v);
        }
    }
    if (const char* bind = getenv_fn("PSPRECOMP_ADHOC_BIND"); bind && *bind) cfg.bind_ip = bind;
    if (const char* relay = getenv_fn("PSPRECOMP_ADHOC_RELAY"); relay && *relay) {
        const std::string v = relay;
        cfg.relay = v == "1" || v == "on" || v == "true" || v == "yes";
    }
    if (const char* rport = getenv_fn("PSPRECOMP_ADHOC_RELAY_PORT"); rport && *rport) {
        char* end = nullptr;
        const long v = std::strtol(rport, &end, 10);
        if (end != rport && *end == 0 && v > 0 && v <= 65535) cfg.relay_port = static_cast<uint16_t>(v);
    }
    const char* env_mac = getenv_fn("PSPRECOMP_ADHOC_MAC");
    if (env_mac && adhoc_parse_mac(env_mac, &cfg.mac)) return cfg;
    if (!mac_file.empty() && read_mac_file(mac_file, &cfg.mac)) return cfg;
    cfg.mac = generate_mac(random_seed);
    if (!mac_file.empty()) write_mac_file(mac_file, cfg.mac);
    return cfg;
}
