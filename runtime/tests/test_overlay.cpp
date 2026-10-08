// Unit tests for code-overlay bank resolution (psp_overlay.cpp): which bank
// answers a lookup in the overlay window, and the code-hash safety check.
// Standalone executable: links psp_overlay.cpp only (no SDL/GL/scheduler).

#include "psp_overlay.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_TRUE(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    } while (0)

static constexpr uint32_t LO = 0x08ABB180U;
static constexpr uint32_t HI = 0x08BC6480U;
static constexpr uint32_t TEXT = 0x40U;  // tiny text section for the test

static void fn_a(uint8_t*, recomp_context*) {}
static void fn_b(uint8_t*, recomp_context*) {}

static uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ULL; }
    return h;
}

static uint8_t* window(std::vector<uint8_t>& ram) { return ram.data() + (LO & 0x07FFFFFFU); }

// Writes an MWo3 header (id) + TEXT bytes of code into the window.
static void load_overlay(std::vector<uint8_t>& ram, uint32_t id, uint8_t fill) {
    uint8_t* w = window(ram);
    std::memset(w, 0, 0x40 + TEXT);
    std::memcpy(w, "MWo3", 4);
    std::memcpy(w + 4, &id, 4);
    std::memcpy(w + 8, &LO, 4);
    std::memcpy(w + 12, &TEXT, 4);
    std::memset(w + 0x40, fill, TEXT);
}

static const uint32_t k_addrs[] = {LO + 0x40, LO + 0x80};
static const FuncPtr k_fns[] = {fn_a, fn_b};

int main() {
    std::vector<uint8_t> ram(0x08000000, 0);

    load_overlay(ram, 1, 0xAB);
    static const PspOverlayBank bank = {
        "ovTest", 1, LO, HI, 0x40 + TEXT, fnv1a64(window(ram), 0x40 + TEXT),
        k_addrs, k_fns, 2,
    };
    psp_overlay_reset_for_tests();
    psp_overlay_register_bank(&bank);

    FuncPtr out = nullptr;
    ASSERT_TRUE(psp_overlay_resolve(ram.data(), LO + 0x80, &out) == PspOverlayStatus::Found
                && out == fn_b, "address of a bank function resolves to it");
    ASSERT_TRUE(psp_overlay_resolve(ram.data(), 0x08A00000U, &out)
                == PspOverlayStatus::NotInWindow, "address outside the window");
    ASSERT_TRUE(psp_overlay_resolve(ram.data(), LO + 0x44, &out)
                == PspOverlayStatus::NotFound, "window address with no bank entry");

    load_overlay(ram, 2, 0xAB);  // overlay id without a bank
    ASSERT_TRUE(psp_overlay_resolve(ram.data(), LO + 0x40, &out)
                == PspOverlayStatus::NoBank, "loaded id has no bank");

    std::memset(window(ram), 0, 0x40 + TEXT);  // nothing loaded (no MWo3)
    ASSERT_TRUE(psp_overlay_resolve(ram.data(), LO + 0x40, &out)
                == PspOverlayStatus::NoBank, "no MWo3 header in the window");

    load_overlay(ram, 1, 0xCD);  // same id, different code
    ASSERT_TRUE(psp_overlay_resolve(ram.data(), LO + 0x40, &out)
                == PspOverlayStatus::HashMismatch, "code differs from the compiled bank");

    load_overlay(ram, 1, 0xAB);  // the right overlay again
    ASSERT_TRUE(psp_overlay_resolve(ram.data(), LO + 0x40, &out) == PspOverlayStatus::Found
                && out == fn_a, "reloading the matching overlay resolves again");

    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
