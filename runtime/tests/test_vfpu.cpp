#include "psp_vfpu.h"
#include "recomp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_EXACT(actual, expected, msg) \
    do { \
        tests_run++; \
        if ((actual) != (expected)) { \
            std::fprintf(stderr, \
                "FAIL: %s: got %f, expected %f\n", \
                msg, \
                static_cast<double>(actual), \
                static_cast<double>(expected)); \
            failures++; \
        } \
    } while (0)

#define ASSERT_APPROX(actual, expected, eps, msg) \
    do { \
        tests_run++; \
        if (std::fabsf((actual) - (expected)) > (eps)) { \
            std::fprintf(stderr, \
                "FAIL: %s: got %f, expected %f (eps=%e)\n", \
                msg, \
                static_cast<double>(actual), \
                static_cast<double>(expected), \
                static_cast<double>(eps)); \
            failures++; \
        } \
    } while (0)

#define ASSERT_INT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        if ((actual) != (expected)) { \
            std::fprintf(stderr, \
                "FAIL: %s: got 0x%X, expected 0x%X\n", \
                msg, \
                static_cast<unsigned>(actual), \
                static_cast<unsigned>(expected)); \
            failures++; \
        } \
    } while (0)

/// Initialize a clean recomp_context with identity prefixes.
static void init_ctx(recomp_context& ctx) {
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.vfpu_ctrl[VFPU_CTRL_SPREFIX] = 0xE4u;
    ctx.vfpu_ctrl[VFPU_CTRL_TPREFIX] = 0xE4u;
    ctx.vfpu_ctrl[VFPU_CTRL_DPREFIX] = 0x00u;
}

// Helpers available for future tests if needed:
// set_vfpu_single(ctx, flat_idx, val) - write single float
// single_idx(reg) - compute flat index from 7-bit encoding

// ===================================================================
// Test 1: vfpu_sin cardinal values (bit-exact)
// ===================================================================
static void test_sin_cardinals() {
    std::printf("  test_sin_cardinals...\n");
    recomp_context ctx;

    // sin(0.0) = 0.0
    init_ctx(ctx);
    ctx.vfpu[0] = 0.0f;  // S000
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);  // S001 = sin(S000)
    ASSERT_EXACT(ctx.vfpu[1], 0.0f, "sin(0.0)");

    // sin(1.0) = 1.0
    init_ctx(ctx);
    ctx.vfpu[0] = 1.0f;
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[1], 1.0f, "sin(1.0)");

    // sin(2.0) = 0.0
    init_ctx(ctx);
    ctx.vfpu[0] = 2.0f;
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[1], 0.0f, "sin(2.0)");

    // sin(3.0) = -1.0
    init_ctx(ctx);
    ctx.vfpu[0] = 3.0f;
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[1], -1.0f, "sin(3.0)");
}

// ===================================================================
// Test 2: vfpu_cos cardinal values (bit-exact)
// ===================================================================
static void test_cos_cardinals() {
    std::printf("  test_cos_cardinals...\n");
    recomp_context ctx;

    // cos(0.0) = 1.0
    init_ctx(ctx);
    ctx.vfpu[0] = 0.0f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[1], 1.0f, "cos(0.0)");

    // cos(1.0) = 0.0
    init_ctx(ctx);
    ctx.vfpu[0] = 1.0f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[1], 0.0f, "cos(1.0)");

    // cos(2.0) = -1.0
    init_ctx(ctx);
    ctx.vfpu[0] = 2.0f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[1], -1.0f, "cos(2.0)");

    // cos(3.0) = 0.0
    init_ctx(ctx);
    ctx.vfpu[0] = 3.0f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[1], 0.0f, "cos(3.0)");
}

// ===================================================================
// Test 3: eat_prefixes resets correctly
// ===================================================================
static void test_eat_prefixes() {
    std::printf("  test_eat_prefixes...\n");
    recomp_context ctx;
    init_ctx(ctx);

    ctx.vfpu_ctrl[VFPU_CTRL_SPREFIX] = 0xDEADBEEFu;
    ctx.vfpu_ctrl[VFPU_CTRL_TPREFIX] = 0xCAFEBABEu;
    ctx.vfpu_ctrl[VFPU_CTRL_DPREFIX] = 0x12345678u;

    vfpu_eat_prefixes(&ctx);

    ASSERT_INT_EQ(ctx.vfpu_ctrl[VFPU_CTRL_SPREFIX], 0xE4u,
                  "SPREFIX after eat");
    ASSERT_INT_EQ(ctx.vfpu_ctrl[VFPU_CTRL_TPREFIX], 0xE4u,
                  "TPREFIX after eat");
    ASSERT_INT_EQ(ctx.vfpu_ctrl[VFPU_CTRL_DPREFIX], 0x00u,
                  "DPREFIX after eat");
}

