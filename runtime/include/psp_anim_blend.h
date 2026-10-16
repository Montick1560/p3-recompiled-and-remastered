// psp_anim_blend.h — blend two evaluated poses word by word (smooth animation from two
// whole-frame samples). Pure, header-only; used by game modules.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

enum : uint8_t { kBlendKeep = 0, kBlendLerp = 1, kBlendQuat = 2 };

// In place into a: kBlendLerp words become a + (b - a) * t; a kBlendQuat word starts a 4-float
// quaternion blended by normalized lerp along the short arc (b's sign flipped when a.b < 0);
// kBlendKeep words (and the 3 words after a quaternion start) are left as a.
inline void psp_blend_pose(float* a, const float* b, const uint8_t* kinds, size_t n, float t) {
    for (size_t i = 0; i < n; i++) {
        if (kinds[i] == kBlendLerp) {
            a[i] += (b[i] - a[i]) * t;
        } else if (kinds[i] == kBlendQuat && i + 3 < n) {
            const float dot = a[i] * b[i] + a[i + 1] * b[i + 1] + a[i + 2] * b[i + 2] + a[i + 3] * b[i + 3];
            const float sign = dot < 0.0f ? -1.0f : 1.0f;
            float q[4];
            float len2 = 0.0f;
            for (int k = 0; k < 4; k++) {
                q[k] = a[i + k] + (sign * b[i + k] - a[i + k]) * t;
                len2 += q[k] * q[k];
            }
            if (len2 > 0.0f) {
                const float inv = 1.0f / std::sqrt(len2);
                for (int k = 0; k < 4; k++) a[i + k] = q[k] * inv;
            }
            i += 3;
        }
    }
}

// Whole frame to blend toward from `whole`: whole + 1 while it is within [.., end]; past the end
// a looping motion wraps to its start frame, a one-shot returns -1 (hold, no blend).
inline int32_t psp_anim_next_frame(int32_t whole, float start, float end, bool loop) {
    const int32_t next = whole + 1;
    if (static_cast<float>(next) <= end) return next;
    return loop ? static_cast<int32_t>(start) : -1;
}
