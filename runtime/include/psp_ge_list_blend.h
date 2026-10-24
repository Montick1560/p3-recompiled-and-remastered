#pragma once
// Blends two recorded GE command lists of the same shape (smooth animation at 60 fps). Some
// games pre-record one display list per animation frame and call the whole frame's list; the
// lists of two neighbouring frames differ only in their matrix and material words. Into `out`:
// world/bone matrix data (0x3B / 0x2B, float24) lerped by t, material alpha (0x58, low byte)
// lerped and rounded, every other word copied from a. Texture offset/scale words (0x48..0x4B)
// stay a's on purpose: models flip through atlas cells with them, and a blended offset draws
// half of one cell and half of the next. In each whole matrix (12 data words after its 0x3A /
// 0x2A number) the three axes keep their lerped length: a plain lerp of a turning model draws it
// smaller halfway (72 degrees a key: 81 %). An axis that turns past 120 degrees between the two
// frames (the Wooyari Hero Mode fire wheel flips 180 degrees a key) has no blend: it would lerp
// through zero, so the list is refused. Pure.
//
// Returns the words written, up to and including the list's RET, or 0 (nothing usable) when
// the two lists differ in any opcode, no RET comes within max_words, a word changes control
// flow inside the list (JUMP, BJUMP, ORIGIN, END, FINISH: the copy runs at another address), or
// a matrix axis turns past 120 degrees.
#include <cmath>
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

// One whole matrix blended into out (its 12 data words at pos): each axis gets the lerped
// direction at the lerped length. False when an axis turns past 120 degrees.
inline bool psp_ge_blend_axes(const uint32_t* a, const uint32_t* b, const size_t* pos, float t, uint32_t* out) {
    for (int c = 0; c < 3; c++) {
        float va[3], vb[3], vm[3], la = 0.0f, lb = 0.0f, lm = 0.0f, dot = 0.0f;
        for (int k = 0; k < 3; k++) {
            va[k] = psp_ge_f24(a[pos[c * 3 + k]]);
            vb[k] = psp_ge_f24(b[pos[c * 3 + k]]);
            vm[k] = va[k] + (vb[k] - va[k]) * t;
            la += va[k] * va[k];
            lb += vb[k] * vb[k];
            lm += vm[k] * vm[k];
            dot += va[k] * vb[k];
        }
        la = std::sqrt(la);
        lb = std::sqrt(lb);
        lm = std::sqrt(lm);
        if (dot < -0.5f * la * lb) return false;
        const float want = la + (lb - la) * t;
        if (lm <= 0.0f) continue;
        for (int k = 0; k < 3; k++) {
            out[pos[c * 3 + k]] = (a[pos[c * 3 + k]] & 0xFF000000u) | psp_ge_to_f24(vm[k] * want / lm);
        }
    }
    return true;
}

inline size_t psp_ge_blend_lists(const uint32_t* a, const uint32_t* b, size_t max_words, float t, uint32_t* out) {
    // Data words of the world (0) and bone (1) matrix being written; -1 before its number.
    int got[2] = {-1, -1};
    size_t pos[2][12];
    for (size_t i = 0; i < max_words; i++) {
        const uint32_t op = a[i] >> 24;
        if (op != b[i] >> 24) return 0;
        if (op == 0x08 || op == 0x09 || op == 0x0C || op == 0x0F || op == 0x14) return 0;
        if (op == 0x3A || op == 0x2A) got[op == 0x2A] = 0;
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
        if ((op == 0x3B || op == 0x2B) && got[op == 0x2B] >= 0) {
            const int m = op == 0x2B;
            pos[m][got[m]++] = i;
            if (got[m] == 12) {
                if (!psp_ge_blend_axes(a, b, pos[m], t, out)) return 0;
                got[m] = 0;  // bone matrices follow one another after one number
            }
        }
        if (op == 0x0B) return i + 1;  // RET
    }
    return 0;
}
