// net/direct_crypto.h: sealed direct-path packets (P3D1 header, XChaCha20-Poly1305) and the replay window.
#include "net/direct_crypto.h"

#include <cstdio>
#include <random>
#include <vector>

using namespace direct;

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static Key key_of(uint8_t seed) {
    Key k;
    for (size_t i = 0; i < k.size(); i++) k[i] = static_cast<uint8_t>(seed + i);
    return k;
}

int main() {
    const Key k = key_of(1);
    {   // roundtrip_each_type
        bool ok = true;
        for (int t = 1; t <= 7; t++) {
            for (size_t len : {size_t{0}, size_t{1}, size_t{1000}}) {
                std::vector<uint8_t> body(len);
                for (size_t i = 0; i < len; i++) body[i] = static_cast<uint8_t>(i * 7 + t);
                const auto pkt = seal(k, 0xA1B2C3D4, 42, static_cast<Type>(t), body.data(), len);
                Opened o;
                ok = ok && pkt.size() == kHeaderSize + 1 + len + kTagSize && open(k, pkt.data(), pkt.size(), &o) &&
                     o.pair_id == 0xA1B2C3D4 && o.counter == 42 && o.type == static_cast<Type>(t) && o.body == body;
                uint32_t pid = 0;
                uint64_t ctr = 0;
                ok = ok && read_header(pkt.data(), pkt.size(), &pid, &ctr) && pid == 0xA1B2C3D4 && ctr == 42;
            }
        }
        CHECK(ok, "roundtrip_each_type");
    }
    {   // keys_mirror
        uint8_t pk[32];
        for (int i = 0; i < 32; i++) pk[i] = static_cast<uint8_t>(i * 3);
        const Keys ab = derive_keys(pk, 0x0A500002, 0x0A500003), ba = derive_keys(pk, 0x0A500003, 0x0A500002);
        CHECK(ab.tx == ba.rx && ab.rx == ba.tx, "keys_mirror: tx/rx swap");
        CHECK(ab.tx != ab.rx, "keys_mirror: tx != rx");
        Keys w = ab;
        w.wipe();
        CHECK(w.tx == Key{} && w.rx == Key{}, "wipe zeroes both keys");
    }
    {   // bit_flip_rejected
        std::vector<uint8_t> body(200 - kHeaderSize - kTagSize - 1, 0x5A);
        const auto pkt = seal(k, 7, 9, Type::Dgram, body.data(), body.size());
        CHECK(pkt.size() == 200, "200-byte packet");
        bool any = false;
        for (size_t bit = 0; bit < pkt.size() * 8; bit++) {
            auto p = pkt;
            p[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
            Opened o;
            any = any || open(k, p.data(), p.size(), &o);
        }
        CHECK(!any, "bit_flip_rejected");
    }
    {   // wrong_key_rejected, truncated_rejected, oversize_refused
        const uint8_t body[3] = {1, 2, 3};
        const auto pkt = seal(k, 7, 9, Type::Dgram, body, 3);
        Opened o;
        CHECK(!open(key_of(2), pkt.data(), pkt.size(), &o), "wrong_key_rejected");
        bool any = false;
        for (size_t len = 0; len < pkt.size(); len++) any = any || open(k, pkt.data(), len, &o);
        CHECK(!any, "truncated_rejected");
        std::vector<uint8_t> big(kMaxPacket - kHeaderSize - kTagSize - 1);
        CHECK(seal(k, 7, 9, Type::Seg, big.data(), big.size()).size() == kMaxPacket, "largest body fits");
        big.push_back(0);
        CHECK(seal(k, 7, 9, Type::Seg, big.data(), big.size()).empty(), "oversize_refused");
    }
    {   // counter_rules
        CHECK(seal(k, 7, 0, Type::Dgram, nullptr, 0).empty(), "counter 0 refused");
        CHECK(seal(k, 7, kCounterLimit, Type::Dgram, nullptr, 0).empty(), "counter limit refused");
        CHECK(!seal(k, 7, kCounterLimit - 1, Type::Dgram, nullptr, 0).empty(), "counter limit - 1 ok");
    }
    {   // unknown type: sealed with a valid key but type 0 or 8
        Opened o;
        CHECK(seal(k, 7, 1, static_cast<Type>(0), nullptr, 0).empty(), "seal type 0 refused");
        CHECK(seal(k, 7, 1, static_cast<Type>(8), nullptr, 0).empty(), "seal type 8 refused");
        (void)o;
    }
    {   // replay_window
        ReplayWindow w;
        CHECK(!w.check(0), "0 never valid");
        for (uint64_t c = 1; c <= 5; c++) w.accept(c);
        CHECK(!w.check(5), "duplicate 5");
        ReplayWindow v;
        v.accept(5);
        CHECK(v.check(4), "4 still in the window");
        v.accept(3000);
        CHECK(!v.check(900), "900 older than newest - 2047");
        CHECK(v.check(2000), "2000 in the window");
        CHECK(v.check(953), "953 = newest - 2047 in the window");  // newest-2047 .. newest = 2048 counters
        CHECK(!v.check(952), "952 = newest - 2048 out");
        v.accept(2000);
        CHECK(!v.check(2000), "2000 accepted once");
        v.accept(uint64_t{1} << 40);
        CHECK(!v.check(3000) && v.check((uint64_t{1} << 40) + 1), "huge jump");
        CHECK(!v.check(uint64_t{1} << 40), "newest after a huge jump is a duplicate");
    }
    {   // fuzz_open
        std::mt19937_64 rng(1234);
        bool any = false;
        std::vector<uint8_t> buf;
        for (int i = 0; i < 200000; i++) {
            buf.resize(rng() % 1501);
            for (auto& b : buf) b = static_cast<uint8_t>(rng());
            if (buf.size() >= 4 && (i & 1)) buf[0] = 'P', buf[1] = '3', buf[2] = 'D', buf[3] = '1';
            Opened o;
            any = any || open(k, buf.data(), buf.size(), &o);
        }
        const uint8_t body[40] = {};
        const auto pkt = seal(k, 99, 77, Type::Seg, body, sizeof body);
        for (int i = 0; i < 200000; i++) {
            auto p = pkt;
            uint8_t delta = static_cast<uint8_t>(rng());
            p[rng() % p.size()] ^= delta ? delta : 1;
            Opened o;
            any = any || open(k, p.data(), p.size(), &o);
        }
        CHECK(!any, "fuzz_open: never true");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("direct crypto: all passed\n");
    return 0;
}
