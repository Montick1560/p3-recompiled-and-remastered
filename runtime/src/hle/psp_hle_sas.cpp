#include "hle/psp_hle.h"
#include "hle/psp_sas_core.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

// ================================================================
// SAS (Software Audio Synthesis) HLE: thin glue between the guest's
// __sceSas* calls and the synthesis core in psp_sas_core.cpp (ported
// from PPSSPP Core/HW/SasAudio.cpp and Core/HLE/sceSas.cpp).
//
// Argument order, return values and error codes follow PPSSPP's
// sceSas.cpp. PPSSPP's hleDelayResult mix timing is dropped: pacing
// comes from sceAudioOutput*Blocking.
//
// The game's sound-completion gate polls a per-voice active byte that
// only clears when __sceSasGetEndFlag reports the voice ended; the
// core flips a voice to "ended" only once its sample data has been
// consumed (VAG end/size, PCM end, or envelope release finishing).
//
// Argument convention: PSP syscalls pass a0..a3 in ctx->r[4..7] and
// the 5th+ args in t0.. (ctx->r[8..]) -- same as sceIoLseek's whence
// in psp_hle_io.cpp.
// ================================================================

namespace {

using namespace psp_sas;

// PPSSPP Core/HLE/ErrorCodes.h
constexpr int32_t SAS_ERR_INVALID_GRAIN = (int32_t)0x80420001U;
constexpr int32_t SAS_ERR_INVALID_MAX_VOICES = (int32_t)0x80420002U;
constexpr int32_t SAS_ERR_INVALID_OUTPUT_MODE = (int32_t)0x80420003U;
constexpr int32_t SAS_ERR_INVALID_SAMPLE_RATE = (int32_t)0x80420004U;
constexpr int32_t SAS_ERR_BAD_ADDRESS = (int32_t)0x80420005U;
constexpr int32_t SAS_ERR_INVALID_VOICE = (int32_t)0x80420010U;
constexpr int32_t SAS_ERR_INVALID_NOISE_FREQ = (int32_t)0x80420011U;
constexpr int32_t SAS_ERR_INVALID_PITCH = (int32_t)0x80420012U;
constexpr int32_t SAS_ERR_INVALID_ADSR_CURVE_MODE = (int32_t)0x80420013U;
constexpr int32_t SAS_ERR_INVALID_PARAMETER = (int32_t)0x80420014U;
constexpr int32_t SAS_ERR_INVALID_LOOP_POS = (int32_t)0x80420015U;
constexpr int32_t SAS_ERR_VOICE_PAUSED = (int32_t)0x80420016U;
constexpr int32_t SAS_ERR_INVALID_VOLUME = (int32_t)0x80420018U;
constexpr int32_t SAS_ERR_INVALID_ADSR_RATE = (int32_t)0x80420019U;
constexpr int32_t SAS_ERR_INVALID_PCM_SIZE = (int32_t)0x8042001AU;
constexpr int32_t SAS_ERR_REV_INVALID_TYPE = (int32_t)0x80420020U;
constexpr int32_t SAS_ERR_REV_INVALID_FEEDBACK = (int32_t)0x80420021U;
constexpr int32_t SAS_ERR_REV_INVALID_DELAY = (int32_t)0x80420022U;
constexpr int32_t SAS_ERR_REV_INVALID_VOLUME = (int32_t)0x80420023U;
constexpr int32_t SAS_ERR_ATRAC3_ALREADY_SET = (int32_t)0x80420040U;
constexpr int32_t KERNEL_ERR_BAD_ARGUMENT = (int32_t)0x80000004U;

// Grain used until the game calls __sceSasInit (matches the previous
// state-machine stub, so an early __sceSasCore still has a size).
constexpr int SAS_DEFAULT_GRAIN = 256;

// SAS calls come from the PCM thread while voice setup may come from
// other game threads -- guard all state with one mutex.
std::mutex g_sas_mtx;

struct SasState {
    SasInstance sas;
    SasState() { sas.SetGrainSize(SAS_DEFAULT_GRAIN); }
};
SasState g_state;
SasInstance& g_sas = g_state.sas;

/// Approximation of Memory::IsValidAddress: RAM, VRAM or scratchpad, with
/// the uncached/cached mirror bits ignored.
bool valid_addr(uint32_t addr) {
    addr &= 0x3FFFFFFFU;
    return (addr >= 0x08000000U && addr < 0x0C000000U) ||
           (addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + PSP_VRAM_SIZE) ||
           (addr >= PSP_SCRATCHPAD_BASE &&
            addr < PSP_SCRATCHPAD_BASE + PSP_SCRATCHPAD_SIZE);
}

bool voice_ok(int voice) {
    return voice >= 0 && voice < PSP_SAS_VOICES_MAX;
}

/// Writes the syscall result and (re)binds the core to this call's rdram.
inline void ret(recomp_context* ctx, int32_t v) { ctx->r[2] = v; }
inline void bind_mem(uint8_t* rdram) { g_sas.SetMemory(rdram, PSP_MEM_SIZE); }

// ---- HLE handlers ----

void hle_sceSasInit(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, grain=a1, maxVoices=a2, outMode=a3, sampleRate=t0)
    uint32_t core = static_cast<uint32_t>(ctx->r[4]);
    uint32_t grain = static_cast<uint32_t>(ctx->r[5]);
    uint32_t max_voices = static_cast<uint32_t>(ctx->r[6]);
    uint32_t out_mode = static_cast<uint32_t>(ctx->r[7]);
    uint32_t rate = static_cast<uint32_t>(ctx->r[8]);
    if (!valid_addr(core) || (core & 0x3F) != 0) {
        ret(ctx, SAS_ERR_BAD_ADDRESS);
        return;
    }
    if (max_voices == 0 || max_voices > PSP_SAS_VOICES_MAX) {
        ret(ctx, SAS_ERR_INVALID_MAX_VOICES);
        return;
    }
    if (grain < 0x40 || grain > 0x800 || (grain & 0x1F) != 0) {
        ret(ctx, SAS_ERR_INVALID_GRAIN);
        return;
    }
    if (out_mode != 0 && out_mode != 1) {
        ret(ctx, SAS_ERR_INVALID_OUTPUT_MODE);
        return;
    }
    if (rate != 44100) {
        ret(ctx, SAS_ERR_INVALID_SAMPLE_RATE);
        return;
    }

