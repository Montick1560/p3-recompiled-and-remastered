// Unit tests for the SAS synthesis core (psp_sas_core.cpp): VAG block decode,
// ADSR progression, a keyed-on VAG voice producing audio then ending, and
// bad guest pointers producing silence. Standalone: no SDL, no game files.

#include "hle/psp_sas_core.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace psp_sas;

static int failures = 0;
static int tests_run = 0;

#define ASSERT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        if ((actual) != (expected)) { \
            std::fprintf(stderr, "FAIL: %s: got %d, expected %d\n", msg, \
                static_cast<int>(actual), static_cast<int>(expected)); \
            failures++; \
        } \
    } while (0)

#define ASSERT_TRUE(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s\n", msg); \
            failures++; \
        } \
    } while (0)

// Small fake guest RAM: the core masks addresses with 0x07FFFFFF, so guest
// address 0x08000100 lands at offset 0x100 here.
static constexpr size_t kMemSize = 0x10000;
static constexpr uint32_t kBase = 0x08000000u;

// Fills a 16-byte VAG block: predictor/shift byte, flags, 14 data bytes
// (two 4-bit samples each, low nibble first).
static void make_block(uint8_t* b, int predict, int shift, int flags,
                       const uint8_t* data14) {
    b[0] = static_cast<uint8_t>((predict << 4) | shift);
    b[1] = static_cast<uint8_t>(flags);
    std::memcpy(b + 2, data14, 14);
}

static void decode_all(VagDecoder& d, const GuestMem& mem, int16_t* out,
                       int n) {
    d.GetSamples(mem, out, n);
}

static void test_vag_decode_predictor0() {
    std::vector<uint8_t> ram(kMemSize, 0);
    GuestMem mem{ram.data(), ram.size()};
    // Predictor 0 (no history), shift 8: sample = (nibble << 12 as s16) >> 8.
    // Nibbles 1..7 -> 16*n, 8 -> -128, 9 -> -112, 0xF -> -16.
    uint8_t data[14] = {0x21, 0x43, 0x65, 0x87, 0xF9, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    make_block(&ram[0x100], 0, 8, 0, data);
    VagDecoder d;
    d.Start(kBase + 0x100, 16, false);
    int16_t out[28];
    decode_all(d, mem, out, 28);
    const int16_t want[10] = {16, 32, 48, 64, 80, 96, 112, -128, -112, -16};
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(out[i], want[i], "predictor0 sample");
    }
    ASSERT_EQ(out[10], 0, "zero nibble decodes to 0");
    ASSERT_TRUE(!d.End(), "block data alone does not set end before overrun");
}

static void test_vag_decode_predictor1_history() {
    std::vector<uint8_t> ram(kMemSize, 0);
    GuestMem mem{ram.data(), ram.size()};
    // Predictor 1 = coefficients {60, 0}, shift 8, every nibble 1 (-> 16).
    // y[n] = 16 + ((y[n-1] * 60) >> 6):
    //   16, 16+(16*60>>6=15)=31, 16+(31*60>>6=29)=45, 16+(45*60>>6=42)=58,
    //   16+(58*60>>6=54)=70, 16+(70*60>>6=65)=81
    uint8_t data[14];
    std::memset(data, 0x11, sizeof(data));
    make_block(&ram[0x200], 1, 8, 0, data);
    VagDecoder d;
    d.Start(kBase + 0x200, 16, false);
    int16_t out[28];
    decode_all(d, mem, out, 28);
    const int16_t want[6] = {16, 31, 45, 58, 70, 81};
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(out[i], want[i], "predictor1 sample");
    }
}

static void test_vag_end_flag_and_data_end() {
    std::vector<uint8_t> ram(kMemSize, 0);
    GuestMem mem{ram.data(), ram.size()};
    uint8_t data[14];
    std::memset(data, 0x44, sizeof(data));
    make_block(&ram[0x300], 0, 4, 0, data);       // block 0: normal
    make_block(&ram[0x310], 0, 4, 7, data);       // block 1: end marker
    VagDecoder d;
    d.Start(kBase + 0x300, 32, false);
    int16_t out[56];
    std::memset(out, 0x55, sizeof(out));
    decode_all(d, mem, out, 56);
    ASSERT_EQ(out[0], 1024, "block 0 decoded (4<<12>>4)");
    ASSERT_EQ(out[27], 1024, "block 0 last sample");
    ASSERT_EQ(out[28], 0, "flag 7 block is silent");
    ASSERT_EQ(out[55], 0, "tail after end is silent");
    ASSERT_TRUE(d.End(), "decoder ended on flag 7");
}

