// adhoc_config: server/nickname/MAC/port-offset settings from the environment.
#include "net/adhoc_config.h"
#include "psp_machine_id.h"

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
    AdhocConfig c = adhoc_config_load(env_of(env), "", 1234, "test-machine");
    CHECK(c.server_host == "127.0.0.1", "default host");
    CHECK(c.server_port == 27312, "default port");
    CHECK(c.port_offset == 10000, "default port offset");
    CHECK(!c.nickname.empty(), "default nickname");
}

static void server_with_port() {
    Env env{{"PSPRECOMP_ADHOC_SERVER", "example.org:27999"}};
    AdhocConfig c = adhoc_config_load(env_of(env), "", 1, "test-machine");
    CHECK(c.server_host == "example.org", "host part");
    CHECK(c.server_port == 27999, "port part");
    Env env2{{"PSPRECOMP_ADHOC_SERVER", "10.0.0.5"}};
    AdhocConfig c2 = adhoc_config_load(env_of(env2), "", 1, "test-machine");
    CHECK(c2.server_host == "10.0.0.5" && c2.server_port == 27312, "host only keeps default port");
}

static void mac_env_override() {
    const std::string file = temp_file("override");
    { std::ofstream(file) << "02:11:22:33:44:55\n"; }
    Env env{{"PSPRECOMP_ADHOC_MAC", "00:00:00:00:00:0a"}};
    AdhocConfig c = adhoc_config_load(env_of(env), file, 1, "test-machine");
    CHECK(adhoc_format_mac(c.mac) == "00:00:00:00:00:0a", "env MAC wins over the file");
    std::remove(file.c_str());
}

static void mac_persisted_once() {
    const std::string file = temp_file("persist");
    Env env;
    AdhocConfig a = adhoc_config_load(env_of(env), file, 777, "test-machine");
    CHECK((a.mac[0] & 0x03) == 0, "generated MAC: unicast, low bits of byte 0 clear");
    AdhocMac parsed{};
    CHECK(adhoc_parse_mac(adhoc_format_mac(a.mac), &parsed), "generated MAC is valid");
    AdhocConfig b = adhoc_config_load(env_of(env), file, 999, "test-machine");  // other seed
    CHECK(a.mac == b.mac, "second load reuses the persisted MAC");
    AdhocConfig other = adhoc_config_load(env_of(env), temp_file("persist2"), 999, "test-machine");
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
    AdhocConfig c = adhoc_config_load(env_of(env), "", 5, "test-machine");
    CHECK((c.mac[0] & 0x03) == 0 && c.mac != AdhocMac{}, "invalid env MAC falls back to generated");
}

static void nick_truncated() {
    Env env{{"PSPRECOMP_ADHOC_NICK", std::string(200, 'x')}};
    AdhocConfig c = adhoc_config_load(env_of(env), "", 1, "test-machine");
    CHECK(c.nickname.size() == 127, "nickname truncated to 127 bytes");
}

static void port_offset_parsed() {
    Env env{{"PSPRECOMP_ADHOC_PORT_OFFSET", "20000"}};
    CHECK(adhoc_config_load(env_of(env), "", 1, "test-machine").port_offset == 20000, "offset from env");
    Env bad{{"PSPRECOMP_ADHOC_PORT_OFFSET", "70000"}};
    CHECK(adhoc_config_load(env_of(bad), "", 1, "test-machine").port_offset == 10000, "out of range keeps default");
    Env zero{{"PSPRECOMP_ADHOC_PORT_OFFSET", "0"}};
    CHECK(adhoc_config_load(env_of(zero), "", 1, "test-machine").port_offset == 0, "zero offset allowed");
    Env bind{{"PSPRECOMP_ADHOC_BIND", "127.0.0.2"}};
    CHECK(adhoc_config_load(env_of(bind), "", 1, "test-machine").bind_ip == "127.0.0.2", "bind address from env");
    CHECK(adhoc_config_load(env_of(zero), "", 1, "test-machine").bind_ip.empty(), "no bind address by default");
}

static void relay_parsed() {
    Env none{};
    const AdhocConfig d = adhoc_config_load(env_of(none), "", 1, "test-machine");
    CHECK(!d.relay && d.relay_port == 27313, "relay off by default, port 27313");
    for (const char* on : {"1", "on", "true", "yes"}) {
        Env env{{"PSPRECOMP_ADHOC_RELAY", on}};
        CHECK(adhoc_config_load(env_of(env), "", 1, "test-machine").relay, "relay switched on");
    }
    Env off{{"PSPRECOMP_ADHOC_RELAY", "0"}};
    CHECK(!adhoc_config_load(env_of(off), "", 1, "test-machine").relay, "relay 0 stays off");
    Env port{{"PSPRECOMP_ADHOC_RELAY_PORT", "27400"}};
    CHECK(adhoc_config_load(env_of(port), "", 1, "test-machine").relay_port == 27400, "relay port from env");
    Env bad{{"PSPRECOMP_ADHOC_RELAY_PORT", "0"}};
    CHECK(adhoc_config_load(env_of(bad), "", 1, "test-machine").relay_port == 27313, "invalid relay port keeps default");
}

