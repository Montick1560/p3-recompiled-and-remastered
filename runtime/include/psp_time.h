#pragma once
// Host timing helpers for HLE paths that must keep real-hardware rates.
// audio_block_for_samples (psp_hle_utility.cpp) paces guest audio threads at
// the 44.1 kHz playback rate; on Windows a plain sleep_for() lands on the
// ~15.6 ms system timer tick, so a 11.6 ms grain wait overshoots ~26% and the
// guest's own audio-driven clock runs slow (T0: movie cursor never reaches its
// 27.365 s threshold without an audio device).

#include <chrono>
#include <cstdint>
#include <mutex>

/// Playback duration of `samples` mono frames at 44.1 kHz, in microseconds.
int64_t psp_audio_pace_us(int64_t samples);

/// Sleep until `deadline`, landing sub-millisecond late. Uses a high-resolution
/// waitable timer on Windows (per-call, so concurrent callers don't interfere);
/// falls back to coarse sleep + yield-spin when unavailable.
void psp_sleep_until(std::chrono::steady_clock::time_point deadline);

/// Media-clock scheduler for one audio channel's no-device pacing. Each grain
/// advances the virtual drain clock by its playback duration, the way a real
/// device would drain while the producer does its per-grain work; waiting
/// "now + grain" instead adds that work on top and runs the guest's
/// audio-driven clocks slow (T0). Thread-safe (one clock per channel; several
/// guest threads may share a channel).
class AudioPaceClock {
public:
    /// Virtual time at which `samples` queued at `now` finish playing: the
    /// grain plays (clock advances) while the caller works on the next one, so
    /// per-grain decode/IO time is absorbed into the wait. A producer more
    /// than a grain behind (idle channel, overload) resyncs from `now`.
    std::chrono::steady_clock::time_point schedule(
        std::chrono::steady_clock::time_point now, int64_t samples);

private:
    std::mutex mutex_;
    std::chrono::steady_clock::time_point next_{};
};
