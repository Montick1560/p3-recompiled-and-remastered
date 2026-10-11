// matching_packet: PSP ad hoc matching packets, byte layouts from PPSSPP's builders.
#include "net/matching_packet.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

using matching::Packet;

static const pro::Mac kA = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0a};
static const pro::Mac kB = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0b};

static void layouts() {
    Packet hello;
    hello.type = matching::HELLO;
    hello.opt = {0xAA, 0xBB, 0xCC};
    CHECK(matching::encode(hello) == (std::vector<uint8_t>{1, 3, 0, 0, 0, 0xAA, 0xBB, 0xCC}), "HELLO");

    Packet join;
    join.type = matching::JOIN;
    CHECK(matching::encode(join) == (std::vector<uint8_t>{2, 0, 0, 0, 0}), "JOIN without opt");

    Packet accept;
    accept.type = matching::ACCEPT;
    accept.opt = {0x11};
    accept.siblings = {kB};
    const std::vector<uint8_t> want_accept = {3, 1, 0, 0, 0, 1, 0, 0, 0, 0x11, 0, 0, 0, 0, 0, 0x0b};
    CHECK(matching::encode(accept) == want_accept, "ACCEPT with one sibling");

    Packet cancel;
    cancel.type = matching::CANCEL;
    cancel.opt = {7, 8};
    CHECK(matching::encode(cancel) == (std::vector<uint8_t>{4, 2, 0, 0, 0, 7, 8}), "CANCEL");

    Packet bulk;
    bulk.type = matching::BULK;
    bulk.opt = {9};
    CHECK(matching::encode(bulk) == (std::vector<uint8_t>{5, 1, 0, 0, 0, 9}), "BULK");

    Packet birth;
    birth.type = matching::BIRTH;
    birth.mac = kA;
    CHECK(matching::encode(birth) == (std::vector<uint8_t>{7, 0, 0, 0, 0, 0, 0x0a}), "BIRTH");
    birth.type = matching::DEATH;
    CHECK(matching::encode(birth) == (std::vector<uint8_t>{8, 0, 0, 0, 0, 0, 0x0a}), "DEATH");

    Packet bye;
    bye.type = matching::BYE;
    CHECK(matching::encode(bye) == std::vector<uint8_t>{9}, "BYE is one byte");
    Packet ping;
    CHECK(matching::encode(ping) == std::vector<uint8_t>{0}, "PING is one byte");
}

static void round_trip() {
    Packet a;
    a.type = matching::ACCEPT;
    a.opt = {1, 2, 3, 4};
    a.siblings = {kA, kB};
    const auto bytes = matching::encode(a);
    Packet b;
    CHECK(matching::decode(bytes.data(), bytes.size(), &b), "ACCEPT decodes");
    CHECK(b.type == matching::ACCEPT && b.opt == a.opt && b.siblings == a.siblings, "ACCEPT fields");
    Packet h;
    h.type = matching::HELLO;
    h.opt.assign(300, 0x5A);
    const auto hb = matching::encode(h);
    CHECK(matching::decode(hb.data(), hb.size(), &b) && b.type == matching::HELLO && b.opt.size() == 300,
          "large HELLO");
    const uint8_t death[7] = {8, 0, 0, 0, 0, 0, 0x0b};
    CHECK(matching::decode(death, 7, &b) && b.type == matching::DEATH && b.mac == kB, "DEATH");
    const uint8_t extra[8] = {1, 1, 0, 0, 0, 0x42, 0xFF, 0xFF};  // trailing bytes are tolerated
    CHECK(matching::decode(extra, 8, &b) && b.opt == std::vector<uint8_t>{0x42}, "trailing bytes ignored");
}

static void malformed_rejected() {
    Packet p;
    CHECK(!matching::decode(nullptr, 0, &p), "empty");
    const uint8_t big[20] = {1, 100, 0, 0, 0};
    CHECK(!matching::decode(big, sizeof(big), &p), "HELLO length beyond the datagram");
    const uint8_t neg[9] = {2, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
    CHECK(!matching::decode(neg, sizeof(neg), &p), "negative length");
    const uint8_t short_hdr[3] = {4, 1, 0};
    CHECK(!matching::decode(short_hdr, sizeof(short_hdr), &p), "truncated header");
    const uint8_t acc[12] = {3, 0, 0, 0, 0, 2, 0, 0, 0, 1, 2, 3};
    CHECK(!matching::decode(acc, sizeof(acc), &p), "ACCEPT siblings beyond the datagram");
    const uint8_t birth[4] = {7, 0, 0, 0};
    CHECK(!matching::decode(birth, sizeof(birth), &p), "BIRTH without a full MAC");
    const uint8_t unknown[1] = {0x42};
    CHECK(!matching::decode(unknown, 1, &p), "unknown type");
}

int main() {
    layouts();
    round_trip();
    malformed_rejected();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("matching_packet: all passed\n");
    return 0;
}