    std::lock_guard<std::mutex> lock(g_sas_mtx);
    bind_mem(rdram);
    g_sas.SetGrainSize(static_cast<int>(grain));
    // Seems like the maxVoices param is actually ignored for all intents
    // and purposes.
    g_sas.maxVoices = PSP_SAS_VOICES_MAX;
    g_sas.outputMode = static_cast<int>(out_mode);
    // Deviation from PPSSPP (which only clears playing/loop): a (re)init
    // returns every voice to its power-on state, as the old stub did, so a
    // stale key-on cannot make the next __sceSasSetKeyOn fail.
    for (auto& v : g_sas.voices) {
        v = SasVoice{};
        v.sampleRate = static_cast<int>(rate);
    }
    ret(ctx, 0);
}

void hle_sceSasGetEndFlag(uint8_t* rdram, recomp_context* ctx) {
    // Bit i set iff voice i is NOT playing (initially 0xFFFFFFFF).
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    ret(ctx, static_cast<int32_t>(g_sas.GetEndFlag()));
    (void)rdram;
}

void hle_sceSasCore(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, out=a1)
    uint32_t out_addr = static_cast<uint32_t>(ctx->r[5]);
    if (!valid_addr(out_addr)) {
        ret(ctx, SAS_ERR_INVALID_PARAMETER);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    bind_mem(rdram);
    g_sas.Mix(out_addr, 0, 0, 0);
    ret(ctx, 0);
}

void hle_sceSasCoreWithMix(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, inout=a1, leftVol=a2, rightVol=a3)
    uint32_t inout_addr = static_cast<uint32_t>(ctx->r[5]);
    int left_vol = ctx->r[6];
    int right_vol = ctx->r[7];
    if (!valid_addr(inout_addr)) {
        ret(ctx, SAS_ERR_INVALID_PARAMETER);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    if (g_sas.outputMode == PSP_SAS_OUTPUTMODE_RAW) {
        ret(ctx, KERNEL_ERR_BAD_ARGUMENT);  // unsupported outputMode
        return;
    }
    bind_mem(rdram);
    g_sas.Mix(inout_addr, inout_addr, left_vol, right_vol);
    ret(ctx, 0);
}

void hle_sceSasSetVoice(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, vagAddr=a2, size=a3, loop=t0)
    int voice = ctx->r[5];
    uint32_t vag_addr = static_cast<uint32_t>(ctx->r[6]);
    int size = ctx->r[7];
    int loop = ctx->r[8];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    if (size == 0 || (static_cast<uint32_t>(size) & 0xF) != 0) {
        ret(ctx, SAS_ERR_INVALID_PARAMETER);
        return;
    }
    if (loop != 0 && loop != 1) {
        ret(ctx, SAS_ERR_INVALID_LOOP_POS);
        return;
    }
    if (!valid_addr(vag_addr)) {
        ret(ctx, 0);  // PPSSPP ignores an invalid VAG address
        return;
    }

    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice& v = g_sas.voices[voice];
    if (v.type == VOICETYPE_ATRAC3) {
        ret(ctx, SAS_ERR_ATRAC3_ALREADY_SET);
        return;
    }
    // Negative sizes return OK but must not play any audio (PPSSPP hack).
    v.SetVag(vag_addr, size < 0 ? 0 : size, loop != 0);
    ret(ctx, 0);
}

