// adhoc_config: server/nickname/MAC/port-offset settings from the environment.
#include "net/adhoc_config.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

using Env = std::map<std::string, std::string>;

static std::function<const char*(const char*)> env_of(const Env& env) {
    return [&env](const char* name) -> const char* {
        auto it = env.find(name);
        return it == env.end() ? nullptr : it->second.c_str();
    };
}

static std::string temp_file(const char* name) {
    std::string path = std::string("adhoc_cfg_test_") + name + ".txt";
    std::remove(path.c_str());
    return path;
}

static void defaults() {
    Env env;
    AdhocConfig c = adhoc_config_load(env_of(env), "", 1234);
    CHECK(c.server_host == "127.0.0.1", "default host");
    CHECK(c.server_port == 27312, "default port");
    CHECK(c.port_offset == 10000, "default port offset");
    CHECK(!c.nickname.empty(), "default nickname");
}

static void server_with_port() {
    Env env{{"PSPRECOMP_ADHOC_SERVER", "example.org:27999"}};
    AdhocConfig c = adhoc_config_load(env_of(env), "", 1);
    CHECK(c.server_host == "example.org", "host part");
    CHECK(c.server_port == 27999, "port part");
    Env env2{{"PSPRECOMP_ADHOC_SERVER", "10.0.0.5"}};
    AdhocConfig c2 = adhoc_config_load(env_of(env2), "", 1);
    CHECK(c2.server_host == "10.0.0.5" && c2.server_port == 27312, "host only keeps default port");
}

static void mac_env_override() {
    const std::string file = temp_file("override");
    { std::ofstream(file) << "02:11:22:33:44:55\n"; }
    Env env{{"PSPRECOMP_ADHOC_MAC", "00:00:00:00:00:0a"}};
    AdhocConfig c = adhoc_config_load(env_of(env), file, 1);
    CHECK(adhoc_format_mac(c.mac) == "00:00:00:00:00:0a", "env MAC wins over the file");
    std::remove(file.c_str());
}

static void mac_persisted_once() {
    const std::string file = temp_file("persist");
    Env env;
    AdhocConfig a = adhoc_config_load(env_of(env), file, 777);
    CHECK((a.mac[0] & 0x03) == 0, "generated MAC: unicast, low bits of byte 0 clear");
    AdhocMac parsed{};
    CHECK(adhoc_parse_mac(adhoc_format_mac(a.mac), &parsed), "generated MAC is valid");
    AdhocConfig b = adhoc_config_load(env_of(env), file, 999);  // other seed
    CHECK(a.mac == b.mac, "second load reuses the persisted MAC");
    AdhocConfig other = adhoc_config_load(env_of(env), temp_file("persist2"), 999);
    CHECK(other.mac != a.mac, "different seeds give different MACs");
    std::remove(file.c_str());
    std::remove("adhoc_cfg_test_persist2.txt");
}

static void bad_mac_rejected() {
    AdhocMac m{};
    CHECK(!adhoc_parse_mac("zz:00:00:00:00:01", &m), "non-hex rejected");
    CHECK(!adhoc_parse_mac("00:00:00:00:01", &m), "five bytes rejected");
    CHECK(!adhoc_parse_mac("00:00:00:00:00:00", &m), "all-zero rejected");
    CHECK(!adhoc_parse_mac("ff:ff:ff:ff:ff:ff", &m), "broadcast rejected");
    CHECK(adhoc_parse_mac("0A-1b-2C-3d-4E-5f", &m) && m[0] == 0x0A && m[5] == 0x5F,
          "dashes and mixed case accepted");
    Env env{{"PSPRECOMP_ADHOC_MAC", "garbage"}};
    AdhocConfig c = adhoc_config_load(env_of(env), "", 5);
    CHECK((c.mac[0] & 0x03) == 0 && c.mac != AdhocMac{}, "invalid env MAC falls back to generated");
}

static void nick_truncated() {
    Env env{{"PSPRECOMP_ADHOC_NICK", std::string(200, 'x')}};
    AdhocConfig c = adhoc_config_load(env_of(env), "", 1);
    CHECK(c.nickname.size() == 127, "nickname truncated to 127 bytes");
}

static void port_offset_parsed() {
    Env env{{"PSPRECOMP_ADHOC_PORT_OFFSET", "20000"}};
    CHECK(adhoc_config_load(env_of(env), "", 1).port_offset == 20000, "offset from env");
    Env bad{{"PSPRECOMP_ADHOC_PORT_OFFSET", "70000"}};
    CHECK(adhoc_config_load(env_of(bad), "", 1).port_offset == 10000, "out of range keeps default");
    Env zero{{"PSPRECOMP_ADHOC_PORT_OFFSET", "0"}};
    CHECK(adhoc_config_load(env_of(zero), "", 1).port_offset == 0, "zero offset allowed");
    Env bind{{"PSPRECOMP_ADHOC_BIND", "127.0.0.2"}};
    CHECK(adhoc_config_load(env_of(bind), "", 1).bind_ip == "127.0.0.2", "bind address from env");
    CHECK(adhoc_config_load(env_of(zero), "", 1).bind_ip.empty(), "no bind address by default");
}

int main() {
    defaults();
    server_with_port();
    mac_env_override();
    mac_persisted_once();
    bad_mac_rejected();
    nick_truncated();
    port_offset_parsed();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("adhoc_config: all passed\n");
    return 0;
}