static void test_vag_loop() {
    std::vector<uint8_t> ram(kMemSize, 0);
    GuestMem mem{ram.data(), ram.size()};
    uint8_t a[14], b[14];
    std::memset(a, 0x11, sizeof(a));              // shift 8 -> 16
    std::memset(b, 0x22, sizeof(b));              // shift 8 -> 32
    make_block(&ram[0x400], 0, 8, 6, a);          // loop start
    make_block(&ram[0x410], 0, 8, 3, b);          // loop end (jump back)
    VagDecoder d;
    d.Start(kBase + 0x400, 32, true);
    int16_t out[28 * 4];
    decode_all(d, mem, out, 28 * 4);
    ASSERT_EQ(out[0], 16, "first pass block 0");
    ASSERT_EQ(out[28], 32, "first pass block 1");
    ASSERT_EQ(out[56], 16, "looped back to block 0");
    ASSERT_EQ(out[84], 32, "looped block 1 again");
    ASSERT_TRUE(!d.End(), "looping voice never ends");
}

static void test_vag_bad_address_silent() {
    std::vector<uint8_t> ram(kMemSize, 0x77);
    GuestMem mem{ram.data(), ram.size()};
    VagDecoder d;
    d.Start(0x0BFFFFF0u, 64, false);               // beyond our RAM
    int16_t out[40];
    std::memset(out, 0x55, sizeof(out));
    d.GetSamples(mem, out, 40);
    bool silent = true;
    for (int16_t s : out) silent = silent && (s == 0);
    ASSERT_TRUE(silent, "out-of-range VAG is silent");
    ASSERT_TRUE(d.End(), "out-of-range VAG ends");
}

static void test_adsr_progression() {
    ADSREnvelope e;
    e.attackRate = 0x10000000;
    e.attackType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE;
    e.decayRate = 0x10000000;
    e.decayType = PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
    e.sustainLevel = 0x10000000;
    e.sustainRate = 0;
    e.sustainType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
    e.releaseRate = 0x08000000;
    e.releaseType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;

    ASSERT_TRUE(e.HasEnded(), "starts off");
    e.KeyOn();
    ASSERT_TRUE(e.NeedsKeyOn(), "keyed on, waiting for first mix");

    // The first ~32 steps are the key-on warm-up: height stays ~0.
    for (int i = 0; i < 31; i++) e.Step();
    ASSERT_TRUE(e.GetHeight() < 64, "key-on warm-up keeps height at ~0");

    // Attack: strictly rising until it saturates at the maximum.
    int prev = e.GetHeight();
    bool rising = true;
    int guard = 0;
    int attack_steps = 0;
    while (e.GetState() != ADSREnvelope::STATE_DECAY && guard++ < 100) {
        ADSREnvelope::ADSRState before = e.GetState();
        e.Step();
        int h = e.GetHeight();
        // The step that enters ATTACK resets the height to 0; only compare
        // steps that were themselves taken in ATTACK.
        if (before == ADSREnvelope::STATE_ATTACK) {
            rising = rising && h > prev;
            attack_steps++;
        }
        prev = h;
    }
    ASSERT_TRUE(attack_steps >= 2, "attack lasts several steps");
    ASSERT_TRUE(rising, "attack height increases monotonically");
    ASSERT_EQ(e.GetState(), ADSREnvelope::STATE_DECAY, "attack -> decay");
    ASSERT_EQ(e.GetHeight(), PSP_SAS_ENVELOPE_HEIGHT_MAX,
              "attack reached max height");

    // Decay: falling until it crosses the sustain level.
    prev = e.GetHeight();
    bool falling = true;
    guard = 0;
    while (e.GetState() == ADSREnvelope::STATE_DECAY && guard++ < 1000) {
        e.Step();
        if (e.GetState() == ADSREnvelope::STATE_DECAY) {
            falling = falling && e.GetHeight() <= prev;
            prev = e.GetHeight();
        }
    }
    ASSERT_TRUE(falling, "decay height does not increase");
    ASSERT_EQ(e.GetState(), ADSREnvelope::STATE_SUSTAIN, "decay -> sustain");
    ASSERT_TRUE(e.GetHeight() < 0x10000000, "sustain starts below level");

    // Sustain with rate 0 holds the level.
    int held = e.GetHeight();
    for (int i = 0; i < 50; i++) e.Step();
    ASSERT_EQ(e.GetHeight(), held, "sustain holds level");
    ASSERT_EQ(e.GetState(), ADSREnvelope::STATE_SUSTAIN, "still sustaining");

    // Key-off: release to zero, then the envelope has ended.
    e.KeyOff();
    ASSERT_EQ(e.GetState(), ADSREnvelope::STATE_RELEASE, "key-off -> release");
    prev = e.GetHeight();
    bool releasing = true;
    guard = 0;
    while (!e.HasEnded() && guard++ < 100) {
        e.Step();
        releasing = releasing && e.GetHeight() <= prev;
        prev = e.GetHeight();
    }
    ASSERT_TRUE(releasing, "release height decreases");
    ASSERT_TRUE(e.HasEnded(), "release finishes");
    ASSERT_EQ(e.GetHeight(), 0, "released to zero");
}

