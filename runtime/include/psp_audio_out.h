#pragma once
// Host audio output for sceAudio: an SDL2 device (44.1 kHz stereo s16) whose
// callback drains the PspAudioMixer. When no device can be opened (or
// PSPRECOMP_NO_AUDIO is set) the runtime falls back to timing-only blocking.
// PSPRECOMP_AUDIO_DUMP=<file.wav> also writes the mixed output to a WAV file.

#include "psp_audio_mixer.h"

/// Open the audio device. Returns false when audio is unavailable.
bool psp_audio_out_init();
void psp_audio_out_shutdown();

/// True when a device is open and draining the mixer.
bool psp_audio_out_active();

PspAudioMixer& psp_audio_mixer();
