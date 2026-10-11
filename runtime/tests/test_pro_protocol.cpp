// pro_protocol: byte layouts of the PRO ad hoc server packets (PPSSPP proAdhoc.h).
#include "net/pro_protocol.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static const pro::Mac kMac = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

static void login_layout() {
    auto p = pro::encode_login(kMac, "Hero", "UCES01421");
    CHECK(p.size() == 144, "login is 144 bytes");
    CHECK(p[0] == pro::LOGIN, "login opcode");
    CHECK(std::memcmp(&p[1], kMac.data(), 6) == 0, "MAC at 1..6");
    CHECK(std::memcmp(&p[7], "Hero", 4) == 0 && p[11] == 0 && p[134] == 0,
          "nickname NUL-padded to 128");
    CHECK(std::memcmp(&p[135], "UCES01421", 9) == 0, "product code at 135..143");
    auto long_nick = pro::encode_login(kMac, std::string(300, 'n'), "UCES01421");
    CHECK(long_nick.size() == 144 && long_nick[7 + 127] == 0,
          "long nickname truncated, keeps a NUL");
}

static void connect_layout() {
    auto p = pro::encode_connect("ABC");
    CHECK(p.size() == 9 && p[0] == pro::CONNECT, "connect is 9 bytes");
    CHECK(std::memcmp(&p[1], "ABC\0\0\0\0\0", 8) == 0, "group NUL-padded to 8");
    auto q = pro::encode_connect("ABCDEFGHIJ");
    CHECK(q.size() == 9 && std::memcmp(&q[1], "ABCDEFGH", 8) == 0, "group truncated to 8");
    CHECK(pro::encode_simple(pro::SCAN) == std::vector<uint8_t>{pro::SCAN}, "scan is one byte");
    auto c = pro::encode_chat("hi");
    CHECK(c.size() == 65 && c[0] == pro::CHAT && c[1] == 'h' && c[3] == 0, "chat is 65 bytes");
}

static std::vector<uint8_t> s2c_connect(const char* nick, const pro::Mac& mac,
                                        const uint8_t ip[4]) {
    std::vector<uint8_t> p(139, 0);
    p[0] = pro::CONNECT;
    std::memcpy(&p[1], nick, std::strlen(nick));
    std::memcpy(&p[129], mac.data(), 6);
    std::memcpy(&p[135], ip, 4);
    return p;
}

static constexpr uint32_t ip_of(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return a | b << 8 | c << 16 | d << 24;
}

static void decode_each_s2c() {
    const uint8_t ip[4] = {192, 168, 1, 20};
    std::vector<uint8_t> s;
    auto add = [&s](const std::vector<uint8_t>& p) { s.insert(s.end(), p.begin(), p.end()); };
    add(s2c_connect("Peer", kMac, ip));
    add({pro::DISCONNECT, 192, 168, 1, 20});
    std::vector<uint8_t> scan(15, 0);
    scan[0] = pro::SCAN;
    std::memcpy(&scan[1], "Room1", 5);
    std::memcpy(&scan[9], kMac.data(), 6);
    add(scan);
    add({pro::SCAN_COMPLETE});
    std::vector<uint8_t> bssid(7, 0);
    bssid[0] = pro::CONNECT_BSSID;
    std::memcpy(&bssid[1], kMac.data(), 6);
    add(bssid);
    std::vector<uint8_t> chat(193, 0);
    chat[0] = pro::CHAT;
    std::memcpy(&chat[1], "hello", 5);
    std::memcpy(&chat[65], "Peer", 4);
    add(chat);

    pro::Decoder d;
    d.feed(s.data(), s.size());
    pro::Event e;
    CHECK(d.next(&e) && e.op == pro::CONNECT && e.nick == "Peer" && e.mac == kMac, "S2C connect");
    CHECK(e.ip == ip_of(192, 168, 1, 20), "S2C connect ip in wire order");
    CHECK(d.next(&e) && e.op == pro::DISCONNECT && e.ip == ip_of(192, 168, 1, 20), "S2C disconnect");
    CHECK(d.next(&e) && e.op == pro::SCAN && e.group == "Room1" && e.mac == kMac, "S2C scan");
    CHECK(d.next(&e) && e.op == pro::SCAN_COMPLETE, "S2C scan complete");
    CHECK(d.next(&e) && e.op == pro::CONNECT_BSSID && e.mac == kMac, "S2C connect bssid");
    CHECK(d.next(&e) && e.op == pro::CHAT && e.message == "hello" && e.nick == "Peer", "S2C chat");
    CHECK(!d.next(&e) && !d.error(), "stream drained");
}

