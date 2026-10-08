#include "psp_audio_mixer.h"

#include <algorithm>
#include <cstring>

namespace {

int32_t scale(int32_t sample, int vol) {
    return static_cast<int32_t>((static_cast<int64_t>(sample) * vol) >> 15);
}

int16_t clamp16(int32_t v) {
    return static_cast<int16_t>(std::clamp(v, -32768, 32767));
}

bool valid(int channel) {
    return channel >= 0 && channel < PspAudioMixer::kChannels;
}

}  // namespace

void PspAudioMixer::push(int channel, const int16_t* samples, int frames, bool stereo,
                         int vol_left, int vol_right) {
    if (!valid(channel) || !samples || frames <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& q = queue_[channel];
    for (int i = 0; i < frames; i++) {
        const int32_t l = stereo ? samples[2 * i] : samples[i];
        const int32_t r = stereo ? samples[2 * i + 1] : samples[i];
        q.push_back(scale(l, vol_left));
        q.push_back(scale(r, vol_right));
    }
}

int PspAudioMixer::queued(int channel) const {
    if (!valid(channel)) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(queue_[channel].size() / 2);
}

void PspAudioMixer::mix(int16_t* out_stereo, int frames) {
    if (!out_stereo || frames <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        for (auto& q : queue_) {
            if (q.size() >= 2) {
                l += q.front(); q.pop_front();
                r += q.front(); q.pop_front();
            }
        }
        out_stereo[2 * i] = clamp16(l);
        out_stereo[2 * i + 1] = clamp16(r);
    }
}

void PspAudioMixer::clear(int channel) {
    if (!valid(channel)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_[channel].clear();
}
