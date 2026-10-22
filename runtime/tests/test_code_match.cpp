// psp_code_match.h: is this function's code loaded at a shared overlay address?
#include "psp_code_match.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

// A fake guest memory: addresses 0x08000000 + offset (the runtime masks with 0x07FFFFFF).
static std::vector<uint8_t> mem(0x10000);
static void w32(uint32_t a, uint32_t v) { std::memcpy(&mem[a & 0x07FFFFFFu], &v, 4); }

static const uint32_t kAddr = 0x08000200;
static const uint32_t kCode[] = {0x27BDFF90u, 0xAFBF001Cu, 0xAFB10018u, 0xAFB00014u,
                                 0xE7B4000Cu, 0x00A08821u, 0x8C8501A4u};
static const size_t kN = sizeof kCode / sizeof kCode[0];

int main() {
    CHECK(!psp_code_matches(mem.data(), kAddr, kCode, kN), "no bank loaded (all zero): no match");
    for (size_t i = 0; i < kN; i++) w32(kAddr + 4 * static_cast<uint32_t>(i), kCode[i]);
    CHECK(psp_code_matches(mem.data(), kAddr, kCode, kN), "the same code words match");
    CHECK(psp_code_matches(mem.data(), kAddr | 0x40000000u, kCode, kN), "the address is masked like guest memory");
    w32(kAddr + 4 * 6, 0x8C8501A8u);
    CHECK(!psp_code_matches(mem.data(), kAddr, kCode, kN), "the last word off: no match");
    CHECK(psp_code_matches(mem.data(), kAddr, kCode, 4), "a shorter prefix still matches");
    w32(kAddr, 0x27BDFFA0u);
    CHECK(!psp_code_matches(mem.data(), kAddr, kCode, kN), "the first word off: no match");
    if (failures) return 1;
    std::printf("code_match: all tests passed\n");
    return 0;
}
