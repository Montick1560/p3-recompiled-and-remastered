// Ported from PPSSPP (GPL-2.0-or-later), Core/HW/SasAudio.h and
// Core/HW/SasReverb.h. Copyright (c) 2012- PPSSPP Project.
//
// SAS (Software Audio Synthesis) mixing core: VAG (PS-ADPCM) decode, ADSR
// envelopes, pitch resampling, PCM voices, dry/wet mix and the PSX-style
// reverb. Pure C++: no PPSSPP framework, no savestates, no logging. Guest
// memory is reached only through GuestMem, which masks addresses with
// 0x07FFFFFF and bounds-checks every range, so a bad guest pointer yields
// silence instead of a host crash.
//
// Deviations from PPSSPP (all deliberate):
//  - ATRAC3 voices are silent stubs (the ATRAC decoder is a separate task).
//  - Noise voices produce silence, exactly as in PPSSPP (it never generates
//    noise either).
//  - A VAG/PCM voice whose data lies outside guest memory ends immediately
//    (PPSSPP leaves the output buffer uninitialised and never ends it).
//  - The reverb return volume is the fixed unity multiplier (PPSSPP scales
//    it by a user config setting).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp_sas {

enum {
    PSP_SAS_VOICES_MAX = 32,
    PSP_SAS_VOL_MAX = 0x1000,
    PSP_SAS_MAX_GRAIN = 2048,   // matches the max grain of __sceSasInit
    PSP_SAS_PITCH_MIN = 0x0000,
    PSP_SAS_PITCH_BASE = 0x1000,
    PSP_SAS_PITCH_MASK = 0xFFF,
    PSP_SAS_PITCH_BASE_SHIFT = 12,
    PSP_SAS_PITCH_MAX = 0x4000,
    PSP_SAS_ENVELOPE_HEIGHT_MAX = 0x40000000,
};

enum SasADSRCurveMode : int {
    PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE = 0,
    PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE = 1,
    PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT = 2,
    PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE = 3,
    PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE = 4,
    PSP_SAS_ADSR_CURVE_MODE_DIRECT = 5,
};

enum SasEffectType {
    PSP_SAS_EFFECT_TYPE_OFF = -1,
    PSP_SAS_EFFECT_TYPE_ROOM = 0,
    PSP_SAS_EFFECT_TYPE_STUDIO_SMALL = 1,
    PSP_SAS_EFFECT_TYPE_STUDIO_MEDIUM = 2,
    PSP_SAS_EFFECT_TYPE_STUDIO_LARGE = 3,
    PSP_SAS_EFFECT_TYPE_HALL = 4,
    PSP_SAS_EFFECT_TYPE_SPACE = 5,
    PSP_SAS_EFFECT_TYPE_ECHO = 6,
    PSP_SAS_EFFECT_TYPE_DELAY = 7,
    PSP_SAS_EFFECT_TYPE_PIPE = 8,
    PSP_SAS_EFFECT_TYPE_MAX = 8,
};

enum SasOutputMode {
    PSP_SAS_OUTPUTMODE_MIXED = 0,
    PSP_SAS_OUTPUTMODE_RAW = 1,
};

enum VoiceType {
    VOICETYPE_OFF,
    VOICETYPE_VAG,  // default
    VOICETYPE_NOISE,
    VOICETYPE_TRIWAVE,
    VOICETYPE_PULSEWAVE,
    VOICETYPE_PCM,
    VOICETYPE_ATRAC3,
};

/// Bounds-checked view of guest RAM. `base` is the start of the 128MB rdram
/// mapping; addresses are masked with 0x07FFFFFF like the emitted code does.
struct GuestMem {
    uint8_t* base = nullptr;
    size_t size = 0;  // bytes available at base (0x08000000 for the real rdram)

    /// Pointer to [addr, addr+len) or nullptr when it leaves the mapping.
    uint8_t* range(uint32_t addr, size_t len) const {
        if (base == nullptr) return nullptr;
        size_t off = addr & 0x07FFFFFFu;
        if (off > size || len > size - off) return nullptr;
        return base + off;
    }
};