static void test_simple_adsr_decode() {
    ADSREnvelope e;
    // Attack field n=0x7F -> rate 0, bit15 set -> LINEAR_BENT attack.
    e.SetSimpleEnvelope(0xFF00 | 0x00, 0x0000);
    ASSERT_EQ(e.attackRate, 0, "attack n=0x7F is rate 0");
    ASSERT_EQ(e.attackType, PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT,
              "attack bit 15 selects bent curve");
    // Attack n=0 -> ((7 - 0) << 26) >> 0.
    e.SetSimpleEnvelope(0x0000, 0x0000);
    ASSERT_EQ(e.attackRate, 7 << 26, "attack n=0 rate");
    ASSERT_EQ(e.attackType, PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE,
              "attack linear");
    ASSERT_EQ(e.sustainLevel, 1 << 26, "sustain level 0 -> 1<<26");
}

// Builds a looping-free VAG of `blocks` blocks of constant value in guest
// RAM and returns its guest address.
static uint32_t put_const_vag(std::vector<uint8_t>& ram, uint32_t off,
                              int blocks, uint8_t nibblePair) {
    uint8_t data[14];
    std::memset(data, nibblePair, sizeof(data));
    for (int i = 0; i < blocks; i++) {
        make_block(&ram[off + i * 16], 0, 4, 0, data);
    }
    return kBase + off;
}

static void setup_voice(SasInstance& sas, int v, uint32_t addr, int size) {
    sas.voices[v].SetVag(addr, size, false);
    ADSREnvelope& e = sas.voices[v].envelope;
    e.attackRate = 0x7FFFFFFF;
    e.attackType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE;
    e.decayRate = 0;
    e.decayType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
    e.sustainLevel = 0x40000000;
    e.sustainRate = 0;
    e.sustainType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
    e.releaseRate = 0x40000000;
    e.releaseType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
}

static void test_voice_plays_then_ends() {
    std::vector<uint8_t> ram(kMemSize, 0);
    SasInstance sas;
    sas.SetMemory(ram.data(), ram.size());
    sas.SetGrainSize(64);
    // 2 blocks = 56 samples of constant 1024 (nibble 4, shift 4).
    uint32_t addr = put_const_vag(ram, 0x1000, 2, 0x44);
    setup_voice(sas, 0, addr, 32);
    ASSERT_EQ(sas.GetEndFlag(), 0xFFFFFFFFu, "all voices idle before key-on");
    sas.voices[0].KeyOn();
    ASSERT_EQ(sas.GetEndFlag(), 0xFFFFFFFEu, "voice 0 playing after key-on");

    const uint32_t out = kBase + 0x2000;
    bool ok = sas.Mix(out, 0, 0, 0);
    ASSERT_TRUE(ok, "mix to valid buffer");
    const int16_t* o = reinterpret_cast<const int16_t*>(&ram[0x2000]);
    long energy = 0;
    for (int i = 0; i < 64 * 2; i++) energy += o[i] < 0 ? -o[i] : o[i];
    ASSERT_TRUE(energy > 0, "keyed-on VAG voice produces non-zero output");
    ASSERT_EQ(o[0], 0, "first samples after key-on are the 0 warm-up");
    ASSERT_EQ(o[0], o[1], "centered voice: L == R");

    // Keep mixing: 56 samples of data are consumed within a few grains.
    int grains = 1;
    while ((sas.GetEndFlag() & 1u) == 0 && grains < 20) {
        sas.Mix(out, 0, 0, 0);
        grains++;
    }
    ASSERT_TRUE((sas.GetEndFlag() & 1u) != 0, "end flag set after data ends");
    ASSERT_TRUE(grains > 1, "voice outlives the first grain (data not yet consumed)");
    ASSERT_TRUE(grains <= 3, "voice ends once its 56 samples are consumed");

    // Ended voice mixes to silence.
    sas.Mix(out, 0, 0, 0);
    energy = 0;
    for (int i = 0; i < 64 * 2; i++) energy += o[i] < 0 ? -o[i] : o[i];
    ASSERT_EQ(energy, 0, "ended voice is silent");
}

