// Unit tests for the sub-interrupt handler table (psp_subintr.cpp):
// register/release/enable/disable with PPSSPP's error codes, and the list
// of enabled handlers a vblank dispatches. Standalone: no SDL, no guest code.

#include "psp_subintr.h"

#include <cstdio>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        if ((actual) != (expected)) { \
            std::fprintf(stderr, "FAIL: %s: got 0x%X, expected 0x%X\n", msg, \
                static_cast<unsigned>(actual), static_cast<unsigned>(expected)); \
            failures++; \
        } \
    } while (0)

static void test_registered_handler_fires_only_when_enabled() {
    PspSubIntrTable t;
    ASSERT_EQ(t.register_handler(PSP_INTR_VBLANK, 15, 0x08861D54u, 0x09000000u),
              0, "register ok");
    ASSERT_EQ(t.enabled(PSP_INTR_VBLANK).size(), 0u, "not enabled yet");
    ASSERT_EQ(t.enable(PSP_INTR_VBLANK, 15), 0, "enable ok");
    auto hs = t.enabled(PSP_INTR_VBLANK);
    ASSERT_EQ(hs.size(), 1u, "one enabled handler");
    ASSERT_EQ(hs[0].sub, 15, "sub number");
    ASSERT_EQ(hs[0].handler, 0x08861D54u, "handler address");
    ASSERT_EQ(hs[0].arg, 0x09000000u, "handler arg");
    ASSERT_EQ(t.enabled(25).size(), 0u, "other interrupt unaffected");
}

static void test_disable_and_release() {
    PspSubIntrTable t;
    t.register_handler(PSP_INTR_VBLANK, 3, 0x1000u, 0);
    t.enable(PSP_INTR_VBLANK, 3);
    ASSERT_EQ(t.disable(PSP_INTR_VBLANK, 3), 0, "disable ok");
    ASSERT_EQ(t.enabled(PSP_INTR_VBLANK).size(), 0u, "disabled not listed");
    ASSERT_EQ(t.release(PSP_INTR_VBLANK, 3), 0, "release ok");
    ASSERT_EQ(t.enable(PSP_INTR_VBLANK, 3),
              static_cast<int>(SCE_KERNEL_ERROR_NOTFOUND_HANDLER),
              "enable after release");
    ASSERT_EQ(t.release(PSP_INTR_VBLANK, 3),
              static_cast<int>(SCE_KERNEL_ERROR_NOTFOUND_HANDLER),
              "double release");
}

static void test_errors() {
    PspSubIntrTable t;
    ASSERT_EQ(t.register_handler(PSP_NUM_INTERRUPTS, 0, 0x1000u, 0),
              static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE),
              "interrupt out of range");
    ASSERT_EQ(t.register_handler(PSP_INTR_VBLANK, PSP_NUM_SUBINTERRUPTS, 0x1000u, 0),
              static_cast<int>(SCE_KERNEL_ERROR_ILLEGAL_INTRCODE),
              "subinterrupt out of range");
    ASSERT_EQ(t.register_handler(PSP_INTR_VBLANK, 1, 0x1000u, 0), 0, "first");
    ASSERT_EQ(t.register_handler(PSP_INTR_VBLANK, 1, 0x2000u, 0),
              static_cast<int>(SCE_KERNEL_ERROR_FOUND_HANDLER),
              "slot already taken");
    ASSERT_EQ(t.enable(PSP_INTR_VBLANK, 2),
              static_cast<int>(SCE_KERNEL_ERROR_NOTFOUND_HANDLER),
              "enable unregistered");
}

static void test_enabled_in_sub_order() {
    PspSubIntrTable t;
    t.register_handler(PSP_INTR_VBLANK, 9, 0x9000u, 0);
    t.register_handler(PSP_INTR_VBLANK, 2, 0x2000u, 0);
    t.enable(PSP_INTR_VBLANK, 9);
    t.enable(PSP_INTR_VBLANK, 2);
    auto hs = t.enabled(PSP_INTR_VBLANK);
    ASSERT_EQ(hs.size(), 2u, "two handlers");
    ASSERT_EQ(hs[0].sub, 2, "lowest sub first");
    ASSERT_EQ(hs[1].sub, 9, "then the next");
}

int main() {
    test_registered_handler_fires_only_when_enabled();
    test_disable_and_release();
    test_errors();
    test_enabled_in_sub_order();
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
