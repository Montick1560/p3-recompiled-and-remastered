// Unit tests for psp_save_crypt (PPSSPP savedata cipher): crypt-mode tables, encrypt/decrypt
// round trips, hash check, short files, and the user's real PPSSPP save when it is on this PC.
#include "hle/psp_save_crypt.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace psp_save_crypt;

static int failures = 0;
static int tests_run = 0;

#define ASSERT_TRUE(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            failures++; \
        } \
    } while (0)

static const uint8_t kKey[16] = {0x44, 0x39, 0x56, 0x5a, 0x47, 0x32, 0x32, 0x59,
                                 0x4c, 0x57, 0x53, 0x4a, 0x47, 0x4e, 0x4c, 0x9a};

static std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

static std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void test_mode_tables() {
    const uint8_t p0[] = {0x00}, p1[] = {0x01}, p3[] = {0x21}, p5[] = {0x41}, px[] = {0x7F};
    ASSERT_TRUE(mode_from_params(nullptr, 0) == 0, "no params -> plain");
    ASSERT_TRUE(mode_from_params(p0, 1) == 0, "0x00 -> plain");
    ASSERT_TRUE(mode_from_params(p1, 1) == 1, "0x01 -> mode 1");
    ASSERT_TRUE(mode_from_params(p3, 1) == 3, "0x21 -> mode 3");
    ASSERT_TRUE(mode_from_params(p5, 1) == 5, "0x41 -> mode 5");
    ASSERT_TRUE(mode_from_params(px, 1) == 1, "unknown non-zero -> mode 1");
    ASSERT_TRUE(request_mode(false, 0, 0x06060010) == 1, "short request -> mode 1");
    ASSERT_TRUE(request_mode(true, 0, 0x06060010) == 5, "secureVersion 0, new SDK -> 5");
    ASSERT_TRUE(request_mode(true, 0, 0x02050000) == 3, "secureVersion 0, old SDK -> 3");
    ASSERT_TRUE(request_mode(true, 1, 0x06060010) == 1, "secureVersion 1 -> 1");
    ASSERT_TRUE(request_mode(true, 2, 0x06060010) == 3, "secureVersion 2 -> 3");
    ASSERT_TRUE(request_mode(true, 3, 0x06060010) == 5, "secureVersion 3, new SDK -> 5");
    ASSERT_TRUE(request_mode(true, 3, 0x02050000) == 1, "secureVersion 3, old SDK -> 1");
}

static void test_round_trips() {
    for (int mode : {1, 3, 5}) {
        for (size_t n : {size_t{1}, size_t{16}, size_t{5000}, size_t{178848}}) {
            std::vector<uint8_t> plain = pattern(n), enc, dec;
            uint8_t hash[16];
            const uint8_t* key = mode > 1 ? kKey : nullptr;
            int e = encrypt(mode, plain, key, &enc, hash);
            ASSERT_TRUE(e == 0 && enc.size() == n + 16, "encrypt adds the 16-byte IV");
            ASSERT_TRUE(enc != plain, "encrypted bytes differ");
            int d = decrypt(mode, enc, key, hash, &dec);
            ASSERT_TRUE(d == 0 && dec == plain, "decrypt with the hash returns the plain bytes");
            dec.clear();
            ASSERT_TRUE(decrypt(mode, enc, key, nullptr, &dec) == 0 && dec == plain,
                        "decrypt without a hash too");
        }
    }
}

static void test_hash_and_short_files() {
    std::vector<uint8_t> plain = pattern(4096), enc, dec;
    uint8_t hash[16];
    ASSERT_TRUE(encrypt(5, plain, kKey, &enc, hash) == 0, "encrypt");
    uint8_t wrong[16];
    std::memset(wrong, 0xEE, sizeof(wrong));
    ASSERT_TRUE(decrypt(5, enc, kKey, wrong, &dec) == -8, "wrong hash -> -8");
    ASSERT_TRUE(dec.empty(), "nothing returned on failure");
    std::vector<uint8_t> sixteen(enc.begin(), enc.begin() + 16);
    ASSERT_TRUE(decrypt(5, sixteen, kKey, nullptr, &dec) == -1, "16 bytes (IV only) -> -1");
    ASSERT_TRUE(decrypt(5, std::vector<uint8_t>(), kKey, nullptr, &dec) == -1, "empty -> -1");
}

// The user's PPSSPP save (decrypted once with PPSSPP's code on 2026-10-16). No game files in the
// repo: skipped when the folder is not on this PC.
static void test_real_save() {
    const fs::path deco = PSP_DECO_DIR;
    const fs::path enc_path = deco / "build/save_main/original_DATA04/SECURE.BIN";
    const fs::path ref_path = deco / "build/save_main/dec_DATA04.bin";
    if (!fs::exists(enc_path) || !fs::exists(ref_path)) {
        std::printf("SKIP: %s not on this PC\n", enc_path.string().c_str());
        return;
    }
    std::vector<uint8_t> enc = slurp(enc_path), ref = slurp(ref_path), dec;
    ASSERT_TRUE(enc.size() == 178864 && ref.size() == 178848, "real save sizes");
    ASSERT_TRUE(decrypt(5, enc, kKey, nullptr, &dec) == 0, "real save decrypts");
    ASSERT_TRUE(dec == ref, "real save matches PPSSPP's decryption byte for byte");
}

int main() {
    test_mode_tables();
    test_round_trips();
    test_hash_and_short_files();
    test_real_save();
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