static void test_mix_with_input_and_volume() {
    std::vector<uint8_t> ram(kMemSize, 0);
    SasInstance sas;
    sas.SetMemory(ram.data(), ram.size());
    sas.SetGrainSize(64);
    // Existing buffer content to be mixed with: constant 1000 both sides.
    int16_t* buf = reinterpret_cast<int16_t*>(&ram[0x3000]);
    for (int i = 0; i < 128; i++) buf[i] = 1000;
    // No voices playing; CoreWithMix scales input by volume/0x1000.
    const uint32_t addr = kBase + 0x3000;
    bool ok = sas.Mix(addr, addr, 0x800, 0x1000);
    ASSERT_TRUE(ok, "in-place mix ok");
    ASSERT_EQ(buf[0], 500, "left input at half volume");
    ASSERT_EQ(buf[1], 1000, "right input at unity");
    ASSERT_EQ(buf[126], 500, "last frame left");
    ASSERT_EQ(buf[127], 1000, "last frame right");
}

static void test_pitch_octave_consumes_double() {
    std::vector<uint8_t> ram(kMemSize, 0);
    SasInstance sas;
    sas.SetMemory(ram.data(), ram.size());
    sas.SetGrainSize(64);
    // 4 blocks = 112 samples. At pitch 0x2000 a grain consumes ~2x faster,
    // so the voice ends sooner than at pitch 0x1000.
    uint32_t addr = put_const_vag(ram, 0x1000, 4, 0x44);
    int grains[2];
    const int pitches[2] = {0x1000, 0x2000};
    for (int k = 0; k < 2; k++) {
        setup_voice(sas, k, addr, 64);
        sas.voices[k].pitch = pitches[k];
        sas.voices[k].KeyOn();
        // Isolate this voice: pause the other.
        sas.voices[1 - k].paused = true;
        int g = 0;
        while ((sas.GetEndFlag() & (1u << k)) == 0 && g < 50) {
            sas.Mix(kBase + 0x2000, 0, 0, 0);
            g++;
        }
        grains[k] = g;
        sas.voices[1 - k].paused = false;
    }
    ASSERT_TRUE(grains[1] < grains[0], "double pitch ends sooner");
}

static void test_bad_vag_address_no_crash() {
    std::vector<uint8_t> ram(kMemSize, 0x66);
    SasInstance sas;
    sas.SetMemory(ram.data(), ram.size());
    sas.SetGrainSize(64);
    setup_voice(sas, 3, 0x0BFFFF00u, 4096);       // far outside our RAM
    sas.voices[3].KeyOn();
    const uint32_t out = kBase + 0x2000;
    std::memset(&ram[0x2000], 0x55, 64 * 4);
    bool ok = sas.Mix(out, 0, 0, 0);
    ASSERT_TRUE(ok, "mix with bad VAG pointer does not fail");
    const int16_t* o = reinterpret_cast<const int16_t*>(&ram[0x2000]);
    long energy = 0;
    for (int i = 0; i < 64 * 2; i++) energy += o[i] < 0 ? -o[i] : o[i];
    ASSERT_EQ(energy, 0, "bad VAG pointer yields silence");
    sas.Mix(out, 0, 0, 0);
    ASSERT_TRUE((sas.GetEndFlag() & (1u << 3)) != 0,
                "voice with unreadable data reports ended");
}

static void test_bad_output_address_no_crash() {
    std::vector<uint8_t> ram(kMemSize, 0);
    SasInstance sas;
    sas.SetMemory(ram.data(), ram.size());
    sas.SetGrainSize(64);
    uint32_t addr = put_const_vag(ram, 0x1000, 2, 0x44);
    setup_voice(sas, 0, addr, 32);
    sas.voices[0].KeyOn();
    bool ok = sas.Mix(0x0BFFFF00u, 0, 0, 0);       // output outside RAM
    ASSERT_TRUE(!ok, "out-of-range output reports failure");
    // Also a null memory base is tolerated.
    SasInstance bare;
    bare.SetGrainSize(64);
    bool ok2 = bare.Mix(kBase, 0, 0, 0);
    ASSERT_TRUE(!ok2, "no guest memory reports failure");
}