// ===================================================================
// Test 4: Prefix swizzle on a pair vector
// ===================================================================
static void test_prefix_swizzle() {
    std::printf("  test_prefix_swizzle...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Load a pair {3.0, 7.0} into S000 and S010
    // Pair register P000 = S000, S010 (col 0, col 1 in row 0)
    // 7-bit encoding for pair P000: mtx=0, col=0, row=0,
    //   transpose=0 -> reg = 0x00
    ctx.vfpu[0] = 3.0f;  // S000: mtx0, row0, col0
    ctx.vfpu[1] = 7.0f;  // S010: mtx0, row0, col1

    // Set SPREFIX to swizzle Y,X (swap elements):
    // swizzle bits: element 0 reads from index 1 (=0b01),
    //               element 1 reads from index 0 (=0b00)
    // bits 1:0 = 01 (element 0 <- index 1)
    // bits 3:2 = 00 (element 1 <- index 0)
    // Result: 0x01 for swizzle, rest 0
    ctx.vfpu_ctrl[VFPU_CTRL_SPREFIX] = 0x01u;

    // vmov pair: vd=0x04 (P010), vs=0x00 (P000), size=2
    // vd P010: mtx=1, col=0, row=0 -> flat: 1*16+0*4+0 = 16,17
    // Actually let's use simpler encoding:
    // vd = 0x04 -> mtx = (4>>2)&7 = 1, col = 4&3 = 0
    // For pair: row = (4>>5)&2 = 0, transpose = (4>>5)&1 = 0
    // Elements at mtx=1, row=0, col=0 and col=1 -> flat 16, 17
    vfpu_vmov(&ctx, nullptr, 0x04, 0x00, 2);

    ASSERT_EXACT(ctx.vfpu[16], 7.0f, "swizzle pair[0]");
    ASSERT_EXACT(ctx.vfpu[17], 3.0f, "swizzle pair[1]");
}

// ===================================================================
// Test 5: vmmul 2x2 identity produces original matrix
// ===================================================================
static void test_vmmul_identity() {
    std::printf("  test_vmmul_identity...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Set up 2x2 identity in matrix 0 (M000)
    // M000 encoding for pair matrix: reg = 0x00
    // Elements: mtx=0
    //   row0: (0,0)=1, (0,1)=0
    //   row1: (1,0)=0, (1,1)=1
    ctx.vfpu[0]  = 1.0f;  // mtx0, row0, col0
    ctx.vfpu[1]  = 0.0f;  // mtx0, row0, col1
    ctx.vfpu[4]  = 0.0f;  // mtx0, row1, col0
    ctx.vfpu[5]  = 1.0f;  // mtx0, row1, col1

    // Set up arbitrary 2x2 in matrix 1 (M100)
    // M100: mtx=1 -> base at vfpu[16]
    ctx.vfpu[16] = 1.0f;  // mtx1, row0, col0
    ctx.vfpu[17] = 2.0f;  // mtx1, row0, col1
    ctx.vfpu[20] = 3.0f;  // mtx1, row1, col0
    ctx.vfpu[21] = 4.0f;  // mtx1, row1, col1

    // vmmul M200, M000, M100 -> result in matrix 2
    // M000 = 0x00, M100 = 0x04, M200 = 0x08
    // vmmul(ctx, rdram, vd=0x08, vs=0x00, vt=0x04, size=2)
    vfpu_vmmul(&ctx, nullptr, 0x08, 0x00, 0x04, 2);

    // Result should be the identity * arbitrary = arbitrary
    ASSERT_APPROX(ctx.vfpu[32], 1.0f, 1e-5f,
                  "mmul identity [0][0]");
    ASSERT_APPROX(ctx.vfpu[33], 2.0f, 1e-5f,
                  "mmul identity [0][1]");
    ASSERT_APPROX(ctx.vfpu[36], 3.0f, 1e-5f,
                  "mmul identity [1][0]");
    ASSERT_APPROX(ctx.vfpu[37], 4.0f, 1e-5f,
                  "mmul identity [1][1]");
}

// ===================================================================
// Test 6: Non-cardinal trig accuracy (epsilon)
// ===================================================================
static void test_trig_noncardinal() {
    std::printf("  test_trig_noncardinal...\n");
    recomp_context ctx;

    // sin(0.5) should be sin(pi/4) = sqrt(2)/2
    init_ctx(ctx);
    ctx.vfpu[0] = 0.5f;
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);
    float expected_sin = std::sinf(0.5f
        * static_cast<float>(M_PI_2));
    ASSERT_APPROX(ctx.vfpu[1], expected_sin, 1e-5f,
                  "sin(0.5)");

    // cos(0.5) should be cos(pi/4) = sqrt(2)/2
    init_ctx(ctx);
    ctx.vfpu[0] = 0.5f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    float expected_cos = std::cosf(0.5f
        * static_cast<float>(M_PI_2));
    ASSERT_APPROX(ctx.vfpu[1], expected_cos, 1e-5f,
                  "cos(0.5)");
}

// ===================================================================
// Test 7: vadd quad adds element-wise
// ===================================================================
static void test_vadd_quad() {
    std::printf("  test_vadd_quad...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // C000 (quad, mtx=0, col=0, row=0, no transpose)
    ctx.vfpu[0] = 1.0f;
    ctx.vfpu[1] = 2.0f;
    ctx.vfpu[2] = 3.0f;
    ctx.vfpu[3] = 4.0f;

    // C100 (quad, mtx=1, col=0)
    ctx.vfpu[16] = 10.0f;
    ctx.vfpu[17] = 20.0f;
    ctx.vfpu[18] = 30.0f;
    ctx.vfpu[19] = 40.0f;

    // vadd C200, C000, C100 (size=4)
    // C000 = 0x00, C100 = 0x04, C200 = 0x08
    vfpu_vadd(&ctx, nullptr, 0x08, 0x00, 0x04, 4);

    ASSERT_EXACT(ctx.vfpu[32], 11.0f, "vadd quad[0]");
    ASSERT_EXACT(ctx.vfpu[33], 22.0f, "vadd quad[1]");
    ASSERT_EXACT(ctx.vfpu[34], 33.0f, "vadd quad[2]");
    ASSERT_EXACT(ctx.vfpu[35], 44.0f, "vadd quad[3]");
}

// ===================================================================
// Test 8: vdot dot product
// ===================================================================
static void test_vdot() {
    std::printf("  test_vdot...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Vector {1, 2, 3} in C000 (triple)
    ctx.vfpu[0] = 1.0f;
    ctx.vfpu[1] = 2.0f;
    ctx.vfpu[2] = 3.0f;

    // Vector {4, 5, 6} in C100 (triple)
    ctx.vfpu[16] = 4.0f;
    ctx.vfpu[17] = 5.0f;
    ctx.vfpu[18] = 6.0f;

    // vdot S200, C000, C100 (size=3)
    vfpu_vdot(&ctx, nullptr, 0x08, 0x00, 0x04, 3);

    // dot = 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    ASSERT_EXACT(ctx.vfpu[32], 32.0f, "vdot triple");
}

// ===================================================================
// Test 9: vcmp EQ mode
// ===================================================================
static void test_vcmp_eq() {
    std::printf("  test_vcmp_eq...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Pair {1, 2} vs {1, 3}
    ctx.vfpu[0] = 1.0f;
    ctx.vfpu[1] = 2.0f;
    ctx.vfpu[16] = 1.0f;
    ctx.vfpu[17] = 3.0f;

    // vcmp EQ, P000, P100 (cond=1, size=2)
    vfpu_vcmp(&ctx, nullptr, 0x00, 0x04, 1, 2);

    // Element 0: 1==1 -> true (bit 0 set)
    // Element 1: 2==3 -> false (bit 1 not set)
    // OR (bit 4) = true, AND (bit 5) = false
    uint32_t cc = ctx.vfpu_ctrl[VFPU_CTRL_CC];
    ASSERT_INT_EQ(cc & 0x3F, 0x11u,
                  "vcmp EQ pair: bits 0,4 set");
}

// ===================================================================
// main
// ===================================================================

int main() {
    std::printf("Running VFPU runtime tests...\n\n");

    test_sin_cardinals();
    test_cos_cardinals();
    test_eat_prefixes();
    test_prefix_swizzle();
    test_vmmul_identity();
    test_trig_noncardinal();
    test_vadd_quad();
    test_vdot();
    test_vcmp_eq();

    std::printf("\n%d tests run, %d failures\n",
                tests_run, failures);

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        std::printf("SOME TESTS FAILED\n");
        return 1;
    }
}
