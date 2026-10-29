// Vendored Monocypher against published vectors, and the OS CSPRNG (direct P2P).
#include "monocypher.h"
#include "net/secure_random.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> out;
    for (; s[0] && s[1]; s += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(s, 2), nullptr, 16)));
    return out;
}

int main() {
    {   // xchacha_kat: draft-irtf-cfrg-xchacha-03 A.3.1
        const char* text = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                           "sunscreen would be it.";
        const auto ad = hex("50515253c0c1c2c3c4c5c6c7");
        const auto key = hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        const auto nonce = hex("404142434445464748494a4b4c4d4e4f5051525354555657");
        const auto want = hex("bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb"
                              "731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
                              "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"
                              "21f9664c97637da9768812f615c68b13b52e");
        const auto want_tag = hex("c0875924c1c7987947deafd8780acf49");
        const size_t n = std::strlen(text);
        std::vector<uint8_t> ct(n);
        uint8_t tag[16];
        crypto_aead_lock(ct.data(), tag, key.data(), nonce.data(), ad.data(), ad.size(),
                         reinterpret_cast<const uint8_t*>(text), n);
        CHECK(ct == want, "xchacha: ciphertext");
        CHECK(std::memcmp(tag, want_tag.data(), 16) == 0, "xchacha: tag");
        std::vector<uint8_t> back(n);
        CHECK(crypto_aead_unlock(back.data(), tag, key.data(), nonce.data(), ad.data(), ad.size(), ct.data(), n) == 0,
              "xchacha: unlock");
        CHECK(std::memcmp(back.data(), text, n) == 0, "xchacha: plaintext back");
    }
    {   // blake2b_kat: RFC 7693 Appendix A
        const auto want = hex("ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
                              "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
        uint8_t h[64];
        crypto_blake2b(h, 64, reinterpret_cast<const uint8_t*>("abc"), 3);
        CHECK(std::memcmp(h, want.data(), 64) == 0, "blake2b(abc)");
    }
    {   // random_nonconstant
        uint8_t a[32] = {}, b[32] = {}, zero[32] = {};
        CHECK(net::secure_random(a, sizeof a) && net::secure_random(b, sizeof b), "secure_random succeeds");
        CHECK(std::memcmp(a, b, 32) != 0, "two draws differ");
        CHECK(std::memcmp(a, zero, 32) != 0, "not all zero");
        std::vector<uint8_t> big(1000);  // more than one getentropy chunk
        CHECK(net::secure_random(big.data(), big.size()), "1000 bytes");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("direct crypto primitives: all passed\n");
    return 0;
}