void hle_sceSasSetVoicePCM(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, pcmAddr=a2, size=a3, loopPos=t0)
    int voice = ctx->r[5];
    uint32_t pcm_addr = static_cast<uint32_t>(ctx->r[6]);
    int size = ctx->r[7];
    int loop_pos = ctx->r[8];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    if (size <= 0 || size > 0x10000) {
        ret(ctx, SAS_ERR_INVALID_PCM_SIZE);
        return;
    }
    if (loop_pos >= size) {
        ret(ctx, SAS_ERR_INVALID_LOOP_POS);
        return;
    }
    if (!valid_addr(pcm_addr)) {
        ret(ctx, 0);
        return;
    }

    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice& v = g_sas.voices[voice];
    if (v.type == VOICETYPE_ATRAC3) {
        ret(ctx, SAS_ERR_ATRAC3_ALREADY_SET);
        return;
    }
    v.SetPcm(pcm_addr, size, loop_pos);
    ret(ctx, 0);
}

void hle_sceSasGetPauseFlag(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    ret(ctx, static_cast<int32_t>(g_sas.GetPauseFlag()));
    (void)rdram;
}

void hle_sceSasSetPause(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voiceMask=a1, pause=a2)
    uint32_t voice_mask = static_cast<uint32_t>(ctx->r[5]);
    bool pause = ctx->r[6] != 0;
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    for (int i = 0; i < PSP_SAS_VOICES_MAX; i++) {
        if (voice_mask & (1U << i)) g_sas.voices[i].paused = pause;
    }
    ret(ctx, 0);
    (void)rdram;
}

void hle_sceSasSetVolume(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, left=a2, right=a3, effectLeft=t0, effectRight=t1)
    int voice = ctx->r[5];
    int64_t l = ctx->r[6], r = ctx->r[7], el = ctx->r[8], er = ctx->r[9];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    if (std::llabs(l) > PSP_SAS_VOL_MAX || std::llabs(r) > PSP_SAS_VOL_MAX ||
        std::llabs(el) > PSP_SAS_VOL_MAX || std::llabs(er) > PSP_SAS_VOL_MAX) {
        ret(ctx, SAS_ERR_INVALID_VOLUME);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice& v = g_sas.voices[voice];
    v.volumeLeft = static_cast<int>(l);
    v.volumeRight = static_cast<int>(r);
    v.effectLeft = static_cast<int>(el);
    v.effectRight = static_cast<int>(er);
    ret(ctx, 0);
}

void hle_sceSasSetPitch(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, pitch=a2)
    int voice = ctx->r[5];
    int pitch = ctx->r[6];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    if (pitch < PSP_SAS_PITCH_MIN || pitch > PSP_SAS_PITCH_MAX) {
        ret(ctx, SAS_ERR_INVALID_PITCH);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.voices[voice].pitch = pitch;
    ret(ctx, 0);
}

void hle_sceSasSetKeyOn(uint8_t* rdram, recomp_context* ctx) {
    int voice = ctx->r[5];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice& v = g_sas.voices[voice];
    if (v.paused || v.on) {
        ret(ctx, SAS_ERR_VOICE_PAUSED);
        return;
    }
    v.KeyOn();
    ret(ctx, 0);
}

// Key-off can also start sounds that only sound during the Release phase.
void hle_sceSasSetKeyOff(uint8_t* rdram, recomp_context* ctx) {
    int voice = ctx->r[5];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice& v = g_sas.voices[voice];
    if (v.paused || !v.on) {
        ret(ctx, SAS_ERR_VOICE_PAUSED);  // this is ok
        return;
    }
    v.KeyOff();
    ret(ctx, 0);
}

void hle_sceSasSetNoise(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, freq=a2)
    int voice = ctx->r[5];
    int freq = ctx->r[6];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    if (freq < 0 || freq >= 64) {
        ret(ctx, SAS_ERR_INVALID_NOISE_FREQ);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    SasVoice& v = g_sas.voices[voice];
    v.type = VOICETYPE_NOISE;
    v.noiseFreq = freq;
    ret(ctx, 0);
}

