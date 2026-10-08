// ATRAC3/ATRAC3plus playback context (the state behind one sceAtrac ID).
//
// Ported from PPSSPP (GPL-2.0-or-later), Core/HLE/AtracCtx2.{h,cpp} and
// Core/HLE/AtracBase.h, with the RIFF/WAVE header analysis written to match
// PPSSPP's AnalyzeAtracTrack/ParseWaveAT3 behaviour. Copyright (c) 2012-
// PPSSPP Project.
//
// This is a *pure* context: no HLE framework, no scheduler, no logging.
// All guest memory is reached through GuestMem, which masks addresses with
// 0x07FFFFFF and bounds-checks every access, so a bad guest pointer yields an
// error code and never a host fault. The sceAtrac* HLE glue lives in
// hle/psp_hle_atrac.cpp.
//
// Simplified relative to PPSSPP: the context state is held host-side in
// IdInfo (a mirror of SceAtracIdInfo) instead of inside guest memory, and the
// low-level-decode and sceSas integration modes are not ported (the game does
// not import them).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace psp_atrac {

// ---- Error codes (PPSSPP Core/HLE/ErrorCodes.h) ----
constexpr int32_t ERR_API_FAIL = (int32_t)0x80630002U;
constexpr int32_t ERR_NO_ATRACID = (int32_t)0x80630003U;
constexpr int32_t ERR_INVALID_CODECTYPE = (int32_t)0x80630004U;
constexpr int32_t ERR_BAD_ATRACID = (int32_t)0x80630005U;
constexpr int32_t ERR_UNKNOWN_FORMAT = (int32_t)0x80630006U;
constexpr int32_t ERR_WRONG_CODECTYPE = (int32_t)0x80630007U;
constexpr int32_t ERR_BAD_CODEC_PARAMS = (int32_t)0x80630008U;
constexpr int32_t ERR_ALL_DATA_LOADED = (int32_t)0x80630009U;
constexpr int32_t ERR_NO_DATA = (int32_t)0x80630010U;
constexpr int32_t ERR_SIZE_TOO_SMALL = (int32_t)0x80630011U;
constexpr int32_t ERR_SECOND_BUFFER_NEEDED = (int32_t)0x80630012U;
constexpr int32_t ERR_INCORRECT_READ_SIZE = (int32_t)0x80630013U;
constexpr int32_t ERR_BAD_ALIGNMENT = (int32_t)0x80630014U;
constexpr int32_t ERR_BAD_SAMPLE = (int32_t)0x80630015U;
constexpr int32_t ERR_BAD_FIRST_RESET_SIZE = (int32_t)0x80630016U;
constexpr int32_t ERR_BAD_SECOND_RESET_SIZE = (int32_t)0x80630017U;
constexpr int32_t ERR_ADD_DATA_IS_TOO_BIG = (int32_t)0x80630018U;
constexpr int32_t ERR_NOT_MONO = (int32_t)0x80630019U;
constexpr int32_t ERR_NO_LOOP_INFORMATION = (int32_t)0x80630021U;
constexpr int32_t ERR_SECOND_BUFFER_NOT_NEEDED = (int32_t)0x80630022U;
constexpr int32_t ERR_BUFFER_IS_EMPTY = (int32_t)0x80630023U;
constexpr int32_t ERR_ALL_DATA_DECODED = (int32_t)0x80630024U;
constexpr int32_t ERR_IS_LOW_LEVEL = (int32_t)0x80630031U;
constexpr int32_t ERR_IS_FOR_SCESAS = (int32_t)0x80630040U;
constexpr int32_t ERR_KERNEL_ILLEGAL_ADDRESS = (int32_t)0x8002006AU;
constexpr int32_t ERR_KERNEL_ILLEGAL_ADDR = (int32_t)0x800200D3U;

// ---- Codec ids / frame sizes ----
constexpr uint16_t CODEC_AT3PLUS = 0x1000;
constexpr uint16_t CODEC_AT3 = 0x1001;
constexpr uint32_t AT3_MAX_SAMPLES = 0x400;      // 1024
constexpr uint32_t AT3PLUS_MAX_SAMPLES = 0x800;  // 2048

// ---- Buffer status (the "state" field of SceAtracIdInfo) ----
enum Status : uint8_t {
    STATUS_UNINITIALIZED = 0,
    STATUS_NO_DATA = 1,
    // Entire file is in memory.
    STATUS_ALL_DATA_LOADED = 2,
    // Buffer fits the entire file but is only partially filled.
    STATUS_HALFWAY_BUFFER = 3,
    // Buffer smaller than the file; data is streamed in.
    STATUS_STREAMED_WITHOUT_LOOP = 4,
    STATUS_STREAMED_LOOP_FROM_END = 5,
    // Audio exists after the loop, so a second buffer holds the trailer.
    STATUS_STREAMED_LOOP_WITH_TRAILER = 6,
    STATUS_LOW_LEVEL = 8,
    STATUS_FOR_SCESAS = 16,
    STATUS_STREAMED_MASK = 4,
};

