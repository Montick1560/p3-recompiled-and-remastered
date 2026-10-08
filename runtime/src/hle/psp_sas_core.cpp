// Ported from PPSSPP (GPL-2.0-or-later), Core/HW/SasAudio.cpp and
// Core/HW/SasReverb.cpp. Copyright (c) 2012- PPSSPP Project.
// See psp_sas_core.h for the list of deliberate deviations.

#include "hle/psp_sas_core.h"

#include <algorithm>
#include <cstring>

namespace psp_sas {

namespace {

inline int clamp_s16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

inline int16_t load_s16(const uint8_t* p) {
    int16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void store_s16(uint8_t* p, int v) {
    int16_t s = static_cast<int16_t>(v);
    std::memcpy(p, &s, sizeof(s));
}

// PS-ADPCM filter coefficients. Everything past index 4 is the hardware
// reading off the end of its own table, so those are garbage extra values.
const uint8_t kVagCoef[16][2] = {
    {   0,   0 },
    {  60,   0 },
    { 115,  52 },
    {  98,  55 },
    { 122,  60 },
    {   0,   0 },
    {   0,   0 },
    {  52,   0 },
    {  55,   2 },
    {  60, 125 },
    {   0,   0 },
    {   0,  91 },
    {   0,   0 },
    {   2, 216 },
    { 125,   6 },
    {   0, 151 },
};

}  // namespace

// ---------------------------------------------------------------- VAG

void VagDecoder::Start(uint32_t data, uint32_t vagSize, bool loopEnabled) {
    loopEnabled_ = loopEnabled;
    loopAtNextBlock_ = false;
    loopStartBlock_ = -1;
    numBlocks_ = static_cast<int>(vagSize / 16);
    end_ = false;
    data_ = data;
    read_ = data;
    curSample = 28;
    curBlock_ = -1;
    s_1 = 0;
    s_2 = 0;
}

void VagDecoder::DecodeBlock(const uint8_t*& read_pointer) {
    if (curBlock_ == numBlocks_ - 1) {
        end_ = true;
        return;
    }

    const uint8_t* readp = read_pointer;
    int predict_nr = *readp++;
    int shift_factor = predict_nr & 0xf;
    predict_nr >>= 4;
    int flags = *readp++;
    if (flags == 7) {
        end_ = true;
        return;
    } else if (flags == 6) {
        loopStartBlock_ = curBlock_;
    } else if (flags == 3) {
        if (loopEnabled_) {
            loopAtNextBlock_ = true;
        }
    }

    int s1 = s_1;
    int s2 = s_2;

    int coef1 = kVagCoef[predict_nr][0];
    int coef2 = -kVagCoef[predict_nr][1];

    for (int i = 0; i < 28; i += 2) {
        uint8_t d = *readp++;
        int sample1 = (short)((d & 0xf) << 12) >> shift_factor;
        int sample2 = (short)((d & 0xf0) << 8) >> shift_factor;
        s2 = clamp_s16(sample1 + ((s1 * coef1 + s2 * coef2) >> 6));
        s1 = clamp_s16(sample2 + ((s2 * coef1 + s1 * coef2) >> 6));
        samples[i] = static_cast<int16_t>(s2);
        samples[i + 1] = static_cast<int16_t>(s1);
    }

    s_1 = s1;
    s_2 = s2;
    curSample = 0;
    curBlock_++;

    read_pointer = readp;
}

void VagDecoder::GetSamples(const GuestMem& mem, int16_t* outSamples,
                            int numSamples) {
    if (end_) {
        std::memset(outSamples, 0, numSamples * sizeof(int16_t));
        return;
    }
    // The whole sample span must be readable; otherwise the voice is silent
    // and ends (a bad pointer must never reach host memory).
    const size_t span = static_cast<size_t>(numBlocks_) * 16;
    if (mem.range(data_, span) == nullptr) {
        end_ = true;
        std::memset(outSamples, 0, numSamples * sizeof(int16_t));
        return;
    }

    const uint8_t* readp = mem.range(read_, 0);
    const uint8_t* origp = readp;

    for (int i = 0; i < numSamples; i++) {
        if (curSample == 28) {
            if (loopAtNextBlock_) {
                // data_ starts at curBlock = -1.
                read_ = data_ + 16 * loopStartBlock_ + 16;
                readp = mem.range(read_, 0);
                origp = readp;
                curBlock_ = loopStartBlock_;
                loopAtNextBlock_ = false;
            }
            // read_ always sits on a block boundary inside the span
            // validated above; DecodeBlock ends the stream before reading
            // past the last block.
            DecodeBlock(readp);
            if (end_) {
                std::memset(&outSamples[i], 0,
                            (numSamples - i) * sizeof(int16_t));
                break;
            }
        }
        outSamples[i] = samples[curSample++];
    }

    if (readp > origp) {
        read_ += static_cast<uint32_t>(readp - origp);
    }
}

// ---------------------------------------------------------------- ADSR
// http://code.google.com/p/jpcsp/source/browse/trunk/src/jpcsp/HLE/modules150/sceSasCore.java

namespace {

int simpleRate(int n) {
    n &= 0x7F;
    if (n == 0x7F) {
        return 0;
    }
    int rate = ((7 - (n & 0x3)) << 26) >> (n >> 2);
    if (rate == 0) {
        return 1;
    }
    return rate;
}

int exponentRate(int n) {
    n &= 0x7F;
    if (n == 0x7F) {
        return 0;
    }
    int rate = ((7 - (n & 0x3)) << 24) >> (n >> 2);
    if (rate == 0) {
        return 1;
    }
    return rate;
}

int getAttackRate(int bitfield1) {
    return simpleRate(bitfield1 >> 8);
}

int getAttackType(int bitfield1) {
    return (bitfield1 & 0x8000) == 0 ? PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE
                                     : PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT;
}

int getDecayRate(int bitfield1) {
    int n = (bitfield1 >> 4) & 0x000F;
    if (n == 0)
        return 0x7FFFFFFF;
    return static_cast<int>(0x80000000u >> n);
}

int getSustainType(int bitfield2) {
    return (bitfield2 >> 14) & 3;
}

int getSustainRate(int bitfield2) {
    if (getSustainType(bitfield2) == PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE) {
        return exponentRate(bitfield2 >> 6);
    } else {
        return simpleRate(bitfield2 >> 6);
    }
}

int getReleaseType(int bitfield2) {
    return (bitfield2 & 0x0020) == 0 ? PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE
                                     : PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
}

int getReleaseRate(int bitfield2) {
    int n = bitfield2 & 0x001F;
    if (n == 31) {
        return 0;
    }
    if (getReleaseType(bitfield2) == PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE) {
        if (n == 30) {
            return 0x40000000;
        } else if (n == 29) {
            return 1;
        }
        return 0x10000000 >> n;
    }
    if (n == 0)
        return 0x7FFFFFFF;
    return static_cast<int>(0x80000000u >> n);
}

int getSustainLevel(int bitfield1) {
    return ((bitfield1 & 0x000F) + 1) << 26;
}

}  // namespace

void ADSREnvelope::SetEnvelope(int flag, int a, int d, int s, int r) {
    if ((flag & 0x1) != 0)
        attackType = (SasADSRCurveMode)a;
    if ((flag & 0x2) != 0)
        decayType = (SasADSRCurveMode)d;
    if ((flag & 0x4) != 0)
        sustainType = (SasADSRCurveMode)s;
    if ((flag & 0x8) != 0)
        releaseType = (SasADSRCurveMode)r;
}

void ADSREnvelope::SetRate(int flag, int a, int d, int s, int r) {
    if ((flag & 0x1) != 0)
        attackRate = a;
    if ((flag & 0x2) != 0)
        decayRate = d;
    if ((flag & 0x4) != 0)
        sustainRate = s;
    if ((flag & 0x8) != 0)
        releaseRate = r;
}

void ADSREnvelope::SetSimpleEnvelope(uint32_t ADSREnv1, uint32_t ADSREnv2) {
    attackRate = getAttackRate(ADSREnv1);
    attackType = (SasADSRCurveMode)getAttackType(ADSREnv1);
    decayRate = getDecayRate(ADSREnv1);
    decayType = PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
    sustainRate = getSustainRate(ADSREnv2);
    sustainType = (SasADSRCurveMode)getSustainType(ADSREnv2);
    releaseRate = getReleaseRate(ADSREnv2);
    releaseType = (SasADSRCurveMode)getReleaseType(ADSREnv2);
    sustainLevel = getSustainLevel(ADSREnv1);
}

void ADSREnvelope::WalkCurve(int type, int rate) {
    int64_t expDelta;
    switch (type) {
    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE:
        height_ += rate;
        break;

    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE:
        height_ -= rate;
        break;

    case PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT:
        if (height_ <= (int64_t)PSP_SAS_ENVELOPE_HEIGHT_MAX * 3 / 4) {
            height_ += rate;
        } else {
            height_ += rate / 4;
        }
        break;

    case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE:
        expDelta = height_ - PSP_SAS_ENVELOPE_HEIGHT_MAX;
        // Flipping the sign so that we can shift in the top bits.
        expDelta += (-expDelta * rate) >> 32;
        height_ = expDelta + PSP_SAS_ENVELOPE_HEIGHT_MAX -
                  ((int64_t)rate + 3) / 4;
        break;

    case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE:
        expDelta = height_ - PSP_SAS_ENVELOPE_HEIGHT_MAX;
        expDelta += (-expDelta * rate) >> 32;
        height_ = expDelta + 0x4000 + PSP_SAS_ENVELOPE_HEIGHT_MAX;
        break;

    case PSP_SAS_ADSR_CURVE_MODE_DIRECT:
        height_ = rate;
        break;
    }
}

void ADSREnvelope::SetState(ADSRState state) {
    if (height_ > PSP_SAS_ENVELOPE_HEIGHT_MAX) {
        height_ = PSP_SAS_ENVELOPE_HEIGHT_MAX;
    }
    state_ = state;
}

void ADSREnvelope::Step() {
    switch (state_) {
    case STATE_ATTACK:
        WalkCurve(attackType, attackRate);
        if (height_ >= PSP_SAS_ENVELOPE_HEIGHT_MAX || height_ < 0)
            SetState(STATE_DECAY);
        break;
    case STATE_DECAY:
        WalkCurve(decayType, decayRate);
        if (height_ < sustainLevel)
            SetState(STATE_SUSTAIN);
        break;
    case STATE_SUSTAIN:
        WalkCurve(sustainType, sustainRate);
        if (height_ <= 0) {
            height_ = 0;
            SetState(STATE_RELEASE);
        }
        break;
    case STATE_RELEASE:
        WalkCurve(releaseType, releaseRate);
        if (height_ <= 0) {
            height_ = 0;
            SetState(STATE_OFF);
        }
        break;
    case STATE_OFF:
        break;

    case STATE_KEYON:
        height_ = 0;
        SetState(STATE_KEYON_STEP);
        break;
    case STATE_KEYON_STEP:
        // Reproduces PSP behaviour: it takes 32 steps at 0 for key-on to
        // "kick in" before the attack phase begins.
        height_++;
        if (height_ >= 31) {
            height_ = 0;
            SetState(STATE_ATTACK);
        }
        break;
    }
}

void ADSREnvelope::KeyOn() {
    SetState(STATE_KEYON);
}

void ADSREnvelope::KeyOff() {
    SetState(STATE_RELEASE);
}

void ADSREnvelope::End() {
    SetState(STATE_OFF);
    height_ = 0;
}

// ---------------------------------------------------------------- Voice

void SasVoice::KeyOn() {
    envelope.KeyOn();
    if (type == VOICETYPE_VAG) {
        if (vagAddr == 0) {
            return;  // nothing to play: stays not-playing (ended)
        }
        vag.Start(vagAddr, vagSize, loop);
    }
    playing = true;
    on = true;
    paused = false;
    sampleFrac = 0;
}

void SasVoice::KeyOff() {
    on = false;
    envelope.KeyOff();
}

void SasVoice::SetVag(uint32_t addr, int size, bool loopOn) {
    if (size < 0) size = 0;
    bool reset = false;
    if (type != VOICETYPE_VAG || vagAddr != addr ||
        vagSize != static_cast<uint32_t>(size) || loop != loopOn) {
        type = VOICETYPE_VAG;
        reset = true;
    }
    vagAddr = addr;
    vagSize = static_cast<uint32_t>(size);
    loop = loopOn;
    if (on) {
        playing = true;
    }
    if (reset) {
        vag.Start(addr, static_cast<uint32_t>(size), loopOn);
    }
}

void SasVoice::SetPcm(uint32_t addr, int size, int loopPos) {
    type = VOICETYPE_PCM;
    pcmAddr = addr;
    pcmSize = size;
    pcmIndex = 0;
    pcmLoopPos = loopPos >= 0 ? loopPos : 0;
    loop = loopPos >= 0;
    playing = true;
}

void SasVoice::ReadSamples(const GuestMem& mem, int16_t* output,
                           int numSamples) {
    switch (type) {
    case VOICETYPE_VAG:
        vag.GetSamples(mem, output, numSamples);
        break;
    case VOICETYPE_PCM: {
        int needed = numSamples;
        int16_t* out = output;
        while (needed > 0) {
            int size = std::min(pcmSize - pcmIndex, needed);
            if (!on) {
                pcmIndex = 0;
                break;
            }
            if (size > 0) {
                const uint8_t* src = mem.range(
                    pcmAddr + static_cast<uint32_t>(pcmIndex) * 2,
                    static_cast<size_t>(size) * 2);
                if (src == nullptr) {
                    // Bad guest pointer: silence, and the voice ends.
                    pcmIndex = pcmSize;
                    loop = false;
                    break;
                }
                std::memcpy(out, src, static_cast<size_t>(size) * 2);
                pcmIndex += size;
                needed -= size;
                out += size;
            }
            if (pcmIndex >= pcmSize) {
                if (!loop) {
                    // All out, quit. HaveSamplesEnded() reports it.
                    break;
                }
                pcmIndex = pcmLoopPos;
            }
        }
        if (needed > 0) {
            std::memset(out, 0, static_cast<size_t>(needed) * sizeof(int16_t));
        }
        break;
    }
    case VOICETYPE_ATRAC3:
        // ATRAC3 voices are not implemented here: silence.
    default:
        std::memset(output, 0, numSamples * sizeof(int16_t));
        break;
    }
}

bool SasVoice::HaveSamplesEnded() const {
    switch (type) {
    case VOICETYPE_VAG:
        return vag.End();
    case VOICETYPE_PCM:
        return pcmIndex >= pcmSize;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- Reverb
// The Sas reverb really is the PSX SPU reverb, with some tweaks. The formula
// is nocash's: http://problemkaputt.de/psx-spx.htm#spureverbformula
// The preset constants are the PSP's own, not the PS1's.

struct SasReverbData {
    const char* name;
    int32_t size;

    int16_t dAPF1;
    int16_t dAPF2;
    int16_t vIIR;
    int16_t vCOMB1;
    int16_t vCOMB2;
    int16_t vCOMB3;
    int16_t vCOMB4;
    int16_t vWALL;

    int16_t vAPF1;
    int16_t vAPF2;
    int16_t mLSAME;
    int16_t mRSAME;
    int16_t mLCOMB1;
    int16_t mRCOMB1;
    int16_t mLCOMB2;
    int16_t mRCOMB2;

    int16_t dLSAME;
    int16_t dRSAME;
    int16_t mLDIFF;
    int16_t mRDIFF;
    int16_t mLCOMB3;
    int16_t mRCOMB3;
    int16_t mLCOMB4;
    int16_t mRCOMB4;

    int16_t dLDIFF;
    int16_t dRDIFF;
    int16_t mLAPF1;
    int16_t mRAPF1;
    int16_t mLAPF2;
    int16_t mRAPF2;
};

namespace {

constexpr int kNumReverbPresets = 9;

const SasReverbData kReverbPresets[kNumReverbPresets] = {
    {
        "Room",
        0x26C0,
        0x007D,0x005B,0x6D80,0x54B8,(int16_t)0xBED0,0x0000,0x0000,(int16_t)0xBA80,
        0x5800,0x5300,0x04D6,0x0333,0x03F0,0x0227,0x0374,0x01EF,
        // The diff-side taps used to be zero here, straight from the PS1 table. The PSP has real values.
        0x0336,0x01B7,0x0335,0x01B6,0x0334,0x01B5,0x0334,0x01B5,
        0x0334,0x01B5,0x01B4,0x0136,0x00B8,0x005C,
    },
    {
        "Studio Small",
        0x1F40,
        0x0033,0x0025,0x70F0,0x4FA8,(int16_t)0xBCE0,0x4410,(int16_t)0xC0F0,(int16_t)0x9C00,
        0x5280,0x4EC0,0x03E4,0x031B,0x03A4,0x02AF,0x0372,0x0266,
        0x031C,0x025D,0x025C,0x018E,0x022F,0x0135,0x01D2,0x00B7,
        0x018F,0x00B5,0x00B4,0x0080,0x004C,0x0026,
    },
    {
        "Studio Medium",
        0x4840,
        0x00B1,0x007F,0x70F0,0x4FA8,(int16_t)0xBCE0,0x4510,(int16_t)0xBEF0,(int16_t)0xB4C0,
        0x5280,0x4EC0,0x0904,0x076B,0x0824,0x065F,0x07A2,0x0616,
        0x076C,0x05ED,0x05EC,0x042E,0x050F,0x0305,0x0462,0x02B7,
        0x042F,0x0265,0x0264,0x01B2,0x0100,0x0080,
    },
    {
        "Studio Large",
        0x6FE0,
        0x00E3,0x00A9,0x6F60,0x4FA8,(int16_t)0xBCE0,0x4510,(int16_t)0xBEF0,(int16_t)0xA680,
        0x5680,0x52C0,0x0DFB,0x0B58,0x0D09,0x0A3C,0x0BD9,0x0973,
        0x0B59,0x08DA,0x08D9,0x05E9,0x07EC,0x04B0,0x06EF,0x03D2,
        0x05EA,0x031D,0x031C,0x0238,0x0154,0x00AA,
    },
    {
        "Hall",
        0xADE0,
        0x01A5,0x0139,0x6000,0x5000,0x4C00,(int16_t)0xB800,(int16_t)0xBC00,(int16_t)0xC000,
        0x6000,0x5C00,0x15BA,0x11BB,0x14C2,0x10BD,0x11BC,0x0DC1,
        0x11C0,0x0DC3,0x0DC0,0x09C1,0x0BC4,0x07C1,0x0A00,0x06CD,
        0x09C2,0x05C1,0x05C0,0x041A,0x0274,0x013A,
    },
    {
        "Space Echo",
        0xF6C0,
        0x033D,0x0231,0x7E00,0x5000,(int16_t)0xB400,(int16_t)0xB000,0x4C00,(int16_t)0xB000,
        0x6000,0x5400,0x1ED6,0x1A31,0x1D14,0x183B,0x1BC2,0x16B2,
        0x1A32,0x15EF,0x15EE,0x1055,0x1334,0x0F2D,0x11F6,0x0C5D,
        0x1056,0x0AE1,0x0AE0,0x07A2,0x0464,0x0232,
    },
    // Echo and Delay are not really presets: the hardware recomputes mLSAME,
    // mRSAME, mLCOMB1, dLSAME, mLAPF1, mRAPF1 and vWALL from sceSasRevParam's
    // delay and feedback whenever the preset is (re)loaded - see
    // ApplyParams(). The stored values are what the recompute produces at
    // delay 255; sceSasRevParam caps delay at 127 and a game that never calls
    // it leaves delay at 0.
    {
        "Echo (almost infinite)",
        0x18040,
        0x0003,0x0003,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,(int16_t)0x8100,
        0x0000,0x0000,0x1FFD,0x0FFD,0x1009,0x0009,0x0000,0x0000,
        0x1009,0x0009,0x1FFF,0x1FFF,0x1FFE,0x1FFE,0x1FFE,0x1FFE,
        0x1FFE,0x1FFE,0x1008,0x1004,0x0008,0x0004,
    },
    // Identical to Echo apart from vWALL, the default feedback: full for
    // Echo, none for Delay.
    {
        "Delay (one - shot echo)",
        0x18040,
        0x0003,0x0003,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,0x0000,
        0x0000,0x0000,0x1FFD,0x0FFD,0x1009,0x0009,0x0000,0x0000,
        0x1009,0x0009,0x1FFF,0x1FFF,0x1FFE,0x1FFE,0x1FFE,0x1FFE,
        0x1FFE,0x1FFE,0x1008,0x1004,0x0008,0x0004,
    },
    {
        "Half Echo",
        0x3C00,
        0x0017,0x0013,0x70F0,0x4FA8,(int16_t)0xBCE0,0x4510,(int16_t)0xBEF0,(int16_t)0x8500,
        0x5F80,0x54C0,0x0371,0x02AF,0x02E5,0x01DF,0x02B0,0x01D7,
        0x0358,0x026A,0x01D6,0x011E,0x012D,0x00B1,0x011F,0x0059,
        0x01A0,0x00E3,0x0058,0x0040,0x0028,0x0014,
    },
};

// Wraps around the upper part of a buffer.
template <int bufsize>
class BufferWrapper {
public:
    BufferWrapper(int16_t* buffer, int position, int usedSize)
        : buf_(buffer), pos_(position), end_(bufsize),
          base_(bufsize - usedSize), size_(usedSize) {}
    int16_t& operator[](int index) {
        int addr = pos_ + index;
        if (addr >= end_) { addr -= size_; }
        if (addr < base_) { addr += size_; }
        return buf_[addr];
    }

    int GetPosition() { return pos_; }
    void Next() {
        pos_++;
        if (pos_ >= end_) {
            pos_ -= size_;
        }
    }

private:
    int16_t* buf_;
    int pos_;
    int end_;
    int base_;
    int size_;
};

}  // namespace

SasReverb::SasReverb() : workspace_(BUFSIZE, 0) {
    data_ = new SasReverbData{};
}

SasReverb::~SasReverb() {
    delete data_;
}

const char* SasReverb::GetPresetName(int preset) {
    if (preset == -1) {
        return "Off";
    } else if (preset < 0 || preset >= kNumReverbPresets) {
        return "Invalid";
    }
    return kReverbPresets[preset].name;
}

void SasReverb::SetPreset(int preset) {
    // -1 means "off"; anything else must be a valid preset index.
    if (preset >= -1 && preset < kNumReverbPresets)
        preset_ = preset;
    if (preset_ != -1) {
        pos_ = BUFSIZE - kReverbPresets[preset_].size;
        std::fill(workspace_.begin(), workspace_.end(), int16_t(0));
    } else {
        pos_ = 0;
    }
    ApplyParams();
}

void SasReverb::SetParams(int delay, int feedback) {
    delay_ = delay;
    feedback_ = feedback;
    ApplyParams();
}

// Echo and Delay are computed presets. With D = delay + 1:
//   mLSAME = D*32 - dAPF1     mLCOMB1 = mRCOMB1 + D*16    mLAPF1 = mLAPF2 + D*16
//   mRSAME = D*16 - dAPF2     dLSAME  = dRSAME  + D*16    mRAPF1 = D*16 + mLAPF2/2
//   vWALL  = -(feedback << 8)
void SasReverb::ApplyParams() {
    if (preset_ == -1) {
        return;
    }
    *data_ = kReverbPresets[preset_];
    if (preset_ != PSP_SAS_EFFECT_TYPE_ECHO &&
        preset_ != PSP_SAS_EFFECT_TYPE_DELAY) {
        return;
    }

    const int d16 = (delay_ + 1) * 16;
    data_->mLSAME = (int16_t)(d16 * 2 - data_->dAPF1);
    data_->mRSAME = (int16_t)(d16 - data_->dAPF2);
    data_->mLCOMB1 = (int16_t)(data_->mRCOMB1 + d16);
    data_->dLSAME = (int16_t)(data_->dRSAME + d16);
    data_->mLAPF1 = (int16_t)(data_->mLAPF2 + d16);
    data_->mRAPF1 = (int16_t)(d16 + (data_->mLAPF2 >> 1));
    data_->vWALL = (int16_t)(-(feedback_ << 8));
}

void SasReverb::ProcessReverb(int16_t* output, const int16_t* input,
                              size_t inputSize, int volLeft, int volRight) {
    // Reverb "off" replicates the input signal into the processed buffer.
    // (Strangely, OFF is not zero-filled every other frame; it is special
    // cased on the hardware.)
    if (preset_ == -1) {
        for (size_t i = 0; i < inputSize; ++i) {
            output[i * 4 + 0] = (int16_t)clamp_s16((int)input[i * 2 + 0] * volLeft >> 15);
            output[i * 4 + 1] = (int16_t)clamp_s16((int)input[i * 2 + 1] * volRight >> 15);
            output[i * 4 + 2] = (int16_t)clamp_s16((int)input[i * 2 + 0] * volLeft >> 15);
            output[i * 4 + 3] = (int16_t)clamp_s16((int)input[i * 2 + 1] * volRight >> 15);
        }
        return;
    }

    // The ME's reverb return is (evol * out) >> 11, not >> 12. Our caller
    // passes leftVol << 3 to pair with the >> 15 below, which is one bit
    // short of that. (PPSSPP additionally scales by a user reverb-volume
    // setting; here that is fixed at unity.)
    volLeft <<= 1;
    volRight <<= 1;

    const SasReverbData& d = *data_;

    BufferWrapper<BUFSIZE> b(workspace_.data(), pos_, d.size);

    // This runs at 22kHz.
    for (size_t i = 0; i < inputSize; i++) {
        // The ME feeds the reverb at a quarter of the mix level.
        int16_t LeftInput = input[i * 2] >> 2;
        int16_t RightInput = input[i * 2 + 1] >> 2;

        int16_t Lin = LeftInput;
        int16_t Rin = RightInput;

        // ____Same Side Reflection(left - to - left and right - to - right)___
        b[d.mLSAME] = (int16_t)clamp_s16(Lin + (b[d.dLSAME] * d.vWALL >> 15) - (b[d.mLSAME - 1] * d.vIIR >> 15) + b[d.mLSAME - 1]);
        b[d.mRSAME] = (int16_t)clamp_s16(Rin + (b[d.dRSAME] * d.vWALL >> 15) - (b[d.mRSAME - 1] * d.vIIR >> 15) + b[d.mRSAME - 1]);
        // ___Different Side Reflection(left - to - right and right - to - left)
        b[d.mLDIFF] = (int16_t)clamp_s16(Lin + (b[d.dRDIFF] * d.vWALL >> 15) - (b[d.mLDIFF - 1] * d.vIIR >> 15) + b[d.mLDIFF - 1]);
        b[d.mRDIFF] = (int16_t)clamp_s16(Rin + (b[d.dLDIFF] * d.vWALL >> 15) - (b[d.mRDIFF - 1] * d.vIIR >> 15) + b[d.mRDIFF - 1]);
        // ___Early Echo(Comb Filter, with input from buffer)____________________
        int32_t Lout = ((d.vCOMB1 * b[d.mLCOMB1] + d.vCOMB2 * b[d.mLCOMB2] + d.vCOMB3 * b[d.mLCOMB3] + d.vCOMB4 * b[d.mLCOMB4]) >> 15);
        int32_t Rout = ((d.vCOMB1 * b[d.mRCOMB1] + d.vCOMB2 * b[d.mRCOMB2] + d.vCOMB3 * b[d.mRCOMB3] + d.vCOMB4 * b[d.mRCOMB4]) >> 15);
        // ___Late Reverb APF1(All Pass Filter 1, with input from COMB)_________
        b[d.mLAPF1] = (int16_t)clamp_s16(Lout - (d.vAPF1 * b[(d.mLAPF1 - d.dAPF1)] >> 15));
        Lout = b[(d.mLAPF1 - d.dAPF1)] + (b[d.mLAPF1] * d.vAPF1 >> 15);
        b[d.mRAPF1] = (int16_t)clamp_s16(Rout - (d.vAPF1 * b[(d.mRAPF1 - d.dAPF1)] >> 15));
        Rout = b[(d.mRAPF1 - d.dAPF1)] + (b[d.mRAPF1] * d.vAPF1 >> 15);
        // ___Late Reverb APF2(All Pass Filter 2, with input from APF1)_________
        b[d.mLAPF2] = (int16_t)clamp_s16(Lout - (d.vAPF2 * b[(d.mLAPF2 - d.dAPF2)] >> 15));
        Lout = b[(d.mLAPF2 - d.dAPF2)] + (b[d.mLAPF2] * d.vAPF2 >> 15);
        b[d.mRAPF2] = (int16_t)clamp_s16(Rout - (d.vAPF2 * b[(d.mRAPF2 - d.dAPF2)] >> 15));
        Rout = b[(d.mRAPF2 - d.dAPF2)] + (b[d.mRAPF2] * d.vAPF2 >> 15);
        // ___Output to Mixer(Output volume multiplied with input from APF2)____
        output[i * 4 + 0] = (int16_t)clamp_s16((Lout * volLeft) >> 15);
        output[i * 4 + 1] = (int16_t)clamp_s16((Rout * volRight) >> 15);
        output[i * 4 + 2] = 0;
        output[i * 4 + 3] = 0;

        b.Next();
    }

    pos_ = b.GetPosition();
}

// ---------------------------------------------------------------- Instance

SasInstance::SasInstance() {
    std::memset(mixTemp_, 0, sizeof(mixTemp_));
}

SasInstance::~SasInstance() = default;

void SasInstance::SetGrainSize(int newGrainSize) {
    if (newGrainSize < 0) newGrainSize = 0;
    if (newGrainSize > PSP_SAS_MAX_GRAIN) newGrainSize = PSP_SAS_MAX_GRAIN;
    grainSize_ = newGrainSize;

    mixBuffer_.assign(static_cast<size_t>(grainSize_) * 2, 0);
    sendBuffer_.assign(static_cast<size_t>(grainSize_) * 2, 0);
    sendBufferDownsampled_.assign(static_cast<size_t>(grainSize_), 0);
    sendBufferProcessed_.assign(static_cast<size_t>(grainSize_) * 2, 0);
}

uint32_t SasInstance::GetEndFlag() const {
    uint32_t endFlag = 0;
    for (int i = 0; i < maxVoices; i++) {
        if (!voices[i].playing)
            endFlag |= (1u << i);
    }
    return endFlag;
}

uint32_t SasInstance::GetPauseFlag() const {
    uint32_t pauseFlag = 0;
    for (int i = 0; i < maxVoices; i++) {
        if (voices[i].paused)
            pauseFlag |= (1u << i);
    }
    return pauseFlag;
}

void SasInstance::SetWaveformEffectType(int type) {
    if (type != waveformEffect.type) {
        waveformEffect.type = type;
        reverb_.SetPreset(type);
    }
}

void SasInstance::SetWaveformEffectParams(int delay, int feedback) {
    waveformEffect.delay = delay;
    waveformEffect.feedback = feedback;
    // Echo and Delay compute most of their parameters from these.
    reverb_.SetParams(delay, feedback);
}

void SasInstance::MixVoice(SasVoice& voice) {
    switch (voice.type) {
    case VOICETYPE_VAG:
        if (!voice.vagAddr)
            break;
        [[fallthrough]];
    case VOICETYPE_PCM:
        if (voice.type == VOICETYPE_PCM && !voice.pcmAddr)
            break;
        [[fallthrough]];
    default: {
        const int grainSize = grainSize_;
        const int voicePitch =
            std::min(std::max(voice.pitch, (int)PSP_SAS_PITCH_MIN),
                     (int)PSP_SAS_PITCH_MAX);

        // The first 32 samples after a key-on are 0s.
        int delay = 0;
        if (voice.envelope.NeedsKeyOn()) {
            const bool ignorePitch =
                voice.type == VOICETYPE_PCM && voicePitch > PSP_SAS_PITCH_BASE;
            delay = ignorePitch
                        ? 32
                        : (int)((32 * (uint32_t)voicePitch) >> PSP_SAS_PITCH_BASE_SHIFT);
            // VAG seems to have an extra sample delay (not shared by PCM.)
            if (voice.type == VOICETYPE_VAG)
                ++delay;
        }

        // Two passes: first read, then resample to exactly "grainSize"
        // samples. mixTemp_ fits 4x that, as the max pitch is 0x4000.
        mixTemp_[0] = voice.resampleHist[0];
        mixTemp_[1] = voice.resampleHist[1];

        uint32_t sampleFrac = voice.sampleFrac;
        int samplesToRead =
            (int)((sampleFrac + (uint32_t)voicePitch *
                                    (uint32_t)std::max(0, grainSize - delay)) >>
                  PSP_SAS_PITCH_BASE_SHIFT);
        const int tempCap = (int)(sizeof(mixTemp_) / sizeof(mixTemp_[0]));
        if (samplesToRead > tempCap - 2) {
            samplesToRead = tempCap - 2;
        }
        int readPos = 2;
        if (voice.envelope.NeedsKeyOn()) {
            readPos = 0;
            samplesToRead += 2;
            if (samplesToRead > tempCap) samplesToRead = tempCap;
        }
        voice.ReadSamples(mem_, &mixTemp_[readPos], samplesToRead);
        int tempPos = readPos + samplesToRead;

        for (int i = 0; i < delay; ++i) {
            // Walk the curve. This means we'll reach ATTACK already, likely.
            voice.envelope.Step();
        }

        const bool needsInterp =
            voicePitch != PSP_SAS_PITCH_BASE ||
            (sampleFrac & PSP_SAS_PITCH_MASK) != 0;
        for (int i = delay; i < grainSize; i++) {
            const int16_t* s = mixTemp_ + (sampleFrac >> PSP_SAS_PITCH_BASE_SHIFT);

            // Two-tap linear interpolation, as the hardware does.
            int sample = s[0];
            if (needsInterp) {
                int f = sampleFrac & PSP_SAS_PITCH_MASK;
                sample = s[0] - (((s[0] - s[1]) * f) >> PSP_SAS_PITCH_BASE_SHIFT);
            }
            sampleFrac += (uint32_t)voicePitch;

            // The maximum envelope height is 1 << 30. Reduce to 14 bits by
            // shifting off 15, rounding by adding (1 << 14) first.
            int envelopeValue = voice.envelope.GetHeight();
            voice.envelope.Step();
            envelopeValue = (envelopeValue + (1 << 14)) >> 15;

            // Scale by the envelope before the volumes (again rounding).
            sample = ((sample * envelopeValue) + (1 << 14)) >> 15;

            // Mix into 32-bit temp buffers; clipping happens in the output loop.
            mixBuffer_[i * 2] += (sample * voice.volumeLeft) >> 12;
            mixBuffer_[i * 2 + 1] += (sample * voice.volumeRight) >> 12;
            sendBuffer_[i * 2] += sample * voice.effectLeft >> 12;
            sendBuffer_[i * 2 + 1] += sample * voice.effectRight >> 12;
        }

        voice.resampleHist[0] = mixTemp_[tempPos - 2];
        voice.resampleHist[1] = mixTemp_[tempPos - 1];

        voice.sampleFrac = sampleFrac - (uint32_t)(tempPos - 2) * PSP_SAS_PITCH_BASE;

        if (voice.HaveSamplesEnded())
            voice.envelope.End();
        if (voice.envelope.HasEnded()) {
            voice.playing = false;
            voice.on = false;
        }
        break;
    }
    }
}

bool SasInstance::Mix(uint32_t outAddr, uint32_t inAddr, int leftVol,
                      int rightVol, bool mute) {
    if (grainSize_ <= 0) {
        return false;
    }
    for (int v = 0; v < PSP_SAS_VOICES_MAX; v++) {
        SasVoice& voice = voices[v];
        if (!voice.playing || voice.paused)
            continue;
        MixVoice(voice);
    }

    if (mute) {
        std::fill(mixBuffer_.begin(), mixBuffer_.end(), 0);
        std::fill(sendBuffer_.begin(), sendBuffer_.end(), 0);
    }

    const size_t grain = static_cast<size_t>(grainSize_);
    const size_t outBytes =
        outputMode == PSP_SAS_OUTPUTMODE_MIXED ? grain * 4 : grain * 8;
    uint8_t* outp = mem_.range(outAddr, outBytes);
    const uint8_t* inp = inAddr ? mem_.range(inAddr, grain * 4) : nullptr;
    const bool outOk = outp != nullptr;

    if (outOk && outputMode == PSP_SAS_OUTPUTMODE_MIXED) {
        WriteMixedOutput(outp, inp, leftVol, rightVol);
    } else if (outOk) {
        // Raw: four planes of `grain` samples each: L, R, send L, send R.
        uint8_t* outpL = outp + grain * 2 * 0;
        uint8_t* outpR = outp + grain * 2 * 1;
        uint8_t* outpSendL = outp + grain * 2 * 2;
        uint8_t* outpSendR = outp + grain * 2 * 3;
        for (size_t i = 0; i < grain * 2; i += 2) {
            store_s16(outpL, clamp_s16(mixBuffer_[i + 0])); outpL += 2;
            store_s16(outpR, clamp_s16(mixBuffer_[i + 1])); outpR += 2;
            store_s16(outpSendL, clamp_s16(sendBuffer_[i + 0])); outpSendL += 2;
            store_s16(outpSendR, clamp_s16(sendBuffer_[i + 1])); outpSendR += 2;
        }
    }
    std::fill(mixBuffer_.begin(), mixBuffer_.end(), 0);
    std::fill(sendBuffer_.begin(), sendBuffer_.end(), 0);
    return outOk;
}

// Note: leftVol/rightVol are how much to scale the input by, not mixBuffer.
void SasInstance::WriteMixedOutput(uint8_t* outp, const uint8_t* inp,
                                   int leftVol, int rightVol) {
    const bool dry = waveformEffect.isDryOn != 0;
    const bool wet = waveformEffect.isWetOn != 0;
    if (wet) {
        ApplyWaveformEffect();
    }

    for (size_t i = 0; i < static_cast<size_t>(grainSize_) * 2; i += 2) {
        int sampleL = 0;
        int sampleR = 0;
        if (inp) {
            // Read before writing: in == out is allowed.
            sampleL = (load_s16(inp + i * 2) * leftVol >> 12);
            sampleR = (load_s16(inp + i * 2 + 2) * rightVol >> 12);
        }
        if (dry) {
            sampleL += mixBuffer_[i + 0];
            sampleR += mixBuffer_[i + 1];
        }
        if (wet) {
            sampleL += sendBufferProcessed_[i + 0];
            sampleR += sendBufferProcessed_[i + 1];
        }
        store_s16(outp + i * 2, clamp_s16(sampleL));
        store_s16(outp + i * 2 + 2, clamp_s16(sampleR));
    }
}

// http://psx.rules.org/spu.txt has some information about setting up the
// delay time by modifying the delay preset.
void SasInstance::ApplyWaveformEffect() {
    // Downsample the send buffer to 22kHz, naively.
    for (int i = 0; i < grainSize_ / 2; i++) {
        sendBufferDownsampled_[i * 2] = (int16_t)clamp_s16(sendBuffer_[i * 4]);
        sendBufferDownsampled_[i * 2 + 1] = (int16_t)clamp_s16(sendBuffer_[i * 4 + 1]);
    }

    // Volume max is 0x1000, while our factor is up to 0x8000: shift left by 3.
    reverb_.ProcessReverb(sendBufferProcessed_.data(),
                          sendBufferDownsampled_.data(), grainSize_ / 2,
                          (uint16_t)(waveformEffect.leftVol << 3),
                          (uint16_t)(waveformEffect.rightVol << 3));
}

}  // namespace psp_sas
