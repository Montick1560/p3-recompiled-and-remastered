// ATRAC3/ATRAC3plus playback context. See hle/psp_atrac_ctx.h.
//
// Ported from PPSSPP (GPL-2.0-or-later), Core/HLE/AtracCtx2.cpp (the
// "Atrac2" context that models the firmware's SceAtracIdInfo bookkeeping),
// Core/HLE/AtracBase.h, and the PCM conversion of Core/HW/Atrac3Standalone.cpp.
// Copyright (c) 2012- PPSSPP Project. Changes: guest memory is reached through
// the bounds-checked GuestMem window instead of PPSSPP's Memory::, the
// context lives host-side, and divisions that a corrupt loop/packet size could
// turn into a host crash are guarded.

#include "hle/psp_atrac_ctx.h"

#include "at3_decoders.h"

#include <algorithm>
#include <cstring>

namespace psp_atrac {

// ---- GuestMem ----

bool GuestMem::Write32(uint32_t addr, uint32_t v) const {
    uint8_t* p = Ptr(addr, 4);
    if (!p) return false;
    std::memcpy(p, &v, 4);
    return true;
}

bool GuestMem::Read32(uint32_t addr, uint32_t* v) const {
    const uint8_t* p = Ptr(addr, 4);
    if (!p) return false;
    std::memcpy(v, p, 4);
    return true;
}

// ---- RIFF/WAVE analysis ----

static uint32_t le16(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int ParseWave(const uint8_t* buf, uint32_t size, TrackInfo* out) {
    *out = TrackInfo();
    // 72 is about the size of the minimum required data to even be valid.
    if (size < 72) {
        return ERR_SIZE_TOO_SMALL;
    }
    if (!buf || std::memcmp(buf, "RIFF", 4) != 0 ||
        std::memcmp(buf + 8, "WAVE", 4) != 0) {
        return ERR_UNKNOWN_FORMAT;
    }

    bool haveFmt = false;
    bool haveData = false;
    uint64_t offset = 12;
    while (offset + 8 <= size) {
        const uint8_t* tag = buf + offset;
        const uint32_t chunkSize = le32(buf + offset + 4);
        const uint64_t body = offset + 8;
        const bool isData = std::memcmp(tag, "data", 4) == 0;
        if (isData) {
            if (body > 0x7FFFFFFFull) {
                return ERR_UNKNOWN_FORMAT;
            }
            out->dataOff = (int)body;
            // Streamed files may carry a size larger than what is buffered;
            // only keep it representable alongside dataOff.
            uint64_t maxSize = 0x7FFFFFFFull - body;
            out->waveDataSize = (int)std::min<uint64_t>(chunkSize, maxSize);
            haveData = true;
            break;
        }
        // Every other chunk has to be fully present to be understood (and to
        // reach the data chunk behind it).
        if (chunkSize > size - body) {
            return std::memcmp(tag, "fmt ", 4) == 0 ? ERR_UNKNOWN_FORMAT
                                                     : ERR_SIZE_TOO_SMALL;
        }
        const uint8_t* c = buf + body;
        if (std::memcmp(tag, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                return ERR_UNKNOWN_FORMAT;
            }
            const uint32_t formatTag = le16(c);
            if (formatTag == 0x0270) {
                out->codec = CODEC_AT3;
            } else if (formatTag == 0xFFFE) {
                out->codec = CODEC_AT3PLUS;
            } else {
                return ERR_UNKNOWN_FORMAT;
            }
            out->numChans = (int)le16(c + 2);
            if (out->numChans != 1 && out->numChans != 2) {
                return ERR_UNKNOWN_FORMAT;
            }
            out->blockAlign = (int)le16(c + 12);
            if (out->blockAlign == 0) {
                return ERR_UNKNOWN_FORMAT;
            }
            // ATRAC3: 14 bytes of extra data follow cbSize; byte 6 is the
            // coding mode (1 = joint stereo).
            if (out->codec == CODEC_AT3 && chunkSize >= 32) {
                out->jointStereo = le16(c + 24) != 0;
            }
            haveFmt = true;
        } else if (std::memcmp(tag, "fact", 4) == 0) {
            if (chunkSize >= 4) {
                out->endSample = (int)(le32(c) & 0x7FFFFFFFu);
            }
            if (chunkSize >= 8) {
                out->firstSampleOffset = (int)(le32(c + 4) & 0x7FFFFFFFu);
            }
        } else if (std::memcmp(tag, "smpl", 4) == 0) {
            // 36-byte header, then 24-byte loop records. Only the first loop
            // is used.
            if (chunkSize >= 36 + 24 && le32(c + 28) >= 1) {
                const uint32_t start = le32(c + 36 + 8);
                const uint32_t end = le32(c + 36 + 12);
                if (start <= 0x7FFFFFFFu && end <= 0x7FFFFFFFu) {
                    out->loopStart = (int)start;
                    out->loopEnd = (int)end;
                }
            }
        }
        offset = body + chunkSize + (chunkSize & 1);
    }

    if (!haveData) {
        return ERR_SIZE_TOO_SMALL;
    }
    if (!haveFmt) {
        return ERR_UNKNOWN_FORMAT;
    }
    return 0;
}

// ---- Standalone decoder backend ----

namespace {

inline int16_t clamp16(float f) {
    if (f >= 1.0f)
        return 32767;
    else if (f <= -1.0f)
        return -32767;
    else
        return (int16_t)(int)(f * 32767);
}

// Wraps the vendored at3_standalone decoder. Ported from PPSSPP's
// Atrac3Audio (Core/HW/Atrac3Standalone.cpp).
class StandaloneDecoder : public PcmDecoder {
public:
    StandaloneDecoder(int codecType, int channels, int blockAlign,
                      const uint8_t* extraData, int extraDataSize)
        : codec_(codecType), channels_(channels), blockAlign_(blockAlign) {
        if (codec_ == CODEC_AT3) {
            at3_ = atrac3_alloc(channels, &blockAlign_, extraData,
                                extraDataSize);
            if (at3_) {
                open_ = true;
            } else {
                failed_ = true;
            }
        }
        for (int i = 0; i < 2; i++) {
            buffers_[i] = new float[4096];
        }
    }

    ~StandaloneDecoder() override {
        if (at3_) atrac3_free(at3_);
        if (at3p_) atrac3p_free(at3p_);
        for (int i = 0; i < 2; i++) delete[] buffers_[i];
    }

    bool Decode(const uint8_t* in, int inBytes, int* consumed,
                int outChannels, int16_t* out, int* outSamples) override {
        if (outSamples) *outSamples = 0;
        if (consumed) *consumed = 0;
        if (!open_) {
            // ATRAC3plus opens lazily, as in PPSSPP.
            if (failed_) return false;
            if (codec_ == CODEC_AT3PLUS) {
                at3p_ = atrac3p_alloc(channels_, &blockAlign_);
                if (at3p_) {
                    open_ = true;
                } else {
                    failed_ = true;
                }
            }
            if (!open_) return false;
        }
        blockAlign_ = inBytes;
        int result;
        int nb = 0;
        if (codec_ == CODEC_AT3PLUS) {
            result = atrac3p_decode_frame(at3p_, buffers_, &nb, in, inBytes);
        } else {
            result = atrac3_decode_frame(at3_, buffers_, &nb, in, inBytes);
        }
        if (result < 0) {
            // Recover from single bad packets: report the regular packet
            // size consumed so the caller advances.
            if (consumed) *consumed = inBytes;
            if (outSamples) *outSamples = nb;
            return false;
        }
        if (consumed) *consumed = result;
        if (outSamples) *outSamples = nb;
        if (nb > 0 && out) {
            const float* left = buffers_[0];
            if (outChannels == 2) {
                const float* right = channels_ == 2 ? buffers_[1] : buffers_[0];
                for (int i = 0; i < nb; i++) {
                    out[i * 2] = clamp16(left[i]);
                    out[i * 2 + 1] = clamp16(right[i]);
                }
            } else {
                for (int i = 0; i < nb; i++) {
                    out[i] = clamp16(left[i]);
                }
            }
        }
        return true;
    }

private:
    ATRAC3PContext* at3p_ = nullptr;
    ATRAC3Context* at3_ = nullptr;
    int codec_;
    int channels_;
    int blockAlign_;
    float* buffers_[2]{};
    bool open_ = false;
    bool failed_ = false;
};

// libatrac3plus.prx's table (PPSSPP sceAudiocodec.cpp at3Params): ATRAC3
// frame size + joint-stereo flag decide the decoder's channel layout.
struct At3Param {
    uint16_t bytes;
    uint8_t channels;
    uint8_t jointStereo;
};
const At3Param kAt3Params[] = {
    {0x0180, 2, 0},
    {0x0130, 2, 0},
    {0x00C0, 2, 1},
    {0x00C0, 1, 0},
    {0x0098, 1, 0},
};

bool At3DecoderChannels(int bytesPerFrame, bool jointStereo, int* channels) {
    for (int i = (int)(sizeof(kAt3Params) / sizeof(kAt3Params[0])) - 1;
         i >= 0; i--) {
        if (kAt3Params[i].bytes == bytesPerFrame &&
            (kAt3Params[i].jointStereo != 0) == jointStereo) {
            *channels = kAt3Params[i].channels;
            return true;
        }
    }
    return false;
}

}  // namespace

std::unique_ptr<PcmDecoder> CreateStandaloneDecoder(
    int codecType, int bytesPerFrame, int channels, bool jointStereo) {
    if (codecType == CODEC_AT3) {
        int decoderChannels = channels;
        At3DecoderChannels(bytesPerFrame, jointStereo, &decoderChannels);
        // Built here rather than taken from the RIFF, so OMA works too.
        uint8_t extraData[14]{};
        extraData[0] = 1;
        extraData[3] = (uint8_t)(decoderChannels << 3);
        extraData[6] = jointStereo;
        extraData[8] = jointStereo;
        extraData[10] = 1;
        return std::unique_ptr<PcmDecoder>(new StandaloneDecoder(
            CODEC_AT3, decoderChannels, bytesPerFrame, extraData,
            (int)sizeof(extraData)));
    }
    return std::unique_ptr<PcmDecoder>(new StandaloneDecoder(
        CODEC_AT3PLUS, channels, bytesPerFrame, nullptr, 0));
}

// ---- Layout helpers (verbatim from AtracCtx2.cpp unless noted) ----

int IdInfo::BitRate() const {
    int bitrate = (sampleSize * 352800) / 1000;
    if (codec == CODEC_AT3PLUS) {
        bitrate = ((bitrate >> 11) + 8) & 0xFFFFFFF0;
    } else {
        bitrate = (bitrate + 511) >> 10;
    }
    return bitrate;
}

// Needs to support negative numbers, and to handle non-powers-of-two.
static int RoundDownToMultiple(int size, int grain) {
    if (grain <= 0) return size;  // guard: corrupt sampleSize
    return size - (size % grain);
}

static int RoundDownToMultipleWithOffset(int offset, int size, int grain) {
    if (grain <= 0) return size;
    if (size > offset) {
        return ((size - offset) / grain) * grain + offset;
    } else {
        return size;
    }
}

static int ComputeSkipFrames(const IdInfo& info, int seekPos) {
    // No idea why this is the rule, but this is the rule.
    return (seekPos & info.SamplesFrameMask()) < info.SkipSamples() ? 2 : 1;
}

static int ComputeFileOffset(const IdInfo& info, int seekPos) {
    int frameOffset = ((seekPos / info.SamplesPerFrame()) - 1) * info.sampleSize;
    if ((seekPos & info.SamplesFrameMask()) < info.SkipSamples() &&
        (frameOffset != 0)) {
        frameOffset -= info.sampleSize;
    }
    return frameOffset + info.dataOff;
}

// Unlike the above, this one need to be inclusive.
static int ComputeLoopEndFileOffset(const IdInfo& info, int seekPos) {
    return (seekPos / info.SamplesPerFrame() + 1) * info.sampleSize +
           info.dataOff;
}

static int ComputeSpaceUsed(const IdInfo& info) {
    // The odd case: If streaming from the second buffer, and we're past the
    // loop end (we're in the tail)...
    if (info.decodePos > info.loopEnd && info.curBuffer == 1) {
        int space = (int)info.secondBufferByte;
        if (info.secondStreamOff < space) {
            space = RoundDownToMultipleWithOffset(
                info.secondStreamOff, (int)info.secondBufferByte,
                info.sampleSize);
        }
        if ((info.secondStreamOff <= space) &&
            (space - info.secondStreamOff < info.streamDataByte)) {
            return info.streamDataByte - (space - info.secondStreamOff);
        }
        return 0;
    }

    // The normal case.
    return info.streamDataByte;
}

static int ComputeRemainFrameStream(const IdInfo& info) {
    if (info.streamDataByte >= info.fileDataEnd - info.curFileOff) {
        // Already done.
        return REMAIN_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
    }
    // Since we're streaming, the remaining frames are what's valid in the
    // buffer.
    return std::max(0, info.streamDataByte / info.sampleSize -
                           (int)info.numSkipFrames);
}

// This got so complicated!
static int ComputeRemainFrameLooped(const IdInfo& info) {
    const int loopStartFileOffset = ComputeFileOffset(info, info.loopStart);
    const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
    const int writeFileOff = info.curFileOff + info.streamDataByte;
    const int leftToRead = writeFileOff - loopEndFileOffset;
    const int firstPartLength = loopEndFileOffset - loopStartFileOffset;

    int remainFrames;
    if (writeFileOff <= loopEndFileOffset || firstPartLength <= 0) {
        // Simple case - just divide to find the number of frames remaining in
        // the buffer. (firstPartLength <= 0 only on a corrupt loop.)
        remainFrames = info.streamDataByte / info.sampleSize;
    } else {
        // Darn, we need to take looping into account...
        const int skipFramesAtLoopStart = ComputeSkipFrames(info, info.loopStart);
        const int secondPartLength = leftToRead % firstPartLength;
        // Sum up all the parts (the buffered, the space remaining before the
        // loop point and the space after the loop point), each divided by
        // sample size, need to take skipped frames into account.
        remainFrames =
            (loopEndFileOffset - info.curFileOff) / info.sampleSize +
            (leftToRead / firstPartLength) *
                (firstPartLength / info.sampleSize - skipFramesAtLoopStart);
        if (secondPartLength > skipFramesAtLoopStart * info.sampleSize) {
            remainFrames +=
                secondPartLength / info.sampleSize - skipFramesAtLoopStart;
        }
    }

    // Clamp to zero.
    remainFrames = std::max(0, remainFrames - (int)info.numSkipFrames);
    if (info.loopNum < 0) {
        // Infinite looping while streaming, we never return that we're done
        // reading data.
        return remainFrames;
    }

    // Additional check for distance to end of playback if we're looping a
    // finite amount of times.
    const int streamBufferEndFileOffset = info.curFileOff + info.streamDataByte;
    if (streamBufferEndFileOffset >= loopEndFileOffset && firstPartLength > 0) {
        const int numBufferedLoops =
            (streamBufferEndFileOffset - loopEndFileOffset) /
            (loopEndFileOffset - loopStartFileOffset);
        if (info.loopNum <= numBufferedLoops) {
            return REMAIN_LOOP_STREAM_DATA_IS_ON_MEMORY;
        }
    }
    return remainFrames;
}

static void InitLengthAndLoop(IdInfo* ctx, int endSample, int waveDataSize,
                              int firstSampleOffset, int loopBegin,
                              int loopEnd) {
    const int off = ctx->codec == CODEC_AT3 ? 0x45 : 0x170;
    const int blockShift = 0x100b - ctx->codec;
    const int firstValidSample = firstSampleOffset + off;
    int numSamplesInFile;
    if (endSample == 0) {
        numSamplesInFile = (waveDataSize / ctx->sampleSize) << (blockShift & 0x1f);
    } else {
        numSamplesInFile = endSample + firstValidSample;
    }
    ctx->decodePos = firstValidSample;
    ctx->loopNum = 0;
    ctx->endSample = numSamplesInFile - 1;
    ctx->numSkipFrames = (uint8_t)(firstValidSample >> (blockShift & 0x1f));
    if (-1 < loopBegin) {
        ctx->loopEnd = loopEnd + off;
        ctx->loopStart = loopBegin + off;
        return;
    }
    ctx->loopEnd = 0;
    ctx->loopStart = 0;
}

static int ComputeAtracStateAndInitSecondBuffer(IdInfo* info, uint32_t readSize,
                                                uint32_t bufferSize) {
    Status state;
    int loopEndFileOffset;
    int loopEnd;

    if (bufferSize < (uint32_t)info->fileDataEnd) {
        if (info->streamDataByte < (int32_t)info->sampleSize * 2) {
            // sampleSize * 3 would be more accurate, but PPSSPP increases
            // tolerance for GTA LCS custom music (#20692).
            return ERR_SIZE_TOO_SMALL;
        }
        loopEnd = info->loopEnd;
        state = STATUS_STREAMED_WITHOUT_LOOP;
        if (loopEnd != 0) {
            state = STATUS_STREAMED_LOOP_FROM_END;
            if (loopEnd != info->endSample) {
                loopEndFileOffset = ComputeLoopEndFileOffset(*info, loopEnd);
                loopEnd = (loopEndFileOffset - info->dataOff) + 1;
                info->state = STATUS_STREAMED_LOOP_WITH_TRAILER;
                if (loopEnd < info->streamDataByte) {
                    info->streamDataByte = loopEnd;
                }
                info->secondStreamOff = 0;
                info->secondBuffer = 0;
                info->secondBufferByte = 0;
                return 0;
            }
        }
    } else {
        state = STATUS_HALFWAY_BUFFER;
        if (readSize >= (uint32_t)info->fileDataEnd) {
            state = STATUS_ALL_DATA_LOADED;
        }
    }
    info->state = state;
    return 0;
}

// ---- AtracCtx ----

AtracCtx::AtracCtx(GuestMem mem, int codecType, DecoderFactory factory)
    : mem_(mem), factory_(factory) {
    info_.codec = (uint16_t)codecType;
    info_.state = STATUS_NO_DATA;
    info_.curBuffer = 0;
}

AtracCtx::~AtracCtx() = default;

int AtracCtx::ValidateData() const {
    if (info_.state == STATUS_NO_DATA) return ERR_NO_DATA;
    return 0;
}

int AtracCtx::ValidateManaged() const {
    if (info_.state == STATUS_NO_DATA) return ERR_NO_DATA;
    if (info_.state == STATUS_LOW_LEVEL) return ERR_IS_LOW_LEVEL;
    if (info_.state == STATUS_FOR_SCESAS) return ERR_IS_FOR_SCESAS;
    return 0;
}

int AtracCtx::Analyze(const GuestMem& mem, uint32_t buffer, uint32_t size,
                      TrackInfo* track) {
    // A null/invalid pointer reads as zeroes: the size check fires first,
    // otherwise the zeroes are not a RIFF header.
    if (size < 72) {
        *track = TrackInfo();
        return ERR_SIZE_TOO_SMALL;
    }
    const uint8_t* p = mem.Ptr(buffer, 1);
    if (!p) {
        *track = TrackInfo();
        return ERR_UNKNOWN_FORMAT;
    }
    uint64_t avail = mem.size - (size_t)(buffer & GuestMem::kAddrMask);
    return ParseWave(p, (uint32_t)std::min<uint64_t>(size, avail), track);
}

void AtracCtx::CreateDecoder(bool jointStereo) {
    decoder_ = (factory_ ? factory_ : CreateStandaloneDecoder)(
        info_.codec, info_.sampleSize, info_.numChan, jointStereo);
}

int AtracCtx::InitFromTrack(const TrackInfo& wave, uint32_t bufferAddr,
                            int readSize, int bufferSize) {
    // Built on a fresh copy and committed only if the header is acceptable.
    IdInfo n;
    n.codec = info_.codec;
    n.state = STATUS_NO_DATA;
    n.numChan = (uint8_t)wave.numChans;
    const int extraSamples = n.codec == CODEC_AT3 ? 0x45 : 0x170;
    n.firstValidSample = extraSamples + wave.firstSampleOffset;
    n.sampleSize = (uint16_t)wave.blockAlign;
    InitLengthAndLoop(&n, wave.endSample, wave.waveDataSize,
                      wave.firstSampleOffset, wave.loopStart, wave.loopEnd);
    const int dataOff = wave.dataOff;
    const int endSample = n.endSample;
    n.streamDataByte = readSize - dataOff;
    n.buffer = bufferAddr;
    n.curFileOff = dataOff;
    n.dataOff = dataOff;
    n.fileDataEnd = wave.waveDataSize + dataOff;
    n.curBuffer = 0;
    n.bufferByte = (uint32_t)bufferSize;
    n.streamOff = dataOff;
    // A packet larger than the buffer can't be streamed. sampleSize is
    // file-derived, so reject it early.
    if (n.sampleSize > (uint32_t)bufferSize) {
        return ERR_BAD_CODEC_PARAMS;
    }
    if (n.loopEnd > endSample) {
        return ERR_BAD_CODEC_PARAMS;
    }

    const int numChunks = (endSample >> ((0x100b - n.codec) & 0x1f));
    if (numChunks * (uint32_t)n.sampleSize < (uint32_t)wave.waveDataSize) {
        int retval = ComputeAtracStateAndInitSecondBuffer(
            &n, (uint32_t)readSize, (uint32_t)bufferSize);
        if (retval < 0) {
            return retval;
        }
        info_ = n;
        return 0;
    }
    return ERR_BAD_CODEC_PARAMS;
}

int AtracCtx::SetData(uint32_t buffer, uint32_t readSize, uint32_t bufferSize,
                      int outputChannels) {
    TrackInfo track;
    int ret = Analyze(mem_, buffer, readSize, &track);
    if (ret < 0) {
        return ret;
    }
    if (track.codec != info_.codec) {
        return ERR_WRONG_CODECTYPE;
    }
    return SetData(track, buffer, readSize, bufferSize, outputChannels);
}

int AtracCtx::SetData(const TrackInfo& track, uint32_t bufferAddr,
                      uint32_t readSize, uint32_t bufferSize,
                      int outputChannels) {
    if (!mem_.IsValid(bufferAddr)) {
        return ERR_KERNEL_ILLEGAL_ADDRESS;
    }
    if (readSize > 0x7FFFFFFFu || bufferSize > 0x7FFFFFFFu) {
        return ERR_SIZE_TOO_SMALL;
    }
    // Games can abuse bufferSize, but readSize is what is actually there:
    // clamp it to the mapped region.
    if (!mem_.IsValidRange(bufferAddr, readSize)) {
        readSize = (uint32_t)(mem_.size - (bufferAddr & GuestMem::kAddrMask));
    }

    int retval = InitFromTrack(track, bufferAddr, (int)readSize, (int)bufferSize);
    if (retval < 0) {
        return retval;
    }

    CreateDecoder(track.jointStereo);
    outputChannels_ = outputChannels == 1 ? 1 : 2;
    codecErr_ = 0;

    int skipCount = 0;
    retval = SkipFrames(&skipCount);
    // Bad first frame (seen in Mui Mui house) or other skip errors are
    // reported, but the context stays set up.
    WrapLastPacket();
    return retval;
}

void AtracCtx::WrapLastPacket() {
    // If streaming, the overshot partial packet at the end is wrapped around.
    if (!StatusIsStreaming(info_.state)) {
        return;
    }

    // This logic is similar to GetStreamDataInfo.
    int distanceToEnd = RoundDownToMultiple(
        (int)info_.bufferByte - info_.streamOff, info_.sampleSize);
    if (info_.streamDataByte < distanceToEnd) {
        // There's space left without wrapping. PPSSPP zeroes 128 bytes at the
        // buffer start in this case (matches hardware's sanity-check dump).
        uint8_t* p = mem_.Ptr(info_.buffer, 128);
        if (p) std::memset(p, 0, 128);
    } else {
        // Wraps around: the last packet got split, copy its head to the
        // start of the buffer.
        const int copyStart = info_.streamOff + distanceToEnd;
        const int copyLen = (int)info_.bufferByte - copyStart;
        if (copyStart >= 0 && copyLen > 0) {
            uint8_t* dst = mem_.Ptr(info_.buffer, (uint32_t)copyLen);
            const uint8_t* src =
                mem_.Ptr(info_.buffer + (uint32_t)copyStart, (uint32_t)copyLen);
            if (dst && src) std::memmove(dst, src, (size_t)copyLen);
        }
    }
}

int AtracCtx::SkipFrames(int* skipCount) {
    *skipCount = 0;
    int finishIgnored;
    while (true) {
        if (info_.numSkipFrames == 0) {
            return 0;
        }
        int retval = DecodeInternal(0, nullptr, &finishIgnored);
        if (retval != 0) {
            if (retval == ERR_API_FAIL) {
                (*skipCount)++;
            }
            return retval;
        }
        (*skipCount)++;
    }
}

uint32_t AtracCtx::GetNextSamples() const {
    // TODO (PPSSPP): Need to reformulate this.
    const int endOfCurrentFrame = info_.decodePos | info_.SamplesFrameMask();
    const int remainder = std::max(0, endOfCurrentFrame - info_.endSample);
    const int adjusted = (info_.decodePos & info_.SamplesFrameMask()) + remainder;
    return (uint32_t)std::max(0, info_.SamplesPerFrame() - adjusted);
}

int AtracCtx::GetNextDecodePosition(int* pos) const {
    // Check if we reached the end.
    if (info_.decodePos > info_.endSample) {
        return ERR_ALL_DATA_DECODED;
    }
    // Check if remaining data in the file is smaller than a frame.
    if (info_.fileDataEnd - info_.curFileOff < info_.sampleSize) {
        return ERR_ALL_DATA_DECODED;
    }
    *pos = info_.decodePos - info_.firstValidSample;
    return 0;
}

int AtracCtx::DecodeData(uint32_t outbufAddr, int* samplesNum, int* finish,
                         int* remains) {
    const int tries = info_.numSkipFrames + 1;
    for (int i = 0; i < tries; i++) {
        int result = DecodeInternal(outbufAddr, samplesNum, finish);
        if (result != 0) {
            *samplesNum = 0;
            return result;
        }
    }

    *remains = RemainingFrames();
    return 0;
}

int AtracCtx::DecodeInternal(uint32_t outbufAddr, int* samplesNum,
                             int* finish) {
    IdInfo& info = info_;

    // Check for end of file.
    const int samplesToDecode = (int)GetNextSamples();
    const int nextFileOff = info.curFileOff + info.sampleSize;
    if (nextFileOff > info.fileDataEnd || info.decodePos > info.endSample) {
        *finish = 1;
        return ERR_ALL_DATA_DECODED;
    }

    // Check for streaming buffer run-out.
    if (StatusIsStreaming(info.state) && info.streamDataByte < info.sampleSize) {
        *finish = 0;
        return ERR_BUFFER_IS_EMPTY;
    }

    // Check for halfway buffer end.
    if (info.state == STATUS_HALFWAY_BUFFER &&
        info.dataOff + info.streamDataByte < nextFileOff) {
        *finish = 0;
        return ERR_BUFFER_IS_EMPTY;
    }

    if (info.state == STATUS_FOR_SCESAS || !decoder_) {
        *finish = 0;
        return ERR_API_FAIL;
    }

    // The output has to fit before anything is consumed.
    const uint32_t outBytes =
        (uint32_t)samplesToDecode * (uint32_t)outputChannels_ * 2u;
    if (info.numSkipFrames == 0 && outbufAddr != 0 && outBytes != 0 &&
        !mem_.IsValidRange(outbufAddr, outBytes)) {
        *finish = 0;
        return ERR_SIZE_TOO_SMALL;
    }

    uint32_t streamOff;
    uint32_t bufferPtr;
    if (!StatusIsStreaming(info.state)) {
        bufferPtr = info.buffer;
        streamOff = (uint32_t)info.curFileOff;
    } else {
        const int bufferIndex = info.curBuffer & 1;
        bufferPtr = bufferIndex == 0 ? info.buffer : info.secondBuffer;
        streamOff = (uint32_t)(bufferIndex == 0 ? info.streamOff
                                                : info.secondStreamOff);
    }

    const uint32_t inAddr = bufferPtr + streamOff;
    const uint8_t* inPtr = mem_.Ptr(inAddr, info.sampleSize);
    if (!inPtr) {
        *finish = 0;
        return ERR_API_FAIL;
    }

    if (decodeTemp_.size() < (size_t)info.SamplesPerFrame() * 2) {
        decodeTemp_.assign((size_t)info.SamplesPerFrame() * 2, 0);
    }

    int bytesConsumed = 0;
    int outSamples = 0;
    if (!decoder_->Decode(inPtr, info.sampleSize, &bytesConsumed,
                          outputChannels_, decodeTemp_.data(), &outSamples)) {
        // Decode failed.
        *finish = 0;
        // 0000020b and 0000020c have been observed for 0xFF and/or garbage
        // data on hardware.
        codecErr_ = 0x20b;
        return ERR_API_FAIL;  // tested (PPSSPP)
    } else {
        codecErr_ = 0;
    }

    // Advance the file offset.
    info.curFileOff += info.sampleSize;

    if (info.numSkipFrames == 0) {
        if (samplesNum) *samplesNum = samplesToDecode;
        if (info.endSample < info.decodePos + samplesToDecode) {
            *finish = info.loopNum == 0;
        } else {
            *finish = 0;
        }
        if (outbufAddr != 0 && samplesToDecode != 0) {
            // PPSSPP copies the head of the decoded frame (only the partial
            // first/last frames are shorter than a full frame).
            uint8_t* outBuf = mem_.Ptr(outbufAddr, outBytes);
            if (outBuf) std::memcpy(outBuf, decodeTemp_.data(), outBytes);
        }

        // Handle increments and looping.
        info.decodePos += samplesToDecode;
        if (info.loopEnd != 0 && info.loopNum != 0 &&
            info.decodePos > info.loopEnd) {
            info.curFileOff = ComputeFileOffset(info, info.loopStart);
            info.numSkipFrames = (uint8_t)ComputeSkipFrames(info, info.loopStart);
            info.decodePos = info.loopStart;
            if (info.loopNum > 0) {
                info.loopNum--;
            }
        }
    } else {
        info.numSkipFrames--;
    }

    // Handle streaming special cases.
    if (StatusIsStreaming(info.state)) {
        info.streamDataByte -= info.sampleSize;
        if (info.curBuffer == 1) {
            // If currently streaming from the second buffer...
            int nextStreamOff = info.secondStreamOff + info.sampleSize;
            if ((int)info.secondBufferByte < nextStreamOff + info.sampleSize) {
                // Done/ran out
                info.streamOff = 0;
                info.secondStreamOff = 0;
                info.curBuffer = 2;
            } else {
                info.secondStreamOff = nextStreamOff;
            }
        } else {
            // Normal streaming from the main buffer. Let's first look at
            // wrapping around the end...
            const int nextStreamOff = info.streamOff + info.sampleSize;
            if (nextStreamOff + info.sampleSize > (int)info.bufferByte) {
                info.streamOff = 0;
            } else {
                info.streamOff = nextStreamOff;
            }

            // Second buffer streaming: if we're in LOOP_WITH_TRAILER and
            // currently streaming from the main buffer, and either there's no
            // loop or we're just done with the final loop and haven't reached
            // the loop point yet...
            if (info.state == STATUS_STREAMED_LOOP_WITH_TRAILER &&
                info.curBuffer == 0 &&
                (info.loopEnd == 0 ||
                 (info.loopNum == 0 && info.loopEnd < info.decodePos))) {
                // ...and our file streaming offset has indeed reached the
                // loop point...
                if (info.curFileOff >= ComputeLoopEndFileOffset(info, info.loopEnd)) {
                    // ...switch to streaming from the secondary buffer, and
                    // copy the last partial packet from the second buffer
                    // back to the start of the main buffer.
                    info.curBuffer = 1;
                    info.streamDataByte = (int32_t)info.secondBufferByte;
                    info.secondStreamOff = 0;
                    // Clamp the copy to the main buffer size; sampleSize is
                    // file-derived and could be larger.
                    size_t copyLen = info.secondBufferByte % info.sampleSize;
                    if (copyLen > info.bufferByte) copyLen = info.bufferByte;
                    uint8_t* dst = mem_.Ptr(info.buffer, (uint32_t)copyLen);
                    const uint8_t* src = mem_.Ptr(
                        info.secondBuffer +
                            (info.secondBufferByte -
                             info.secondBufferByte % info.sampleSize),
                        (uint32_t)copyLen);
                    if (dst && src && copyLen) {
                        std::memmove(dst, src, copyLen);
                    }
                }
            }
        }
    }
    return 0;
}

int AtracCtx::RemainingFrames() const {
    const IdInfo& info = info_;

    // Handle the easy cases first.
    switch (info.state) {
    case STATUS_UNINITIALIZED:
    case STATUS_NO_DATA:
        return 0;
    case STATUS_ALL_DATA_LOADED:
        return REMAIN_ALLDATA_IS_ON_MEMORY;  // Not sure about no data.
    case STATUS_HALFWAY_BUFFER: {
        // Pretty simple - compute the remaining space, and divide by the
        // sample size, adjusting for frames-to-skip.
        const int writeFileOff = info.dataOff + info.streamDataByte;
        if (info.curFileOff < writeFileOff) {
            return std::max(0, (writeFileOff - info.curFileOff) / info.sampleSize -
                                   (int)info.numSkipFrames);
        }
        return 0;
    }
    case STATUS_STREAMED_WITHOUT_LOOP:
        return ComputeRemainFrameStream(info);

    case STATUS_STREAMED_LOOP_FROM_END:
        return ComputeRemainFrameLooped(info);

    case STATUS_STREAMED_LOOP_WITH_TRAILER:
        if (info.decodePos <= info.loopEnd) {
            // If before the tail, just treat it as looped.
            return ComputeRemainFrameLooped(info);
        } else {
            // If in tail, treat is as unlooped.
            return ComputeRemainFrameStream(info);
        }
    default:
        return ERR_BAD_ATRACID;
    }
}

int AtracCtx::LoopStatus() const {
    const IdInfo& info = info_;
    if (info.loopEnd == 0) {
        // No loop available.
        return 0;
    } else if (info.loopNum != 0) {
        // We've got at least one loop to go.
        return 1;
    } else {
        // Return 1 if we haven't passed the loop point.
        return info.decodePos <= info.loopEnd ? 1 : 0;
    }
}

int AtracCtx::SetLoopNum(int loopNum) {
    if (info_.loopEnd <= 0) {
        // File doesn't contain loop information, looping isn't allowed.
        return ERR_NO_LOOP_INFORMATION;
    }
    // Just override the current loop counter.
    info_.loopNum = loopNum;
    return 0;
}

int AtracCtx::GetSoundSample(int* outEndSample, int* outLoopStartSample,
                             int* outLoopEndSample) const {
    const IdInfo& info = info_;
    *outEndSample = info.endSample - info.firstValidSample;
    if (info.loopEnd == 0) {
        *outLoopStartSample = -1;
        *outLoopEndSample = -1;
    } else {
        *outLoopStartSample = info.loopStart - info.firstValidSample;
        *outLoopEndSample = info.loopEnd - info.firstValidSample;
    }
    return 0;
}

// ---- Streaming buffer bookkeeping ----

int AtracCtx::AddStreamData(uint32_t bytesToAdd) {
    IdInfo& info = info_;

    // WARNING: bytesToAdd might not be sampleSize aligned, even though we
    // return a sampleSize-aligned size in GetStreamDataInfo, so other parts of
    // the code still have to handle unaligned data amounts.
    if (info.state == STATUS_HALFWAY_BUFFER) {
        const int newFileOffset =
            (int)((uint32_t)info.streamDataByte + (uint32_t)info.dataOff +
                  bytesToAdd);
        if (newFileOffset == info.fileDataEnd) {
            info.state = STATUS_ALL_DATA_LOADED;
        } else if (newFileOffset > info.fileDataEnd) {
            return ERR_ADD_DATA_IS_TOO_BIG;
        }
        info.streamDataByte = (int32_t)((uint32_t)info.streamDataByte + bytesToAdd);
    } else {
        // TODO (PPSSPP): Check for ERR_ADD_DATA_IS_TOO_BIG in the other modes
        // too.
        info.streamDataByte = (int32_t)((uint32_t)info.streamDataByte + bytesToAdd);
    }
    return 0;
}

static int ComputeLoopedStreamWritableBytes(const IdInfo& info,
                                            const int loopStartFileOffset,
                                            const uint32_t loopEndFileOffset) {
    const uint32_t writeOffset =
        (uint32_t)(info.curFileOff + info.streamDataByte);
    if (writeOffset >= loopEndFileOffset) {
        const int loopLength = (int)(loopEndFileOffset - loopStartFileOffset);
        if (loopLength <= 0) return 0;  // guard: corrupt loop points
        return loopLength -
               ((info.curFileOff + info.streamDataByte) - (int)loopEndFileOffset) %
                   loopLength;
    } else {
        return (int)(loopEndFileOffset - writeOffset);
    }
}

static int IncrementAndLoop(int curOffset, int increment, int loopStart,
                            int loopEnd) {
    const int sum = curOffset + increment;
    if (sum >= loopEnd && loopEnd > loopStart) {
        return loopStart + (sum - loopEnd) % (loopEnd - loopStart);
    } else {
        return sum;
    }
}

static int WrapAroundRoundedBufferSize(int offset, int bufferSize, int addend,
                                       int grainSize) {
    bufferSize = RoundDownToMultipleWithOffset(offset, bufferSize, grainSize);
    const int sum = offset + addend;
    if (bufferSize <= sum) {
        return sum - bufferSize;
    } else {
        return sum;
    }
}

static void ComputeStreamBufferDataInfo(const IdInfo& info, uint32_t* writePtr,
                                        uint32_t* bytesToWrite,
                                        uint32_t* readFileOffset) {
    // Streaming data info
    //
    // This really is the core logic of sceAtrac (PPSSPP's words).
    const uint32_t streamOff = info.curBuffer != 1 ? info.streamOff : 0;
    const int spaceUsed = ComputeSpaceUsed(info);
    const int spaceLeftAfterStreamOff = RoundDownToMultipleWithOffset(
        (int)streamOff, (int)info.bufferByte, info.sampleSize);
    const int streamPos = (int)streamOff + spaceUsed;
    int spaceLeftInBuffer;
    if (streamPos >= spaceLeftAfterStreamOff) {
        spaceLeftInBuffer = spaceLeftAfterStreamOff - spaceUsed;
    } else {
        spaceLeftInBuffer = spaceLeftAfterStreamOff - streamPos;
    }
    const int loopStartFileOffset = ComputeFileOffset(info, info.loopStart);
    const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);

    if (spaceLeftInBuffer < 0) {
        // Most likely, the file was truncated.
        spaceLeftInBuffer = 0;
    }

    switch (info.state) {
    case STATUS_STREAMED_WITHOUT_LOOP: {
        *bytesToWrite = (uint32_t)std::clamp(
            info.fileDataEnd - (info.curFileOff + info.streamDataByte), 0,
            spaceLeftInBuffer);
        const int streamFileOff = info.curFileOff + info.streamDataByte;
        if (streamFileOff < info.fileDataEnd) {
            *readFileOffset = (uint32_t)streamFileOff;
            *writePtr = info.buffer +
                        (uint32_t)WrapAroundRoundedBufferSize(
                            info.streamOff, (int)info.bufferByte,
                            info.streamDataByte, info.sampleSize);
        } else {
            *readFileOffset = 0;
            *writePtr = info.buffer;
        }
        break;
    }

    case STATUS_STREAMED_LOOP_FROM_END:
        *bytesToWrite = (uint32_t)std::min(
            ComputeLoopedStreamWritableBytes(info, loopStartFileOffset,
                                             (uint32_t)loopEndFileOffset),
            spaceLeftInBuffer);
        *readFileOffset = (uint32_t)IncrementAndLoop(
            info.curFileOff, info.streamDataByte, loopStartFileOffset,
            loopEndFileOffset);
        *writePtr = info.buffer +
                    (uint32_t)WrapAroundRoundedBufferSize(
                        info.streamOff, (int)info.bufferByte,
                        info.streamDataByte, info.sampleSize);
        break;

    case STATUS_STREAMED_LOOP_WITH_TRAILER: {
        // Behaves like WITHOUT_LOOP or STREAMED_LOOP_FROM_END depending on
        // the decode position.
        if (info.decodePos <= info.loopEnd) {
            *bytesToWrite = (uint32_t)std::min(
                ComputeLoopedStreamWritableBytes(info, loopStartFileOffset,
                                                 (uint32_t)loopEndFileOffset),
                spaceLeftInBuffer);
            *readFileOffset = (uint32_t)IncrementAndLoop(
                info.curFileOff, info.streamDataByte, loopStartFileOffset,
                loopEndFileOffset);
        } else {
            const int streamFileOff = info.curFileOff + info.streamDataByte;
            *bytesToWrite = (uint32_t)std::clamp(info.fileDataEnd - streamFileOff,
                                                 0, spaceLeftInBuffer);
            if (streamFileOff < info.fileDataEnd) {
                *readFileOffset = (uint32_t)streamFileOff;
            } else {
                *readFileOffset = 0;
            }
        }
        if (info.decodePos <= info.loopEnd || info.curBuffer != 1) {
            *writePtr = info.buffer +
                        (uint32_t)WrapAroundRoundedBufferSize(
                            info.streamOff, (int)info.bufferByte,
                            info.streamDataByte, info.sampleSize);
        } else {
            *writePtr = info.buffer +
                        (uint32_t)WrapAroundRoundedBufferSize(
                            0, (int)info.bufferByte, spaceUsed, info.sampleSize);
        }
        break;
    }

    default:
        // unreachable (callers handle the other states)
        *writePtr = info.buffer;
        *bytesToWrite = 0;
        *readFileOffset = 0;
        break;
    }
}

void AtracCtx::GetStreamDataInfo(uint32_t* writePtr, uint32_t* bytesToWrite,
                                 uint32_t* readFileOffset) const {
    const IdInfo& info = info_;

    switch (info.state) {
    case STATUS_ALL_DATA_LOADED:
        // Nothing to do, the whole track is loaded already.
        *writePtr = info.buffer;
        *bytesToWrite = 0;
        *readFileOffset = 0;
        break;

    case STATUS_HALFWAY_BUFFER: {
        // This is both the file offset and the offset in the buffer, since
        // it's direct mapped in this mode (no wrapping or any other trickery).
        const int fileOffset = (int)info.dataOff + (int)info.streamDataByte;
        const int bytesLeftInFile = (int)info.fileDataEnd - fileOffset;
        // Just ask for the rest of the data. The game can supply as much of it
        // as it wants at a time.
        *writePtr = info.buffer + (uint32_t)fileOffset;
        *bytesToWrite = (uint32_t)bytesLeftInFile;
        *readFileOffset = (uint32_t)fileOffset;
        break;
    }

    default:
        ComputeStreamBufferDataInfo(info, writePtr, bytesToWrite, readFileOffset);
        break;
    }
}

// ---- Seeking ----

int AtracCtx::ResetPlayPosition(int seekPos, int bytesWrittenFirstBuf,
                                int bytesWrittenSecondBuf, bool* delay) {
    *delay = false;

    IdInfo& info = info_;
    if (info.state == STATUS_STREAMED_LOOP_WITH_TRAILER &&
        info.secondBufferByte == 0) {
        return ERR_SECOND_BUFFER_NEEDED;
    }

    seekPos += info.firstValidSample;

    if ((uint32_t)seekPos > (uint32_t)info.endSample) {
        return ERR_BAD_SAMPLE;
    }

    int result = ResetPlayPositionInternal(seekPos, bytesWrittenFirstBuf,
                                           bytesWrittenSecondBuf);
    if (result >= 0) {
        int skipCount = 0;
        result = SkipFrames(&skipCount);
        if (skipCount) {
            *delay = true;
        }
    }
    return result;
}

int AtracCtx::ResetPlayPositionInternal(int seekPos, int bytesWrittenFirstBuf,
                                        int bytesWrittenSecondBuf) {
    // Redo the same calculation as before, for input validation.
    ResetBufferInfo bufferInfo;
    GetResetBufferInfoInternal(&bufferInfo, seekPos);

    // Input validation.
    if ((uint32_t)bytesWrittenFirstBuf < bufferInfo.first.minWriteBytes ||
        (uint32_t)bytesWrittenFirstBuf > bufferInfo.first.writableBytes) {
        return ERR_BAD_FIRST_RESET_SIZE;
    }
    if ((uint32_t)bytesWrittenSecondBuf < bufferInfo.second.minWriteBytes ||
        (uint32_t)bytesWrittenSecondBuf > bufferInfo.second.writableBytes) {
        return ERR_BAD_SECOND_RESET_SIZE;
    }

    IdInfo& info = info_;
    info.decodePos = seekPos;
    info.numSkipFrames = (uint8_t)ComputeSkipFrames(info, seekPos);
    info.loopNum = 0;
    info.curFileOff = ComputeFileOffset(info, seekPos);

    codecErr_ = 0x20b;  // wtf? testing shows it. (PPSSPP)

    switch (info.state) {
    case STATUS_ALL_DATA_LOADED:
        // We're done.
        return 0;

    case STATUS_HALFWAY_BUFFER:
        info.streamDataByte += bytesWrittenFirstBuf;
        if (info.dataOff + info.streamDataByte >= info.fileDataEnd) {
            // Buffer full, we can transition to a full buffer here, if all the
            // bytes were written. Let's do it.
            info.state = STATUS_ALL_DATA_LOADED;
        }
        return 0;

    case STATUS_STREAMED_WITHOUT_LOOP:
    case STATUS_STREAMED_LOOP_FROM_END:
        // We just adopt the bytes that were written as our stream data, no
        // math needed.
        info.streamDataByte = bytesWrittenFirstBuf;
        info.curBuffer = 0;
        info.streamOff = 0;
        return 0;

    case STATUS_STREAMED_LOOP_WITH_TRAILER: {
        // As usual with the second buffer and trailer, things get tricky here.
        const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
        if (info.curFileOff >= loopEndFileOffset) {
            const int secondBufferSizeRounded =
                RoundDownToMultiple((int)info.secondBufferByte, info.sampleSize);
            if (info.curFileOff < loopEndFileOffset + secondBufferSizeRounded) {
                info.streamDataByte =
                    ((loopEndFileOffset + secondBufferSizeRounded) -
                     info.curFileOff) +
                    bytesWrittenFirstBuf;
                info.curBuffer = 1;
                info.secondStreamOff = info.curFileOff - loopEndFileOffset;
            } else {
                info.streamDataByte = bytesWrittenFirstBuf;
                info.curBuffer = 2;  // Temporary value! Will immediately switch back to 0.
                info.streamOff = 0;
            }
        } else {
            info.streamDataByte = bytesWrittenFirstBuf;
            info.curBuffer = 0;
            info.streamOff = 0;
        }
        return 0;
    }
    default:
        return 0;
    }
}

// This is basically sceAtracGetBufferInfoForResetting.
// NOTE: Not const! This can cause SkipFrames!
int AtracCtx::GetBufferInfoForResetting(ResetBufferInfo* bufferInfo, int seekPos,
                                        bool* delay) {
    *delay = false;
    const IdInfo& info = info_;

    if (info.state == STATUS_STREAMED_LOOP_WITH_TRAILER &&
        info.secondBufferByte == 0) {
        return ERR_SECOND_BUFFER_NEEDED;
    }

    seekPos += info.firstValidSample;

    if ((uint32_t)seekPos > (uint32_t)info.endSample) {
        return ERR_BAD_SAMPLE;
    }

    GetResetBufferInfoInternal(bufferInfo, seekPos);
    // Yes, this happens here! If there are any frames to skip, they get
    // skipped! Even though this looks like a function that shouldn't change
    // the state.
    int skipCount = 0;
    int retval = SkipFrames(&skipCount);
    if (skipCount > 0) *delay = true;
    return retval;
}

void AtracCtx::GetResetBufferInfoInternal(ResetBufferInfo* bufferInfo,
                                          int seekPos) const {
    const IdInfo& info = info_;

    switch (info.state) {
    case STATUS_NO_DATA:
    case STATUS_ALL_DATA_LOADED:
        // Everything is loaded, so nothing needs to be read.
        bufferInfo->first.writePosPtr = info.buffer;
        bufferInfo->first.writableBytes = 0;
        bufferInfo->first.minWriteBytes = 0;
        bufferInfo->first.filePos = 0;
        break;
    case STATUS_HALFWAY_BUFFER: {
        // Not too hard, we just ask to fill up the missing part of the buffer.
        const int streamPos = info.dataOff + info.streamDataByte;
        const int fileOff =
            info.dataOff + (seekPos / info.SamplesPerFrame() + 1) * info.sampleSize;
        bufferInfo->first.writePosPtr = info.buffer + (uint32_t)streamPos;
        bufferInfo->first.writableBytes = (uint32_t)(info.fileDataEnd - streamPos);
        bufferInfo->first.filePos = (uint32_t)streamPos;
        bufferInfo->first.minWriteBytes = (uint32_t)std::max(0, fileOff - streamPos);
        break;
    }

    case STATUS_STREAMED_WITHOUT_LOOP:
    case STATUS_STREAMED_LOOP_FROM_END: {
        // Relatively easy, just can't forget those skipped frames.
        const int curFileOffset = ComputeFileOffset(info, seekPos);
        const int bufferEnd = RoundDownToMultiple((int)info.bufferByte, info.sampleSize);
        bufferInfo->first.writePosPtr = info.buffer;
        bufferInfo->first.writableBytes =
            (uint32_t)std::min(info.fileDataEnd - curFileOffset, bufferEnd);
        bufferInfo->first.minWriteBytes =
            (uint32_t)((ComputeSkipFrames(info, seekPos) + 1) * info.sampleSize);
        bufferInfo->first.filePos = (uint32_t)curFileOffset;
        break;
    }
    case STATUS_STREAMED_LOOP_WITH_TRAILER: {
        // As usual, with the second buffer, things get crazy complicated...
        const int seekFileOffset = ComputeFileOffset(info, seekPos);
        const int loopEndFileOffset =
            ComputeLoopEndFileOffset(info, info.loopEnd) - 1;
        const int bufferEnd = RoundDownToMultiple((int)info.bufferByte, info.sampleSize);
        const int skipBytes = (ComputeSkipFrames(info, seekPos) + 1) * info.sampleSize;
        const int secondBufferEnd =
            RoundDownToMultiple((int)info.secondBufferByte, info.sampleSize);
        if (seekFileOffset < loopEndFileOffset) {
            const int remainingBeforeLoop = (loopEndFileOffset - seekFileOffset) + 1;
            bufferInfo->first.writePosPtr = info.buffer;
            bufferInfo->first.writableBytes =
                (uint32_t)std::min(bufferEnd, remainingBeforeLoop);
            bufferInfo->first.minWriteBytes =
                (uint32_t)std::min(skipBytes, remainingBeforeLoop);
            bufferInfo->first.filePos = (uint32_t)seekFileOffset;
        } else if (loopEndFileOffset + secondBufferEnd <= seekFileOffset) {
            bufferInfo->first.writePosPtr = info.buffer;
            bufferInfo->first.writableBytes =
                (uint32_t)std::min(info.fileDataEnd - seekFileOffset, bufferEnd);
            bufferInfo->first.minWriteBytes = (uint32_t)skipBytes;
            bufferInfo->first.filePos = (uint32_t)seekFileOffset;
        } else if (loopEndFileOffset + (int)info.secondBufferByte + 1 <
                   info.fileDataEnd) {
            const int endOffset = loopEndFileOffset + secondBufferEnd + 1;
            bufferInfo->first.writePosPtr = info.buffer;
            bufferInfo->first.writableBytes =
                (uint32_t)std::min(info.fileDataEnd - endOffset, bufferEnd);
            bufferInfo->first.minWriteBytes =
                (uint32_t)std::max(0, seekFileOffset + skipBytes - endOffset);
            bufferInfo->first.filePos = (uint32_t)endOffset;
        } else {
            bufferInfo->first.writePosPtr = info.buffer;
            bufferInfo->first.writableBytes = 0;
            bufferInfo->first.minWriteBytes = 0;
            bufferInfo->first.filePos = 0;
        }
        break;
    }
    default:
        bufferInfo->first = ResetBufferInfo::Single();
        break;
    }

    // Reset never needs a second buffer write, since the loop is in a fixed
    // place. second.writePosPtr is always the same as the first buffer's pos.
    bufferInfo->second.writePosPtr = info.buffer;
    bufferInfo->second.writableBytes = 0;
    bufferInfo->second.minWriteBytes = 0;
    bufferInfo->second.filePos = 0;
}

// ---- Second buffer ----

// Where to read from to fill the second buffer.
int AtracCtx::GetSecondBufferInfo(uint32_t* fileOffset, uint32_t* readSize) const {
    const IdInfo& info = info_;
    if (info.state != STATUS_STREAMED_LOOP_WITH_TRAILER) {
        // No second buffer needed in this state.
        *fileOffset = 0;
        *readSize = 0;
        return ERR_SECOND_BUFFER_NOT_NEEDED;
    }

    const int loopEndFileOffset = ComputeLoopEndFileOffset(info, info.loopEnd);
    *fileOffset = (uint32_t)loopEndFileOffset;
    *readSize = (uint32_t)(info.fileDataEnd - loopEndFileOffset);
    return 0;
}

int AtracCtx::SetSecondBuffer(uint32_t secondBuffer, uint32_t secondBufferSize) {
    IdInfo& info = info_;

    uint32_t loopEndFileOffset = (uint32_t)ComputeLoopEndFileOffset(info, info.loopEnd);
    if ((info.sampleSize * 3 <= (int)secondBufferSize ||
         (info.fileDataEnd - (int)loopEndFileOffset) <= (int)secondBufferSize)) {
        if (info.state == STATUS_STREAMED_LOOP_WITH_TRAILER) {
            info.secondBuffer = secondBuffer;
            info.secondBufferByte = secondBufferSize;
            info.secondStreamOff = 0;
            return 0;
        } else {
            return ERR_SECOND_BUFFER_NOT_NEEDED;
        }
    }
    return ERR_SIZE_TOO_SMALL;
}

}  // namespace psp_atrac
