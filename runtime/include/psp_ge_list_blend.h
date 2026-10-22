#pragma once
// Blends two recorded GE command lists of the same shape (smooth animation at 60 fps). Some
// games pre-record one display list per animation frame and call the whole frame's list; the
// lists of two neighbouring frames differ only in their matrix and material words. Into `out`:
// world/bone matrix data (0x3B / 0x2B, float24) lerped by t, material alpha (0x58, low byte)
// lerped and rounded, every other word copied from a. Texture offset/scale words (0x48..0x4B)
// stay a's on purpose: models flip through atlas cells with them, and a blended offset draws
// half of one cell and half of the next. Pure.
//
// Returns the words written, up to and including the list's RET, or 0 (nothing usable) when
// the two lists differ in any opcode, no RET comes within max_words, or a word changes control
// flow inside the list (JUMP, BJUMP, ORIGIN, END, FINISH): the copy runs at another address.
#include <cstddef>
#include <cstdint>
#include <cstring>

inline float psp_ge_f24(uint32_t word) {
    const uint32_t bits = (word & 0x00FFFFFFu) << 8;
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

inline uint32_t psp_ge_to_f24(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return bits >> 8;
}

inline size_t psp_ge_blend_lists(const uint32_t* a, const uint32_t* b, size_t max_words, float t, uint32_t* out) {
    for (size_t i = 0; i < max_words; i++) {
        const uint32_t op = a[i] >> 24;
        if (op != b[i] >> 24) return 0;
        if (op == 0x08 || op == 0x09 || op == 0x0C || op == 0x0F || op == 0x14) return 0;
        uint32_t w = a[i];
        if (a[i] != b[i]) {
            if (op == 0x3B || op == 0x2B) {
                const float fa = psp_ge_f24(a[i]), fb = psp_ge_f24(b[i]);
                w = op << 24 | psp_ge_to_f24(fa + (fb - fa) * t);
            } else if (op == 0x58) {
                const float aa = static_cast<float>(a[i] & 0xFFu), ab = static_cast<float>(b[i] & 0xFFu);
                const uint32_t alpha = static_cast<uint32_t>(aa + (ab - aa) * t + 0.5f) & 0xFFu;
                w = (a[i] & 0xFFFFFF00u) | alpha;
            }
        }
        out[i] = w;
        if (op == 0x0B) return i + 1;  // RET
    }
    return 0;
}
