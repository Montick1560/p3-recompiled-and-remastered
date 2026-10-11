// psp_stack_layout: guest thread stacks must stay inside their own block.
// Regression: the k0 area and sp were placed at the block BASE, so every
// thread's stack ran down into the next block (user_main over FileThread
// and the GE callback stack), corrupting live frames.
#include "psp_stack_layout.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static void k0_and_sp_inside_block() {
    PspStackCarver c(0x0BFFF000U);
    PspStackBlock b = c.carve(0x24000U);
    CHECK(b.base == 0x0BFDB000U && b.size == 0x24000U, "block below the previous top");
    CHECK(b.k0 == 0x0BFFEF00U, "k0 area is the top 256 bytes of the block");
    CHECK(b.sp == b.k0, "sp starts at k0 (PPSSPP FillStack)");
    CHECK(b.k0 >= b.base && b.k0 + 0x100U <= b.base + b.size, "k0 area inside the block");
}

static void blocks_never_overlap() {
    PspStackCarver c(0x0BFFF000U);
    const uint32_t sizes[] = {147456, 4096, 8192, 16384, 49152, 4096, 100, 0x1234};
    std::vector<PspStackBlock> blocks;
    for (uint32_t s : sizes) blocks.push_back(c.carve(s));
    bool ok = true;
    for (size_t i = 0; i < blocks.size(); i++) {
        for (size_t j = i + 1; j < blocks.size(); j++) {
            const auto& a = blocks[i];
            const auto& b = blocks[j];
            if (a.base < b.base + b.size && b.base < a.base + a.size) ok = false;
        }
        // Everything a thread touches: [base, k0 + 0x100).
        if (blocks[i].k0 + 0x100U > blocks[i].base + blocks[i].size) ok = false;
    }
    CHECK(ok, "consecutive stacks are disjoint and self-contained");
}

static void sizes_rounded() {
    PspStackCarver c(0x0C000000U);
    PspStackBlock tiny = c.carve(16);
    CHECK(tiny.size == 0x200U, "tiny stacks get the k0 area plus room");
    PspStackBlock odd = c.carve(0x1234);
    CHECK(odd.size == 0x1300U && (odd.base & 0xFFU) == 0, "sizes rounded up to 256");
    CHECK(c.next() == odd.base, "next block starts below the last one");
}

int main() {
    k0_and_sp_inside_block();
    blocks_never_overlap();
    sizes_rounded();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("stack_layout: all passed\n");
    return 0;
}