void hle_sceSasSetSL(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, level=a2)
    int voice = ctx->r[5];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.voices[voice].envelope.SetSustainLevel(ctx->r[6]);
    ret(ctx, 0);
}

void hle_sceSasSetADSR(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, flag=a2, a=a3, d=t0, s=t1, r=t2)
    int voice = ctx->r[5];
    int flag = ctx->r[6];
    int a = ctx->r[7], d = ctx->r[8], s = ctx->r[9], r = ctx->r[10];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    // A flag-like mask of the invalid (negative) values.
    int invalid = (a < 0 ? 0x1 : 0) | (d < 0 ? 0x2 : 0) |
                  (s < 0 ? 0x4 : 0) | (r < 0 ? 0x8 : 0);
    if (invalid & flag) {
        ret(ctx, SAS_ERR_INVALID_ADSR_RATE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.voices[voice].envelope.SetRate(flag, a, d, s, r);
    ret(ctx, 0);
}

void hle_sceSasSetADSRmode(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, flag=a2, a=a3, d=t0, s=t1, r=t2)
    int voice = ctx->r[5];
    int flag = ctx->r[6];
    int a = ctx->r[7], d = ctx->r[8], s = ctx->r[9], r = ctx->r[10];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    // Probably by accident, the PSP ignores the top bit of these values.
    a &= 0x7FFFFFFF;
    d &= 0x7FFFFFFF;
    s &= 0x7FFFFFFF;
    r &= 0x7FFFFFFF;

    // This will look like the update flag for the invalid modes.
    int invalid = 0;
    if (a > 5 || (a & 1) != 0) invalid |= 0x1;
    if (d > 5 || (d & 1) != 1) invalid |= 0x2;
    if (s > 5) invalid |= 0x4;
    if (r > 5 || (r & 1) != 1) invalid |= 0x8;
    if (invalid & flag) {
        // Some games do 5,5,5,5 right at init; it fails even on a PSP.
        ret(ctx, SAS_ERR_INVALID_ADSR_CURVE_MODE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.voices[voice].envelope.SetEnvelope(flag, a, d, s, r);
    ret(ctx, 0);
}

void hle_sceSasSetSimpleADSR(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, voice=a1, env1=a2, env2=a3)
    int voice = ctx->r[5];
    uint32_t env1 = static_cast<uint32_t>(ctx->r[6]);
    uint32_t env2 = static_cast<uint32_t>(ctx->r[7]);
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    // This bit gives an error if you try to set it.
    if ((env2 >> 13) & 1) {
        ret(ctx, SAS_ERR_INVALID_ADSR_CURVE_MODE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.voices[voice].envelope.SetSimpleEnvelope(env1 & 0xFFFF,
                                                   env2 & 0xFFFF);
    ret(ctx, 0);
}

void hle_sceSasGetEnvelopeHeight(uint8_t* rdram, recomp_context* ctx) {
    int voice = ctx->r[5];
    (void)rdram;
    if (!voice_ok(voice)) {
        ret(ctx, SAS_ERR_INVALID_VOICE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    ret(ctx, g_sas.voices[voice].envelope.GetHeight());
}

void hle_sceSasGetAllEnvelopeHeights(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, heightsPtr=a1) -- write 32 ints.
    uint32_t ptr = static_cast<uint32_t>(ctx->r[5]);
    if (!valid_addr(ptr)) {
        ret(ctx, SAS_ERR_INVALID_PARAMETER);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    bind_mem(rdram);
    uint8_t* dst = g_sas.memory().range(ptr, PSP_SAS_VOICES_MAX * 4U);
    if (dst != nullptr) {
        for (int i = 0; i < PSP_SAS_VOICES_MAX; i++) {
            int32_t h = g_sas.voices[i].envelope.GetHeight();
            std::memcpy(dst + i * 4, &h, sizeof(h));
        }
    }
    ret(ctx, 0);
}

void hle_sceSasRevType(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, type=a1)
    int type = ctx->r[5];
    (void)rdram;
    if (type < PSP_SAS_EFFECT_TYPE_OFF || type > PSP_SAS_EFFECT_TYPE_MAX) {
        ret(ctx, SAS_ERR_REV_INVALID_TYPE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.SetWaveformEffectType(type);
    ret(ctx, 0);
}

void hle_sceSasRevParam(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, delay=a1, feedback=a2)
    int delay = ctx->r[5];
    int feedback = ctx->r[6];
    (void)rdram;
    if (delay < 0 || delay >= 128) {
        ret(ctx, SAS_ERR_REV_INVALID_DELAY);
        return;
    }
    if (feedback < 0 || feedback >= 128) {
        ret(ctx, SAS_ERR_REV_INVALID_FEEDBACK);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.SetWaveformEffectParams(delay, feedback);
    ret(ctx, 0);
}

void hle_sceSasRevEVOL(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, leftVol=a1, rightVol=a2)
    uint32_t lv = static_cast<uint32_t>(ctx->r[5]);
    uint32_t rv = static_cast<uint32_t>(ctx->r[6]);
    (void)rdram;
    if (lv > 0x1000 || rv > 0x1000) {
        ret(ctx, SAS_ERR_REV_INVALID_VOLUME);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.waveformEffect.leftVol = static_cast<int>(lv);
    g_sas.waveformEffect.rightVol = static_cast<int>(rv);
    ret(ctx, 0);
}

void hle_sceSasRevVON(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, dry=a1, wet=a2)
    (void)rdram;
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.waveformEffect.isDryOn = ctx->r[5] != 0;
    g_sas.waveformEffect.isWetOn = ctx->r[6] != 0;
    ret(ctx, 0);
}

void hle_sceSasGetGrain(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    ret(ctx, g_sas.GetGrainSize());
    (void)rdram;
}

void hle_sceSasSetGrain(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, grain=a1) -- same bounds as __sceSasInit.
    int grain = ctx->r[5];
    (void)rdram;
    if (grain < 0x40 || grain > 0x800 || (grain & 0x1F) != 0) {
        ret(ctx, SAS_ERR_INVALID_GRAIN);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.SetGrainSize(grain);
    ret(ctx, 0);
}

void hle_sceSasGetOutputmode(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    ret(ctx, g_sas.outputMode);
    (void)rdram;
}

void hle_sceSasSetOutputmode(uint8_t* rdram, recomp_context* ctx) {
    // (core=a0, outputMode=a1)
    uint32_t mode = static_cast<uint32_t>(ctx->r[5]);
    (void)rdram;
    if (mode != 0 && mode != 1) {
        ret(ctx, SAS_ERR_INVALID_OUTPUT_MODE);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sas_mtx);
    g_sas.outputMode = static_cast<int>(mode);
    ret(ctx, 0);
}

}  // namespace

// ---- Registration ----

void psp_hle_register_sas() {
    psp_hle_register("__sceSasInit", hle_sceSasInit);
    psp_hle_register("__sceSasCore", hle_sceSasCore);
    psp_hle_register("__sceSasCoreWithMix", hle_sceSasCoreWithMix);
    psp_hle_register("__sceSasSetVoice", hle_sceSasSetVoice);
    psp_hle_register("__sceSasSetVoicePCM", hle_sceSasSetVoicePCM);
    psp_hle_register("__sceSasSetPitch", hle_sceSasSetPitch);
    psp_hle_register("__sceSasSetKeyOn", hle_sceSasSetKeyOn);
    psp_hle_register("__sceSasSetKeyOff", hle_sceSasSetKeyOff);
    psp_hle_register("__sceSasSetPause", hle_sceSasSetPause);
    psp_hle_register("__sceSasSetGrain", hle_sceSasSetGrain);
    psp_hle_register("__sceSasGetGrain", hle_sceSasGetGrain);
    psp_hle_register("__sceSasGetEndFlag", hle_sceSasGetEndFlag);
    psp_hle_register("__sceSasGetPauseFlag", hle_sceSasGetPauseFlag);
    psp_hle_register("__sceSasGetEnvelopeHeight",
                      hle_sceSasGetEnvelopeHeight);
    psp_hle_register("__sceSasGetAllEnvelopeHeights",
                      hle_sceSasGetAllEnvelopeHeights);
    psp_hle_register("__sceSasSetVolume", hle_sceSasSetVolume);
    psp_hle_register("__sceSasSetADSR", hle_sceSasSetADSR);
    psp_hle_register("__sceSasSetADSRmode", hle_sceSasSetADSRmode);
    psp_hle_register("__sceSasSetSL", hle_sceSasSetSL);
    psp_hle_register("__sceSasSetSimpleADSR", hle_sceSasSetSimpleADSR);
    psp_hle_register("__sceSasSetNoise", hle_sceSasSetNoise);
    psp_hle_register("__sceSasRevParam", hle_sceSasRevParam);
    psp_hle_register("__sceSasRevType", hle_sceSasRevType);
    psp_hle_register("__sceSasRevEVOL", hle_sceSasRevEVOL);
    psp_hle_register("__sceSasRevVON", hle_sceSasRevVON);
    psp_hle_register("__sceSasGetOutputmode", hle_sceSasGetOutputmode);
    psp_hle_register("__sceSasSetOutputmode", hle_sceSasSetOutputmode);
}