struct WaveformEffect {
    int type = PSP_SAS_EFFECT_TYPE_OFF;
    int delay = 0;
    int feedback = 0;
    int leftVol = 0;
    int rightVol = 0;
    int isDryOn = 1;
    int isWetOn = 0;
};

// VAG is Sony's ADPCM format: 28 16-bit samples per 16-byte block.
class VagDecoder {
public:
    void Start(uint32_t dataPtr, uint32_t vagSize, bool loopEnabled);
    void GetSamples(const GuestMem& mem, int16_t* outSamples, int numSamples);
    void DecodeBlock(const uint8_t*& readp);
    bool End() const { return end_; }
    uint32_t GetReadPtr() const { return read_; }

private:
    int16_t samples[28]{};
    int curSample = 0;

    uint32_t data_ = 0;
    uint32_t read_ = 0;
    int curBlock_ = -1;
    int loopStartBlock_ = -1;
    int numBlocks_ = 0;

    int s_1 = 0;
    int s_2 = 0;

    bool loopEnabled_ = false;
    bool loopAtNextBlock_ = false;
    bool end_ = true;
};

class ADSREnvelope {
public:
    // Actual PSP state values (STATE_KEYON_STEP is PPSSPP's hack state).
    enum ADSRState {
        STATE_KEYON_STEP = -42,
        STATE_KEYON = -2,
        STATE_OFF = -1,
        STATE_ATTACK = 0,
        STATE_DECAY = 1,
        STATE_SUSTAIN = 2,
        STATE_RELEASE = 3,
    };

    void SetSimpleEnvelope(uint32_t ADSREnv1, uint32_t ADSREnv2);
    void SetEnvelope(int flag, int a, int d, int s, int r);
    void SetRate(int flag, int a, int d, int s, int r);
    void SetSustainLevel(int sl) { sustainLevel = sl; }

    void WalkCurve(int type, int rate);

    void KeyOn();
    void KeyOff();
    void End();
    void Step();

    int GetHeight() const {
        return (int)(height_ > (int64_t)PSP_SAS_ENVELOPE_HEIGHT_MAX
                         ? (int64_t)PSP_SAS_ENVELOPE_HEIGHT_MAX : height_);
    }
    ADSRState GetState() const { return state_; }
    bool NeedsKeyOn() const { return state_ == STATE_KEYON; }
    bool HasEnded() const { return state_ == STATE_OFF; }

    int attackRate = 0;
    int decayRate = 0;
    int sustainRate = 0;
    int sustainLevel = 0;
    int releaseRate = 0;

    SasADSRCurveMode attackType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE;
    SasADSRCurveMode decayType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
    SasADSRCurveMode sustainType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
    SasADSRCurveMode releaseType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;

private:
    void SetState(ADSRState state);

    ADSRState state_ = STATE_OFF;
    int64_t height_ = 0;  // s64 so rate arithmetic never overflows
};

struct SasVoice {
    void KeyOn();
    void KeyOff();

    /// __sceSasSetVoice semantics: restart the decoder only if the sample
    /// source changed (so re-issuing the same SetVoice does not rewind).
    void SetVag(uint32_t addr, int size, bool loopOn);
    /// __sceSasSetVoicePCM semantics.
    void SetPcm(uint32_t addr, int size, int loopPos);

    void ReadSamples(const GuestMem& mem, int16_t* output, int numSamples);
    bool HaveSamplesEnded() const;

    bool playing = false;
    bool paused = false;  // a voice can be playing AND paused: it won't mix
    bool on = false;      // key-on / key-off

    VoiceType type = VOICETYPE_OFF;

    uint32_t vagAddr = 0;
    uint32_t vagSize = 0;
    uint32_t pcmAddr = 0;
    int pcmSize = 0;
    int pcmIndex = 0;
    int pcmLoopPos = 0;
    int sampleRate = 44100;