static std::string read_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

static bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

static void machine_hash_stable() {
    const std::string h = adhoc_machine_hash("pc-1");
    bool hex = h.size() == 16;
    for (char ch : h) hex &= (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    CHECK(hex, "machine hash is 16 lowercase hex digits");
    CHECK(h == adhoc_machine_hash("pc-1"), "machine hash is stable");
    CHECK(h != adhoc_machine_hash("pc-2"), "machine hash differs per machine");
    CHECK(adhoc_machine_hash("") == adhoc_machine_hash("unknown"), "empty machine id hashes \"unknown\"");
}

static void mac_file_keeps_mac_on_same_machine() {
    const std::string file = temp_file("same_machine");
    Env env;
    AdhocConfig a = adhoc_config_load(env_of(env), file, 111, "pc-1");
    AdhocConfig b = adhoc_config_load(env_of(env), file, 222, "pc-1");
    CHECK(a.mac == b.mac, "same machine keeps the file's MAC");
    CHECK(ends_with(read_line(file), " " + adhoc_machine_hash("pc-1")), "file records the machine hash");
    std::remove(file.c_str());
}

static void copied_file_other_machine_regenerated() {
    const std::string file = temp_file("copied");
    Env env;
    AdhocConfig a = adhoc_config_load(env_of(env), file, 333, "pc-1");
    AdhocConfig b = adhoc_config_load(env_of(env), file, 333, "pc-2");
    CHECK(a.mac != b.mac, "a copied file on another machine gives a new MAC");
    CHECK(ends_with(read_line(file), " " + adhoc_machine_hash("pc-2")), "file rewritten for the new machine");
    AdhocConfig c = adhoc_config_load(env_of(env), file, 333, "pc-1");
    CHECK(c.mac != b.mac, "copy moved back: regenerated again");
    CHECK(ends_with(read_line(file), " " + adhoc_machine_hash("pc-1")), "file rewritten for the first machine");
    std::remove(file.c_str());
}

static void legacy_mac_file_regenerated() {
    const std::string file = temp_file("legacy");
    { std::ofstream(file) << "02:00:00:00:00:0a\n"; }
    Env env;
    AdhocConfig c = adhoc_config_load(env_of(env), file, 444, "pc-1");
    CHECK(adhoc_format_mac(c.mac) != "02:00:00:00:00:0a", "legacy file without hash regenerated");
    CHECK(ends_with(read_line(file), " " + adhoc_machine_hash("pc-1")), "legacy file gains the hash");
    std::remove(file.c_str());
}

static void env_mac_wins() {
    const std::string file = temp_file("env_wins");
    { std::ofstream(file) << "02:00:00:00:00:0a\n"; }
    Env env{{"PSPRECOMP_ADHOC_MAC", "02:11:22:33:44:55"}};
    AdhocConfig c = adhoc_config_load(env_of(env), file, 1, "pc-9");
    CHECK(adhoc_format_mac(c.mac) == "02:11:22:33:44:55", "env MAC wins over a machine-bound file");
    CHECK(read_line(file) == "02:00:00:00:00:0a", "env MAC leaves the file untouched");
    std::remove(file.c_str());
}

static void machine_id_env_override() {
    const std::string file = temp_file("machine_env");
    Env env{{"PSPRECOMP_ADHOC_MACHINE_ID", "lab-b"}};
    adhoc_config_load(env_of(env), file, 5, "pc-1");
    CHECK(ends_with(read_line(file), " " + adhoc_machine_hash("lab-b")), "PSPRECOMP_ADHOC_MACHINE_ID replaces the machine id");
    std::remove(file.c_str());
}

static void unwritable_mac_file_still_gives_mac() {
    Env env;
    AdhocConfig c = adhoc_config_load(env_of(env), "no_such_dir_adhoc_cfg/sub/adhoc_mac.txt", 6, "pc-1");
    CHECK((c.mac[0] & 0x03) == 0 && c.mac != AdhocMac{}, "unwritable file: a valid MAC in memory");
}

static void host_machine_id() {
    const std::string id = psp_machine_id();
#ifdef _WIN32
    // The MachineGuid value, not the computer-name fallback.
    CHECK(id.size() == 36 && id[8] == '-' && id[13] == '-' && id[18] == '-' && id[23] == '-',
          "Windows machine id is the MachineGuid");
#else
    CHECK(!id.empty(), "machine id found");
#endif
    CHECK(id == psp_machine_id(), "machine id is stable");
}

int main() {
    defaults();
    relay_parsed();
    server_with_port();
    mac_env_override();
    mac_persisted_once();
    bad_mac_rejected();
    nick_truncated();
    port_offset_parsed();
    machine_hash_stable();
    mac_file_keeps_mac_on_same_machine();
    copied_file_other_machine_regenerated();
    legacy_mac_file_regenerated();
    env_mac_wins();
    machine_id_env_override();
    unwritable_mac_file_still_gives_mac();
    host_machine_id();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("adhoc_config: all passed\n");
    return 0;
}
