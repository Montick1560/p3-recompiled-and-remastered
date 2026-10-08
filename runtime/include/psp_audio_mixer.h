#pragma once
// sceAudio output mixer: one FIFO of stereo s16 frames per PSP audio channel
// (8 normal channels + the Output2/SRC channel), mixed with saturation into
// the host device buffer. Pure and thread-safe (game threads push, the audio
// device callback mixes). PSP volumes: 0x8000 = unity.

#include <cstdint>
#include <deque>
#include <mutex>

class PspAudioMixer {
public:
    static constexpr int kNormalChannels = 8;
    static constexpr int kOutput2 = kNormalChannels;  // sceAudioOutput2 / SRC
    static constexpr int kChannels = kNormalChannels + 1;

    /// Queue `frames` frames from `samples` (interleaved L/R when `stereo`,
    /// else one sample per frame copied to both sides), scaled by the PSP
    /// left/right volumes (0..0xFFFF, 0x8000 = unity).
    void push(int channel, const int16_t* samples, int frames, bool stereo,
              int vol_left, int vol_right);

    /// Frames still queued on `channel`.
    int queued(int channel) const;

    /// Pop up to `frames` frames from every channel and sum them into
    /// `out_stereo` (interleaved), clamped to s16. Missing frames are silence.
    void mix(int16_t* out_stereo, int frames);

    /// Drop everything queued on `channel` (channel release).
    void clear(int channel);

private:
    mutable std::mutex mutex_;
    std::deque<int32_t> queue_[kChannels];  // interleaved L/R, pre-scaled
};