inline bool StatusIsStreaming(Status s) {
    return (s & STATUS_STREAMED_MASK) != 0;
}

// sceAtracGetRemainFrame "no more data needed" values.
constexpr int REMAIN_ALLDATA_IS_ON_MEMORY = -1;
constexpr int REMAIN_NONLOOP_STREAM_DATA_IS_ON_MEMORY = -2;
constexpr int REMAIN_LOOP_STREAM_DATA_IS_ON_MEMORY = -3;

// ---- Guest memory window ----
// Wraps the runtime's rdram mapping. Addresses are masked with 0x07FFFFFF
// (the runtime-wide convention) and rejected when below the NULL page or
// when the range leaves the mapping. `size` lets tests use a small buffer.
struct GuestMem {
    static constexpr uint32_t kAddrMask = 0x07FFFFFFu;
    static constexpr uint32_t kNullPage = 0x00010000u;

    uint8_t* base = nullptr;
    size_t size = 0;

    GuestMem() = default;
    GuestMem(uint8_t* b, size_t s) : base(b), size(s) {}

    bool IsValid(uint32_t addr) const {
        return base != nullptr && addr >= kNullPage &&
               (addr & kAddrMask) < size;
    }
    bool IsValidRange(uint32_t addr, uint32_t len) const {
        if (!IsValid(addr)) return false;
        return (uint64_t)(addr & kAddrMask) + len <= size;
    }
    // Valid pointer to at least `len` bytes, or nullptr.
    uint8_t* Ptr(uint32_t addr, uint32_t len = 1) const {
        return IsValidRange(addr, len) ? base + (addr & kAddrMask) : nullptr;
    }
    bool Write32(uint32_t addr, uint32_t v) const;
    bool Read32(uint32_t addr, uint32_t* v) const;
};

// ---- RIFF/WAVE header analysis ----
// What libatrac3plus gleans from the header of an .at3 file.
struct TrackInfo {
    uint16_t codec = 0;       // CODEC_AT3PLUS / CODEC_AT3
    int numChans = 0;         // fmt channel count (1 or 2)
    int blockAlign = 0;       // bytes per encoded frame ("sampleSize")
    int endSample = 0;        // fact: total sample count (0 = no fact chunk)
    int firstSampleOffset = 0;  // fact: samples preceding the first valid one
    int waveDataSize = 0;     // size of the data chunk
    int dataOff = 0;          // file offset of the first frame
    int loopStart = -1;       // smpl: first loop, -1 when absent
    int loopEnd = -1;
    bool jointStereo = false;  // ATRAC3 only (from the fmt extra data)
};

// Parses a RIFF/WAVE ATRAC3/ATRAC3plus header from `buf` (which holds `size`
// valid bytes). Returns 0 or a negative ERR_* code.
int ParseWave(const uint8_t* buf, uint32_t size, TrackInfo* out);

// ---- Decoder backend ----
// One ATRAC frame -> interleaved s16 PCM. The default implementation wraps
// the vendored at3_standalone decoder; tests may supply a fake.
class PcmDecoder {
public:
    virtual ~PcmDecoder() = default;
    // Decodes one frame of `inBytes`. `outSamples` receives the samples per
    // channel. `out` may be null (frame decoded for state only). On failure
    // `consumed` is still set to `inBytes`. Returns false on a bad frame.
    virtual bool Decode(const uint8_t* in, int inBytes, int* consumed,
                        int outChannels, int16_t* out, int* outSamples) = 0;
};

using DecoderFactory = std::unique_ptr<PcmDecoder> (*)(
    int codecType, int bytesPerFrame, int channels, bool jointStereo);

std::unique_ptr<PcmDecoder> CreateStandaloneDecoder(
    int codecType, int bytesPerFrame, int channels, bool jointStereo);

// ---- Context state (host mirror of PPSSPP's SceAtracIdInfo) ----
struct IdInfo {
    int32_t decodePos = 0;        // sample position that decodes next
    int32_t endSample = 0;        // last sample index of the track
    int32_t loopStart = 0;
    int32_t loopEnd = 0;
    int32_t firstValidSample = 0;
    uint8_t numSkipFrames = 0;    // frames decoded and discarded first
    Status state = STATUS_UNINITIALIZED;
    uint8_t curBuffer = 0;
    uint8_t numChan = 0;
    uint16_t sampleSize = 0;      // bytes per encoded frame
    uint16_t codec = 0;
    int32_t dataOff = 0;          // file offset of the first frame
    int32_t curFileOff = 0;       // file offset of the next frame to decode
    int32_t fileDataEnd = 0;      // file size
    int32_t loopNum = 0;
    int32_t streamDataByte = 0;   // buffered, not yet decoded bytes
    int32_t streamOff = 0;        // buffer offset the next decode reads
    int32_t secondStreamOff = 0;
    uint32_t buffer = 0;          // guest address of the main buffer
    uint32_t secondBuffer = 0;
    uint32_t bufferByte = 0;
    uint32_t secondBufferByte = 0;

