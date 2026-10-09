// Unit tests for the GE block transfer (psp_ge_transfer.h): PPSSPP
// GPUCommon::DoBlockTransfer memory semantics on a fake guest RAM.

#include "psp_ge_transfer.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    std::vector<uint8_t> ram(0x08000000);  // masked 128 MB guest space
    uint8_t* rdram = ram.data();
    // Source: 4x2 pixels of 32 bpp at (1,1) in a 8-pixel-stride image at 0x08800000.
    const uint32_t src = 0x08800000, dst = 0x04100000;
    for (uint32_t i = 0; i < 8 * 4 * 4; i++) rdram[(src & 0x07FFFFFF) + i] = (uint8_t)i;
    GeTransferRegs r{};
    r.src = src & 0xFFFFF0;
    r.srcw = ((src >> 8) & 0xFF0000) | 8;   // high address byte | stride 8
    r.dst = dst & 0xFFFFF0;
    r.dstw = ((dst >> 8) & 0xFF0000) | 16;  // stride 16
    r.srcpos = (1 << 10) | 1;               // x=1, y=1
    r.dstpos = (2 << 10) | 3;               // x=3, y=2
    r.size = ((2 - 1) << 10) | (4 - 1);     // 4 wide, 2 high
    r.start = 1;                            // 32 bpp
    psp_ge_block_transfer(rdram, r);
    for (int y = 0; y < 2; y++) {
        const uint8_t* s = rdram + (src & 0x07FFFFFF) + ((1 + y) * 8 + 1) * 4;
        const uint8_t* d = rdram + (dst & 0x07FFFFFF) + ((2 + y) * 16 + 3) * 4;
        CHECK(std::memcmp(s, d, 4 * 4) == 0, "row copied to the destination rect");
    }
    CHECK(rdram[(dst & 0x07FFFFFF) + ((2 * 16) + 3) * 4 - 1] == 0, "pixel left of the rect untouched");
    CHECK(rdram[(dst & 0x07FFFFFF) + ((2 * 16) + 7) * 4] == 0, "pixel right of the rect untouched");

    // 16 bpp (start bit 0 clear) and the 0x04800000 VRAM mirror maps onto 0x04000000.
    std::memset(rdram + 0x04000000, 0, 64);
    for (int i = 0; i < 8; i++) rdram[0x00900000 + i] = (uint8_t)(0xA0 + i);
    GeTransferRegs h{};
    h.src = 0x900000; h.srcw = (0x08 << 16) | 8;
    h.dst = 0x800000; h.dstw = (0x04 << 16) | 8;   // 0x04800000 -> 0x04000000
    h.size = 4 - 1;                                // 4 wide, 1 high
    h.start = 0;
    psp_ge_block_transfer(rdram, h);
    CHECK(std::memcmp(rdram + 0x04000000, rdram + 0x00900000, 8) == 0, "16 bpp copy into the VRAM mirror base");

    if (failures) { std::fprintf(stderr, "SOME TESTS FAILED\n"); return 1; }
    std::printf("ge_transfer: all tests passed\n");
    return 0;
}
