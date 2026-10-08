// Unit tests for the volatile-memory lock protocol
// (hle/psp_hle_volatile_mem.h). Oracle: PPSSPP Core/HLE/scePower.cpp
// KernelVolatileMemLock / KernelVolatileMemUnlock.
//
// Standalone executable (test_refer_info convention): no SDL/GL/scheduler
// deps — the header under test is pure protocol logic over guest memory.

#include "hle/psp_hle_volatile_mem.h"

#include <cstdio>
#include <vector>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_U32_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        if (static_cast<uint32_t>(actual) != \
            static_cast<uint32_t>(expected)) { \
            std::fprintf(stderr, "FAIL: %s: got 0x%08X, expected 0x%08X\n", \
                msg, static_cast<uint32_t>(actual), \
                static_cast<uint32_t>(expected)); \
            failures++; \
        } \
    } while (0)

// Guest out-pointers (>= 0x10000 so the NULL-page guard does not drop them).
static constexpr uint32_t PADDR = 0x00020000U;
static constexpr uint32_t PSIZE = 0x00020004U;

static void test_lock_writes_region_and_marks_locked(uint8_t* rdram) {
    PspVolatileMem vm;
    ASSERT_U32_EQ(psp_volatile_mem_lock(vm, rdram, 0, PADDR, PSIZE), 0,
                  "first lock succeeds");
    ASSERT_U32_EQ(psp_mem_read<uint32_t>(rdram, PADDR), 0x08400000U,
                  "lock writes the volatile base address");
    ASSERT_U32_EQ(psp_mem_read<uint32_t>(rdram, PSIZE), 0x00400000U,
                  "lock writes the 4 MB size");
    ASSERT_U32_EQ(vm.locked, 1, "state is locked");
}

static void test_second_lock_is_in_use(uint8_t* rdram) {
    PspVolatileMem vm;
    psp_volatile_mem_lock(vm, rdram, 0, PADDR, PSIZE);
    ASSERT_U32_EQ(psp_volatile_mem_lock(vm, rdram, 0, PADDR, PSIZE),
                  PSP_VMEM_ERROR_IN_USE, "second lock reports in-use");
}

static void test_bad_type_is_invalid_mode(uint8_t* rdram) {
    PspVolatileMem vm;
    ASSERT_U32_EQ(psp_volatile_mem_lock(vm, rdram, 1, PADDR, PSIZE),
                  PSP_VMEM_ERROR_INVALID_MODE, "type != 0 lock is invalid mode");
    ASSERT_U32_EQ(vm.locked, 0, "invalid lock leaves state unlocked");
    ASSERT_U32_EQ(psp_volatile_mem_unlock(vm, 1),
                  PSP_VMEM_ERROR_INVALID_MODE, "type != 0 unlock is invalid mode");
}

static void test_unlock_cycle(uint8_t* rdram) {
    PspVolatileMem vm;
    ASSERT_U32_EQ(psp_volatile_mem_unlock(vm, 0), PSP_VMEM_ERROR_NOT_LOCKED,
                  "unlock while unlocked fails");
    psp_volatile_mem_lock(vm, rdram, 0, PADDR, PSIZE);
    ASSERT_U32_EQ(psp_volatile_mem_unlock(vm, 0), 0, "unlock after lock succeeds");
    ASSERT_U32_EQ(psp_volatile_mem_lock(vm, rdram, 0, PADDR, PSIZE), 0,
                  "lock again after unlock succeeds");
}

static void test_null_out_pointers_are_skipped(uint8_t* rdram) {
    PspVolatileMem vm;
    ASSERT_U32_EQ(psp_volatile_mem_lock(vm, rdram, 0, 0, 0), 0,
                  "lock with NULL out-pointers still succeeds");
}

int main() {
    std::vector<uint8_t> rdram(0x00100000, 0);
    test_lock_writes_region_and_marks_locked(rdram.data());
    test_second_lock_is_in_use(rdram.data());
    test_bad_type_is_invalid_mode(rdram.data());
    test_unlock_cycle(rdram.data());
    test_null_out_pointers_are_skipped(rdram.data());
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