    int SamplesPerFrame() const {
        return codec == CODEC_AT3PLUS ? (int)AT3PLUS_MAX_SAMPLES
                                      : (int)AT3_MAX_SAMPLES;
    }
    int SamplesFrameMask() const { return SamplesPerFrame() - 1; }
    int SkipSamples() const { return codec == CODEC_AT3PLUS ? 0x170 : 0x45; }
    int BitRate() const;
};

// sceAtracGetBufferInfoForResetting output (guest layout: 8 x u32).
struct ResetBufferInfo {
    struct Single {
        uint32_t writePosPtr = 0;
        uint32_t writableBytes = 0;
        uint32_t minWriteBytes = 0;
        uint32_t filePos = 0;
    };
    Single first;
    Single second;
};

class AtracCtx {
public:
    // `codecType` is CODEC_AT3PLUS or CODEC_AT3 (the ID's slot type).
    AtracCtx(GuestMem mem, int codecType, DecoderFactory factory = nullptr);
    ~AtracCtx();

    // Argument validation shared by the sceAtrac* entry points
    // (PPSSPP AtracValidateData / AtracValidateManaged).
    int ValidateData() const;
    int ValidateManaged() const;

    Status BufferState() const { return info_.state; }
    const IdInfo& Info() const { return info_; }
    int Channels() const { return info_.numChan; }
    int OutputChannels() const { return outputChannels_; }
    int SamplesPerFrame() const { return info_.SamplesPerFrame(); }
    int BytesPerFrame() const { return info_.sampleSize; }
    int CodecType() const { return info_.codec; }
    int Bitrate() const { return info_.BitRate(); }
    uint32_t InternalCodecError() const { return codecErr_; }
    bool HasSecondBuffer() const { return info_.secondBufferByte != 0; }

    // Parses the header at guest `buffer` and initialises the context.
    // Returns 0 or a negative ERR_* code (PPSSPP sceAtracSetData semantics,
    // including WRONG_CODECTYPE when the data's codec is not this ID's).
    int SetData(uint32_t buffer, uint32_t readSize, uint32_t bufferSize,
                int outputChannels);
    // Same, from an already analysed header (the *AndGetID variants).
    int SetData(const TrackInfo& track, uint32_t buffer, uint32_t readSize,
                uint32_t bufferSize, int outputChannels);
    // Header analysis from guest memory (null/invalid pointers read as zeroes
    // so the size check fires first, as in PPSSPP).
    static int Analyze(const GuestMem& mem, uint32_t buffer, uint32_t size,
                       TrackInfo* track);
    int SetSecondBuffer(uint32_t secondBuffer, uint32_t secondBufferSize);
    int GetSecondBufferInfo(uint32_t* fileOffset, uint32_t* desiredSize) const;

    // Decodes one frame into guest `outbufAddr` (0 = discard). Writes the
    // sample count / finish flag / remaining frames. Returns 0 or ERR_*.
    int DecodeData(uint32_t outbufAddr, int* samplesNum, int* finish,
                   int* remains);
    int GetNextDecodePosition(int* pos) const;
    uint32_t GetNextSamples() const;
    int RemainingFrames() const;
    int LoopStatus() const;
    int LoopNum() const { return info_.loopNum; }
    int SetLoopNum(int loopNum);
    int GetSoundSample(int* endSample, int* loopStart, int* loopEnd) const;

    void GetStreamDataInfo(uint32_t* writePtr, uint32_t* writableBytes,
                           uint32_t* readOffset) const;
    int AddStreamData(uint32_t bytesToAdd);
    // NOTE: not const; the real library skips frames here (sets *delay).
    int GetBufferInfoForResetting(ResetBufferInfo* out, int sample,
                                  bool* delay);
    int ResetPlayPosition(int sample, int bytesWrittenFirstBuf,
                          int bytesWrittenSecondBuf, bool* delay);

private:
    int DecodeInternal(uint32_t outbufAddr, int* samplesNum, int* finish);
    void GetResetBufferInfoInternal(ResetBufferInfo* out, int seekPos) const;
    int ResetPlayPositionInternal(int seekPos, int bytesWrittenFirstBuf,
                                  int bytesWrittenSecondBuf);
    int SkipFrames(int* skipCount);
    void WrapLastPacket();
    int InitFromTrack(const TrackInfo& track, uint32_t bufferAddr,
                      int readSize, int bufferSize);
    void CreateDecoder(bool jointStereo);

    GuestMem mem_;
    IdInfo info_;
    DecoderFactory factory_;
    std::unique_ptr<PcmDecoder> decoder_;
    std::vector<int16_t> decodeTemp_;
    int outputChannels_ = 2;
    uint32_t codecErr_ = 0;
};

}  // namespace psp_atrac
