// Unit tests for psp_eboot_loader ([image] source = "disc"): image building from synthetic ELFs,
// and the three real Patapon 3 EBOOTs (encrypted == PPSSPP dump) when they are on this PC.
#include "psp_eboot_loader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// ELF32 LE ET_EXEC with one PT_LOAD whose bytes start at file offset 0x54
// (52-byte header + one 32-byte program header).
static std::vector<uint8_t> make_elf(uint32_t vaddr, const std::vector<uint8_t>& bytes, uint32_t memsz) {
    std::vector<uint8_t> f(0x54, 0);
    auto w16 = [&](size_t o, uint16_t v) { f[o] = uint8_t(v); f[o + 1] = uint8_t(v >> 8); };
    auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) f[o + i] = uint8_t(v >> (8 * i)); };
    f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F'; f[4] = 1; f[5] = 1; f[6] = 1;
    w16(16, 2); w16(18, 8); w32(20, 1); w32(28, 52); w16(40, 52); w16(42, 32); w16(44, 1);
    w32(52, 1); w32(56, 0x54); w32(60, vaddr); w32(64, vaddr);
    w32(68, uint32_t(bytes.size())); w32(72, memsz);
    f.insert(f.end(), bytes.begin(), bytes.end());
    return f;
}

static const std::vector<uint8_t> kBytes = {1, 2, 3, 4};
static const std::vector<uint8_t> kImage = {1, 2, 3, 4, 0, 0, 0, 0};
static const PspImageSegment kSeg{0x08804000U, 4, 8};
static PspExpectedImage want() { return {&kSeg, 1, psp_eboot_fnv1a64(kImage.data(), kImage.size())}; }

static void fnv_vectors() {
    CHECK(psp_eboot_fnv1a64(nullptr, 0) == 0xCBF29CE484222325ULL, "fnv of empty");
    CHECK(psp_eboot_fnv1a64(reinterpret_cast<const uint8_t*>("a"), 1) == 0xAF63DC4C8601EC8CULL, "fnv of a");
}

static void plain_elf_loads() {
    std::vector<uint8_t> img; std::string d;
    CHECK(psp_eboot_build_image(make_elf(0x08804000U, kBytes, 8), want(), img, d) == EbootStatus::Ok, "plain ELF ok");
    CHECK(img == kImage, "file bytes then zeros up to memsz");
    CHECK(d == "plain ELF", "detail plain ELF");
}

static void refusals() {
    std::vector<uint8_t> img; std::string d;
    CHECK(psp_eboot_build_image(make_elf(0x08804000U, {1, 2, 3, 5}, 8), want(), img, d) == EbootStatus::HashMismatch, "other bytes -> hash mismatch");
    CHECK(img.empty(), "no image on mismatch");
    CHECK(psp_eboot_build_image(make_elf(0x08900000U, kBytes, 8), want(), img, d) == EbootStatus::LayoutMismatch, "other vaddr");
    CHECK(psp_eboot_build_image(make_elf(0x08804000U, kBytes, 9), want(), img, d) == EbootStatus::LayoutMismatch, "other memsz");
    auto cut = make_elf(0x08804000U, kBytes, 8); cut.resize(cut.size() - 2);
    CHECK(psp_eboot_build_image(cut, want(), img, d) == EbootStatus::NotElf, "truncated segment");
    CHECK(psp_eboot_build_image(std::vector<uint8_t>(100, 'x'), want(), img, d) == EbootStatus::NotElf, "not an ELF");
    CHECK(psp_eboot_build_image({}, want(), img, d) == EbootStatus::NotElf, "empty file");
    auto exe = make_elf(0x08804000U, kBytes, 8); exe[16] = 3;   // ET_DYN
    CHECK(psp_eboot_build_image(exe, want(), img, d) == EbootStatus::NotElf, "not ET_EXEC");
    std::vector<uint8_t> psp(0x200, 0); std::memcpy(psp.data(), "~PSP", 4);
    CHECK(psp_eboot_build_image(psp, want(), img, d) == EbootStatus::DecryptFailed, "garbage ~PSP");
}

static void host_paths() {
    CHECK(psp_eboot_host_path("C:/P 3/versions/x/disc0", "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN")
          == "C:/P 3/versions/x/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", "disc0 path");
    CHECK(psp_eboot_host_path("D:\\d\\disc0\\", "disc0:/PSP_GAME/SYSDIR/BOOT.BIN")
          == "D:\\d\\disc0/PSP_GAME/SYSDIR/BOOT.BIN", "trailing separator dropped");
}

static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The single PT_LOAD of all three releases: file offset 0xb4, vaddr 0x08804000, filesz 0x297860,
// memsz 0x3C2480 (build/spike_decrypt/AUDIT_deepseek.md section 1.4).
static void real_releases() {
    const std::string deco = PSP_DECO_DIR;
    const char* pairs[][2] = {
        {"/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", "/INFN00001_EBOOT.BIN"},
        {"/build/v115b/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", "/build/v115b/EBOOT_dec.BIN"},
        {"/build/vanilla/disc0/PSP_GAME/SYSDIR/EBOOT.BIN", "/build/vanilla/EBOOT_dec.BIN"},
    };
    for (auto& p : pairs) {
        std::vector<uint8_t> enc = slurp(deco + p[0]), dump = slurp(deco + p[1]);
        if (enc.size() < 0x200 || dump.size() < 0xB4 + 0x297860) {
            std::printf("SKIP real EBOOT %s (not on this PC)\n", p[0]);
            continue;
        }
        std::vector<uint8_t> ref(dump.begin() + 0xB4, dump.begin() + 0xB4 + 0x297860);
        ref.resize(0x3C2480, 0);
        PspImageSegment seg{0x08804000U, 0x297860U, 0x3C2480U};
        PspExpectedImage w{&seg, 1, psp_eboot_fnv1a64(ref.data(), ref.size())};
        std::vector<uint8_t> a, b; std::string da, db;
        CHECK(psp_eboot_build_image(enc, w, a, da) == EbootStatus::Ok && da == "decrypted", p[0]);
        CHECK(psp_eboot_build_image(dump, w, b, db) == EbootStatus::Ok && db == "plain ELF", p[1]);
        CHECK(a == ref && b == ref, "decrypted image equals the PPSSPP dump's image");
    }
}

int main() {
    fnv_vectors();
    plain_elf_loads();
    refusals();
    host_paths();
    real_releases();
    if (failures == 0) std::printf("test_eboot_loader: all passed\n");
    return failures == 0 ? 0 : 1;
}