static void split_and_coalesced() {
    const uint8_t ip[4] = {10, 0, 0, 2};
    std::vector<uint8_t> both = s2c_connect("A", kMac, ip);
    both.push_back(pro::SCAN_COMPLETE);

    pro::Decoder bytewise;
    std::vector<pro::Event> got;
    for (uint8_t b : both) {
        bytewise.feed(&b, 1);
        pro::Event e;
        while (bytewise.next(&e)) got.push_back(e);
    }
    pro::Decoder whole;
    whole.feed(both.data(), both.size());
    std::vector<pro::Event> got2;
    pro::Event e;
    while (whole.next(&e)) got2.push_back(e);
    CHECK(got.size() == 2 && got2.size() == 2, "two events either way");
    CHECK(got.size() == 2 && got[0].op == pro::CONNECT && got[0].nick == "A" &&
              got[1].op == pro::SCAN_COMPLETE,
          "byte-by-byte decode");
    CHECK(got2.size() == 2 && got.size() == 2 && got2[0].ip == got[0].ip && got2[0].ip == ip_of(10, 0, 0, 2),
          "same result when coalesced");
}

static void fixed_length_fields() {
    const uint8_t ip[4] = {1, 2, 3, 4};
    const std::string nick128(128, 'Z');
    std::vector<uint8_t> p(139, 0);
    p[0] = pro::CONNECT;
    std::memcpy(&p[1], nick128.data(), 128);  // no terminator
    std::memcpy(&p[135], ip, 4);
    pro::Decoder d;
    d.feed(p.data(), p.size());
    pro::Event e;
    CHECK(d.next(&e) && e.nick == nick128, "unterminated nickname kept at 128 bytes");
    CHECK(e.mac == pro::Mac{} && e.ip == ip_of(1, 2, 3, 4), "fields after the nickname intact");
    std::vector<uint8_t> scan(15, 0);
    scan[0] = pro::SCAN;
    const uint8_t name[8] = {0xC3, 0x91, 'A', 'B', 'C', 'D', 'E', 'F'};  // non-ASCII, no NUL
    std::memcpy(&scan[1], name, 8);
    d.feed(scan.data(), scan.size());
    CHECK(d.next(&e) && e.group.size() == 8, "8-byte group without NUL");
}

static void unknown_opcode_sets_error() {
    pro::Decoder d;
    const uint8_t junk[2] = {0x42, 0x00};
    d.feed(junk, 2);
    pro::Event e;
    CHECK(!d.next(&e) && d.error(), "unknown opcode is an error");
    const uint8_t ok = pro::SCAN_COMPLETE;
    d.feed(&ok, 1);
    CHECK(!d.next(&e), "no events after an error");
    CHECK(pro::s2c_size(pro::CONNECT) == 139 && pro::s2c_size(pro::CHAT) == 193 &&
              pro::s2c_size(pro::PING) == 1 && pro::s2c_size(0x42) == 0,
          "size table");
}

int main() {
    login_layout();
    connect_layout();
    decode_each_s2c();
    split_and_coalesced();
    fixed_length_fields();
    unknown_opcode_sets_error();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("pro_protocol: all passed\n");
    return 0;
}