static void test_pcm_voice_and_wet_mix() {
    std::vector<uint8_t> ram(kMemSize, 0);
    SasInstance sas;
    sas.SetMemory(ram.data(), ram.size());
    sas.SetGrainSize(128);
    int16_t* pcm = reinterpret_cast<int16_t*>(&ram[0x1000]);
    for (int i = 0; i < 100; i++) pcm[i] = 8000;
    setup_voice(sas, 1, 0, 0);
    sas.voices[1].SetPcm(kBase + 0x1000, 100, -1);
    sas.voices[1].KeyOn();
    // Wet only, reverb off: the send bus is replicated at the return volume.
    sas.waveformEffect.isDryOn = 0;
    sas.waveformEffect.isWetOn = 1;
    sas.waveformEffect.leftVol = 0x1000;
    sas.waveformEffect.rightVol = 0x1000;
    const uint32_t out = kBase + 0x2000;
    bool ok = sas.Mix(out, 0, 0, 0);
    ASSERT_TRUE(ok, "wet mix ok");
    const int16_t* o = reinterpret_cast<const int16_t*>(&ram[0x2000]);
    long energy = 0;
    for (int i = 0; i < 128 * 2; i++) energy += o[i] < 0 ? -o[i] : o[i];
    ASSERT_TRUE(energy > 0, "PCM voice reaches the wet bus");
    // Dry only -> same voice (fresh) is audible on the dry bus too.
    SasInstance dry;
    dry.SetMemory(ram.data(), ram.size());
    dry.SetGrainSize(128);
    setup_voice(dry, 1, 0, 0);
    dry.voices[1].SetPcm(kBase + 0x1000, 60, -1);
    dry.voices[1].KeyOn();
    dry.Mix(out, 0, 0, 0);
    energy = 0;
    for (int i = 0; i < 128 * 2; i++) energy += o[i] < 0 ? -o[i] : o[i];
    ASSERT_TRUE(energy > 0, "PCM voice on the dry bus");
    // 60 PCM samples fit in one grain after the 32-sample key-on delay.
    ASSERT_TRUE((dry.GetEndFlag() & 2u) != 0, "PCM voice ends after its data");
    // Reverb preset processing stays bounded and does not crash.
    SasInstance rv;
    rv.SetMemory(ram.data(), ram.size());
    rv.SetGrainSize(128);
    rv.SetWaveformEffectType(PSP_SAS_EFFECT_TYPE_HALL);
    rv.waveformEffect.isWetOn = 1;
    rv.waveformEffect.leftVol = rv.waveformEffect.rightVol = 0x1000;
    setup_voice(rv, 0, 0, 0);
    rv.voices[0].SetPcm(kBase + 0x1000, 100, -1);
    rv.voices[0].KeyOn();
    for (int g = 0; g < 8; g++) ok = rv.Mix(out, 0, 0, 0) && ok;
    ASSERT_TRUE(ok, "reverb mix runs");
}

static void test_raw_output_mode() {
    std::vector<uint8_t> ram(kMemSize, 0);
    SasInstance sas;
    sas.SetMemory(ram.data(), ram.size());
    sas.SetGrainSize(64);
    sas.outputMode = PSP_SAS_OUTPUTMODE_RAW;
    bool ok = sas.Mix(kBase + 0x2000, 0, 0, 0);
    ASSERT_TRUE(ok, "raw mode mix ok");
    // Raw mode needs 4 planes (8 * grain bytes): a buffer that only fits the
    // mixed layout is rejected, not overrun.
    bool bad = sas.Mix(kBase + static_cast<uint32_t>(kMemSize) - 64 * 4, 0, 0, 0);
    ASSERT_TRUE(!bad, "raw mode checks the full 4-plane range");
}

int main() {
    test_vag_decode_predictor0();
    test_vag_decode_predictor1_history();
    test_vag_end_flag_and_data_end();
    test_vag_loop();
    test_vag_bad_address_silent();
    test_adsr_progression();
    test_simple_adsr_decode();
    test_voice_plays_then_ends();
    test_mix_with_input_and_volume();
    test_pitch_octave_consumes_double();
    test_bad_vag_address_no_crash();
    test_bad_output_address_no_crash();
    test_pcm_voice_and_wet_mix();
    test_raw_output_mode();
    std::printf("%d tests run, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