    uint32_t sampleFrac = 0;
    int pitch = PSP_SAS_PITCH_BASE;
    bool loop = false;

    int noiseFreq = 0;

    int volumeLeft = PSP_SAS_VOL_MAX;
    int volumeRight = PSP_SAS_VOL_MAX;
    // Volume "sent" to the effects engine (reverb).
    int effectLeft = PSP_SAS_VOL_MAX;
    int effectRight = PSP_SAS_VOL_MAX;
    int16_t resampleHist[2] = {0, 0};

    ADSREnvelope envelope;
    VagDecoder vag;
};

struct SasReverbData;

/// The PSP's SAS reverb (PSX SPU reverb formula with PSP presets).
class SasReverb {
public:
    SasReverb();
    ~SasReverb();
    SasReverb(const SasReverb&) = delete;
    SasReverb& operator=(const SasReverb&) = delete;

    void SetPreset(int preset);
    // __sceSasRevParam's delay and feedback (0..127); only Echo/Delay use them.
    void SetParams(int delay, int feedback);
    int GetPreset() const { return preset_; }
    static const char* GetPresetName(int preset);

    // Input: mixdown of the reverb-enabled sends at 22kHz. Output: 44kHz.
    void ProcessReverb(int16_t* output, const int16_t* input,
                       size_t inputSize, int volLeft, int volRight);

private:
    enum { BUFSIZE = 0x20000 };
    void ApplyParams();

    std::vector<int16_t> workspace_;
    SasReverbData* data_;
    int preset_ = -1;
    int pos_ = 0;
    int delay_ = 0;
    int feedback_ = 0;
};

class SasInstance {
public:
    SasInstance();
    ~SasInstance();
    SasInstance(const SasInstance&) = delete;
    SasInstance& operator=(const SasInstance&) = delete;

    /// Point the mixer at guest RAM. Must be set before Mix(); a null base
    /// makes every voice silent.
    void SetMemory(uint8_t* rdram, size_t size = 0x08000000u) {
        mem_.base = rdram;
        mem_.size = size;
    }
    const GuestMem& memory() const { return mem_; }

    void SetGrainSize(int newGrainSize);
    int GetGrainSize() const { return grainSize_; }

    /// Mixes one grain. Output goes to outAddr (grain stereo s16 frames in
    /// mixed mode; four grain-long planes L,R,sendL,sendR in raw mode). If
    /// inAddr != 0 its stereo frames are scaled by leftVol/rightVol
    /// (0x1000 = unity) and added (CoreWithMix; in == out is allowed).
    /// Voices advance even when the output address is bad. Returns false if
    /// the output range was not valid guest memory.
    bool Mix(uint32_t outAddr, uint32_t inAddr, int leftVol, int rightVol,
             bool mute = false);

    uint32_t GetEndFlag() const;
    uint32_t GetPauseFlag() const;

    void SetWaveformEffectType(int type);
    void SetWaveformEffectParams(int delay, int feedback);

    int maxVoices = PSP_SAS_VOICES_MAX;
    int sampleRate = 44100;
    int outputMode = PSP_SAS_OUTPUTMODE_MIXED;

    SasVoice voices[PSP_SAS_VOICES_MAX];
    WaveformEffect waveformEffect;

private:
    void MixVoice(SasVoice& voice);
    void ApplyWaveformEffect();
    void WriteMixedOutput(uint8_t* outp, const uint8_t* inp, int leftVol,
                          int rightVol);

    GuestMem mem_;
    SasReverb reverb_;
    int grainSize_ = 0;
    std::vector<int32_t> mixBuffer_;
    std::vector<int32_t> sendBuffer_;
    std::vector<int16_t> sendBufferDownsampled_;
    std::vector<int16_t> sendBufferProcessed_;
    // Extra margin for very high pitches.
    int16_t mixTemp_[PSP_SAS_MAX_GRAIN * 4 + 2 + 16];
};

}  // namespace psp_sas
