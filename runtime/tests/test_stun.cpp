// net/stun.h: STUN Binding request/response codec (RFC 5389), IPv4 only.
#include "net/stun.h"
#include "net/direct_crypto.h"

#include <cstdio>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

// RFC 5769 2.2 "Sample IPv4 Response" (192.0.2.1:32853).
static const std::vector<uint8_t> kRfcResponse = {
    0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
    0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63,
    0x74, 0x6f, 0x72, 0x20, 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
    0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9,
    0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};
static const uint8_t kRfcTxid[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};

int main() {
    uint32_t ip = 0;
    uint16_t port = 0;
    {   // rfc5769_ipv4_response
        CHECK(stun::parse_binding_success(kRfcResponse.data(), kRfcResponse.size(), kRfcTxid, &ip, &port),
              "rfc5769: parses");
        CHECK(ip == 0xC0000201 && port == 32853, "rfc5769: 192.0.2.1:32853");
    }
    {   // request_layout
        const auto r = stun::binding_request(kRfcTxid);
        CHECK(r.size() == 20, "request: 20 bytes");
        CHECK(r[0] == 0x00 && r[1] == 0x01 && r[2] == 0 && r[3] == 0, "request: type 0x0001, length 0");
        CHECK(r[4] == 0x21 && r[5] == 0x12 && r[6] == 0xa4 && r[7] == 0x42, "request: cookie");
        CHECK(std::equal(r.begin() + 8, r.end(), kRfcTxid), "request: txid");
    }
    {   // wrong_txid, wrong_type, attribute_overruns_length, ipv6_only_response
        uint8_t other[12] = {};
        CHECK(!stun::parse_binding_success(kRfcResponse.data(), kRfcResponse.size(), other, &ip, &port), "wrong_txid");
        auto err = kRfcResponse;
        err[1] = 0x11;  // 0x0111 error response
        CHECK(!stun::parse_binding_success(err.data(), err.size(), kRfcTxid, &ip, &port), "wrong_type");
        auto over = kRfcResponse;
        over[0x26] = 0x00, over[0x27] = 0x40;  // XOR-MAPPED-ADDRESS length 64: past the message
        CHECK(!stun::parse_binding_success(over.data(), over.size(), kRfcTxid, &ip, &port), "attribute overruns");
        auto lenbad = kRfcResponse;
        lenbad[3] = 0x50;  // message length past the buffer
        CHECK(!stun::parse_binding_success(lenbad.data(), lenbad.size(), kRfcTxid, &ip, &port), "length overruns");
        auto v6 = kRfcResponse;
        v6[0x29] = 0x02;  // family IPv6
        CHECK(!stun::parse_binding_success(v6.data(), v6.size(), kRfcTxid, &ip, &port), "ipv6_only_response");
        // MAPPED-ADDRESS (0x0001) is the fallback when there is no XOR-MAPPED-ADDRESS.
        std::vector<uint8_t> plain = {0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42};
        plain.insert(plain.end(), kRfcTxid, kRfcTxid + 12);
        for (uint8_t b : {0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x12, 0x34, 198, 51, 100, 7}) plain.push_back(b);
        CHECK(stun::parse_binding_success(plain.data(), plain.size(), kRfcTxid, &ip, &port) && ip == 0xC6336407 &&
                  port == 0x1234,
              "MAPPED-ADDRESS fallback");
    }
    {   // demux
        direct::Key k{};
        const auto p = direct::seal(k, 1, 1, direct::Type::Probe, nullptr, 0);
        CHECK(!stun::looks_like_stun(p.data(), p.size()), "demux: P3D1 is not STUN");
        CHECK(stun::looks_like_stun(kRfcResponse.data(), kRfcResponse.size()), "demux: the RFC response is STUN");
        CHECK(!stun::looks_like_stun(kRfcResponse.data(), 19), "demux: short is not STUN");
    }
    {   // fuzz_parse
        std::mt19937_64 rng(77);
        bool wrong = false;
        uint8_t other[12] = {1};
        std::vector<uint8_t> buf;
        for (int i = 0; i < 200000; i++) {
            if (i & 1) {
                buf = kRfcResponse;
                uint8_t d = static_cast<uint8_t>(rng());
                buf[rng() % buf.size()] ^= d ? d : 1;
                buf.resize(rng() % (buf.size() + 1));
            } else {
                buf.resize(rng() % 600);
                for (auto& b : buf) b = static_cast<uint8_t>(rng());
            }
            wrong = wrong || stun::parse_binding_success(buf.data(), buf.size(), other, &ip, &port);
            stun::parse_binding_success(buf.data(), buf.size(), kRfcTxid, &ip, &port);
            stun::looks_like_stun(buf.data(), buf.size());
        }
        CHECK(!wrong, "fuzz_parse: never true with a wrong txid");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("stun: all passed\n");
    return 0;
}
