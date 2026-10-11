#pragma once
// Guest thread stacks, PSP layout (PPSSPP PSPThread::FillStack): a thread
// owns the block [base, base + size); the 256-byte k0 area is the top of
// the block and the stack pointer starts at k0, growing down towards base.
// Blocks are carved downward from the top of user memory and never overlap.
#include <cstdint>

struct PspStackBlock {
    uint32_t base;  // lowest address; ReferThreadStatus "stack", k0+0xC8
    uint32_t size;  // multiple of 256
    uint32_t k0;    // base + size - 0x100
    uint32_t sp;    // initial stack pointer (= k0)
};

class PspStackCarver {
public:
    explicit PspStackCarver(uint32_t top) : next_(top) {}

    PspStackBlock carve(uint32_t size) {
        size = (size + 0xFFU) & ~0xFFU;
        if (size < 0x200U) size = 0x200U;  // k0 area plus a minimal stack
        PspStackBlock b{};
        b.base = next_ - size;
        b.size = size;
        b.k0 = b.base + size - 0x100U;
        b.sp = b.k0;
        next_ = b.base;
        return b;
    }

    uint32_t next() const { return next_; }

private:
    uint32_t next_;
};
