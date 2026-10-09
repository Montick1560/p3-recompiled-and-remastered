#pragma once
// sceAudio channel-volume resolution (pure, header-only). PSP volumes:
// 0x8000 = unity. A channel owns a live volume pair; an output call with a
// negative volume keeps it ("leave that side alone"), a non-negative one
// replaces it. Mirrors PPSSPP __AudioEnqueue / sceAudioChangeChannelVolume.

#include <cstdint>

namespace psp_audio_volume {

constexpr int kMaxChannelVolume = 0xFFFF;   // normal channels
constexpr int kMaxOutput2Volume = 0xFFFFF;  // Output2/SRC is 20-bit

struct ChannelVolume {
    int left = 0;   // sceAudioChReserve zeroes the live volume
    int right = 0;
};

/// Apply an output call's volumes to `cv` (negative = keep that side).
/// Returns false and leaves `cv` untouched when a volume exceeds `max_vol`.
inline bool resolve(ChannelVolume& cv, int left, int right,
                    int max_vol = kMaxChannelVolume) {
    if (left > max_vol || right > max_vol) return false;
    if (left >= 0) cv.left = left;
    if (right >= 0) cv.right = right;
    return true;
}

/// sceAudioOutputPannedBlocking ORs the volumes before range-checking, so a
/// negative volume is invalid there rather than "keep".
inline bool resolve_panned(ChannelVolume& cv, int left, int right) {
    if (static_cast<uint32_t>(left | right) > kMaxChannelVolume) return false;
    return resolve(cv, left, right);
}

/// sceAudioOutput2OutputBlocking: the volume is unsigned 20-bit and is not
/// stored. Returns false when out of range.
inline bool output2_volume(int vol) {
    return vol >= 0 && vol <= kMaxOutput2Volume;
}

}  // namespace psp_audio_volume
