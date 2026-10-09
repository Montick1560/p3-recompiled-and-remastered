#include "hle/psp_atrac_ctx.h"
#include "hle/psp_hle.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

// ================================================================
// sceAtrac3plus HLE: thin glue between the PSP calling convention and the
// pure AtracCtx (hle/psp_atrac_ctx.h). Ported from PPSSPP
// (GPL-2.0-or-later), Core/HLE/sceAtrac.cpp -- argument order, output
// pointer writes and return codes follow it; the hleDelayResult/ME timing
// emulation is intentionally not reproduced (calls complete immediately).
//
// Argument convention: PSP syscalls pass a0..a3 in ctx->r[4..7] and the 5th
// in t0 (ctx->r[8]). Results go to ctx->r[2]; errors are the 0x806300xx
// sceAtrac codes. All state sits behind one mutex (decode, streaming and
// reset calls arrive from different game threads).
// ================================================================

namespace {

using namespace psp_atrac;

constexpr int kMaxAtracIds = 6;  // PSP_MAX_ATRAC_IDS
constexpr int32_t kErrBusy = (int32_t)0x80000021U;
constexpr int32_t kErrOutOfMemory = (int32_t)0x80000022U;

std::mutex g_atrac_mtx;
std::unique_ptr<AtracCtx> g_contexts[kMaxAtracIds];
// Slot types: "start with 2 of each in this order" (PPSSPP __AtracInit).
uint16_t g_context_types[kMaxAtracIds] = {
    CODEC_AT3PLUS, CODEC_AT3PLUS, CODEC_AT3, CODEC_AT3, 0, 0};


GuestMem guest(uint8_t* rdram) { return GuestMem(rdram, PSP_MEM_SIZE); }

AtracCtx* get_atrac(int id) {
    if (id < 0 || id >= kMaxAtracIds) return nullptr;
    return g_contexts[id].get();
}

int validate_data(const AtracCtx* a) {
    return a ? a->ValidateData() : ERR_BAD_ATRACID;
}
int validate_managed(const AtracCtx* a) {
    return a ? a->ValidateManaged() : ERR_BAD_ATRACID;
}

int alloc_atrac(uint8_t* rdram, int codecType) {
    for (int i = 0; i < kMaxAtracIds; i++) {
        if (g_context_types[i] == codecType && !g_contexts[i]) {
            g_contexts[i].reset(new AtracCtx(guest(rdram), codecType));
            return i;
        }
    }
    return ERR_NO_ATRACID;
}

int free_atrac(int id) {
    if (id >= 0 && id < kMaxAtracIds && g_contexts[id]) {
        g_contexts[id].reset();
        return 0;
    }
    return ERR_BAD_ATRACID;
}

void ret(recomp_context* ctx, int32_t v) { ctx->r[2] = v; }

uint32_t arg(recomp_context* ctx, int n) {
    return static_cast<uint32_t>(ctx->r[4 + n]);
}

void put32(uint8_t* rdram, uint32_t addr, uint32_t v) {
    // Invalid output pointers are skipped (PPSSPP: IsValidAddress guard).
    guest(rdram).Write32(addr, v);
}

// ---- Handlers ----

// sceAtracGetAtracID(codecType)
void hle_sceAtracGetAtracID(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    const uint32_t codecType = arg(ctx, 0);
    if (codecType != CODEC_AT3 && codecType != CODEC_AT3PLUS) {
        ret(ctx, ERR_INVALID_CODECTYPE);
        return;
    }
    ret(ctx, alloc_atrac(rdram, (int)codecType));
}

// sceAtracReleaseAtracID(atracID)
void hle_sceAtracReleaseAtracID(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    ret(ctx, free_atrac((int)arg(ctx, 0)));
}

// sceAtracSetData(atracID, buffer, bufferSize)
void hle_sceAtracSetData(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    if (!a) {
        ret(ctx, ERR_BAD_ATRACID);
        return;
    }
    const uint32_t buffer = arg(ctx, 1);
    const uint32_t bufferSize = arg(ctx, 2);
    ret(ctx, a->SetData(buffer, bufferSize, bufferSize, 2));
}

// sceAtracSetHalfwayBuffer(atracID, buffer, readSize, bufferSize)
void hle_sceAtracSetHalfwayBuffer(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    if (!a) {
        ret(ctx, ERR_BAD_ATRACID);
        return;
    }
    const uint32_t buffer = arg(ctx, 1);
    const uint32_t readSize = arg(ctx, 2);
    const uint32_t bufferSize = arg(ctx, 3);
    if (readSize > bufferSize) {
        ret(ctx, ERR_INCORRECT_READ_SIZE);
        return;
    }
    ret(ctx, a->SetData(buffer, readSize, bufferSize, 2));
}

// Shared by the *AndGetID variants: analyse, allocate a slot of the data's
// codec type, set the data, free the slot again on failure.
int32_t set_data_and_get_id(uint8_t* rdram, uint32_t buffer, uint32_t readSize,
                            uint32_t bufferSize) {
    TrackInfo track;
    int r = AtracCtx::Analyze(guest(rdram), buffer, readSize, &track);
    if (r < 0) return r;
    int id = alloc_atrac(rdram, track.codec);
    if (id < 0) return id;
    r = g_contexts[id]->SetData(track, buffer, readSize, bufferSize, 2);
    if (r < 0) {
        free_atrac(id);
        return r;
    }
    return id;
}

// sceAtracSetDataAndGetID(buffer, bufferSize)
void hle_sceAtracSetDataAndGetID(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    uint32_t bufferSize = arg(ctx, 1);
    // A large value happens in Tales of VS; it is impossible for it to be
    // that big anyway, so cap it (PPSSPP).
    if ((int32_t)bufferSize < 0) bufferSize = 0x10000000;
    ret(ctx, set_data_and_get_id(rdram, arg(ctx, 0), bufferSize, bufferSize));
}

// sceAtracSetHalfwayBufferAndGetID(buffer, readSize, bufferSize)
void hle_sceAtracSetHalfwayBufferAndGetID(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    const uint32_t readSize = arg(ctx, 1);
    const uint32_t bufferSize = arg(ctx, 2);
    if (readSize > bufferSize) {
        ret(ctx, ERR_INCORRECT_READ_SIZE);
        return;
    }
    ret(ctx, set_data_and_get_id(rdram, arg(ctx, 0), readSize, bufferSize));
}

// sceAtracReinit(at3Count, at3plusCount)
void hle_sceAtracReinit(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    const int at3Count = (int)arg(ctx, 0);
    const int at3plusCount = (int)arg(ctx, 1);
    for (int i = 0; i < kMaxAtracIds; i++) {
        if (g_contexts[i]) {
            ret(ctx, kErrBusy);  // cannot reinit while IDs in use
            return;
        }
    }

    std::memset(g_context_types, 0, sizeof(g_context_types));
    int next = 0;
    int space = kMaxAtracIds;

    // This seems to deinit things.
    if (at3Count == 0 && at3plusCount == 0) {
        ret(ctx, 0);
        return;
    }

    // First, ATRAC3+. These IDs seem to cost double (probably memory).
    // Intentionally signed: 9999 tries to allocate, -1 does not.
    for (int i = 0; i < at3plusCount; i++) {
        space -= 2;
        if (space >= 0) g_context_types[next++] = CODEC_AT3PLUS;
    }
    for (int i = 0; i < at3Count; i++) {
        space -= 1;
        if (space >= 0) g_context_types[next++] = CODEC_AT3;
    }

    // If we ran out of space, we still initialise some, but return an error.
    ret(ctx, space >= 0 ? 0 : kErrOutOfMemory);
}

// sceAtracDecodeData(atracID, outAddr, numSamplesAddr, finishFlagAddr,
//                    remainAddr)
// A null outAddr is valid (skips data).
void hle_sceAtracDecodeData(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_data(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    const uint32_t outAddr = arg(ctx, 1);
    const uint32_t numSamplesAddr = arg(ctx, 2);
    const uint32_t finishFlagAddr = arg(ctx, 3);
    const uint32_t remainAddr = arg(ctx, 4);

    if (outAddr & 1) {
        ret(ctx, ERR_BAD_ALIGNMENT);
        return;
    }
    if (outAddr != 0 && !guest(rdram).IsValid(outAddr)) {
        // Dunno what error code to return here, but we have to bail
        // (PPSSPP uses the same).
        ret(ctx, ERR_SIZE_TOO_SMALL);
        return;
    }

    int numSamplesWritten = 0;
    int finish = 0;
    int remains = 0;
    int r = a->DecodeData(outAddr, &numSamplesWritten, &finish, &remains);
    if (r != ERR_BAD_ATRACID && r != ERR_NO_DATA) {
        put32(rdram, numSamplesAddr, (uint32_t)numSamplesWritten);
        put32(rdram, finishFlagAddr, (uint32_t)finish);
        // On error, no remaining frame value is written.
        if (r == 0) put32(rdram, remainAddr, (uint32_t)remains);
    }
    // First decodes + every 500th: samples written and output peak, to tell
    // "nothing decoded" from "decoded silence" when music is missing.
    static int decode_calls = 0;
    if (++decode_calls <= 8 || decode_calls % 500 == 0) {
        int peak = 0;
        if (outAddr != 0 && numSamplesWritten > 0) {
            for (int i = 0; i < numSamplesWritten * 2; i++) {
                int v = (int16_t)(rdram[(outAddr + i * 2) & 0x07FFFFFFU] |
                                  (rdram[(outAddr + i * 2 + 1) & 0x07FFFFFFU] << 8));
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
        }
        std::fprintf(stderr,
            "[ATRAC] decode #%d id=%u ret=0x%08X samples=%d finish=%d remain=%d peak=%d\n",
            decode_calls, (unsigned)arg(ctx, 0), (unsigned)r, numSamplesWritten,
            finish, remains, peak);
    }
    ret(ctx, r);
}

// sceAtracGetNextSample(atracID, outNAddr)
void hle_sceAtracGetNextSample(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_data(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    put32(rdram, arg(ctx, 1), a->GetNextSamples());
    ret(ctx, 0);
}

// sceAtracGetStreamDataInfo(atracID, writePtrAddr, writableBytesAddr,
//                           readOffsetAddr)
void hle_sceAtracGetStreamDataInfo(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    uint32_t writePtr = 0, writableBytes = 0, readOffset = 0;
    a->GetStreamDataInfo(&writePtr, &writableBytes, &readOffset);
    put32(rdram, arg(ctx, 1), writePtr);
    put32(rdram, arg(ctx, 2), writableBytes);
    put32(rdram, arg(ctx, 3), readOffset);
    ret(ctx, 0);
}

// sceAtracAddStreamData(atracID, bytesToAdd)
void hle_sceAtracAddStreamData(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    if (a->BufferState() == STATUS_ALL_DATA_LOADED) {
        // Some games call this with 0, which is pretty harmless.
        ret(ctx, ERR_ALL_DATA_LOADED);
        return;
    }
    ret(ctx, a->AddStreamData(arg(ctx, 1)));
}

// sceAtracSetLoopNum(atracID, loopNum)
void hle_sceAtracSetLoopNum(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_data(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    ret(ctx, a->SetLoopNum((int)arg(ctx, 1)));
}

// sceAtracGetRemainFrame(atracID, remainAddr)
void hle_sceAtracGetRemainFrame(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    if (!guest(rdram).IsValid(arg(ctx, 1))) {
        ret(ctx, ERR_KERNEL_ILLEGAL_ADDR);  // would crash on hardware
        return;
    }
    put32(rdram, arg(ctx, 1), (uint32_t)a->RemainingFrames());
    ret(ctx, 0);
}

// sceAtracGetSoundSample(atracID, endSampleAddr, loopStartAddr, loopEndAddr)
void hle_sceAtracGetSoundSample(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    int endSample = -1, loopStart = -1, loopEnd = -1;
    int r = a->GetSoundSample(&endSample, &loopStart, &loopEnd);
    if (r < 0) {
        ret(ctx, r);
        return;
    }
    put32(rdram, arg(ctx, 1), (uint32_t)endSample);
    put32(rdram, arg(ctx, 2), (uint32_t)loopStart);
    put32(rdram, arg(ctx, 3), (uint32_t)loopEnd);
    ret(ctx, r);
}

// sceAtracResetPlayPosition(atracID, sample, bytesWrittenFirstBuf,
//                           bytesWrittenSecondBuf)
void hle_sceAtracResetPlayPosition(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    bool delay = false;
    ret(ctx, a->ResetPlayPosition((int)arg(ctx, 1), (int)arg(ctx, 2),
                                  (int)arg(ctx, 3), &delay));
}

// sceAtracGetBufferInfoForResetting(atracID, sample, bufferInfoAddr)
// Writes AtracResetBufferInfo: {first, second} x {writePosPtr, writableBytes,
// minWriteBytes, filePos}.
void hle_sceAtracGetBufferInfoForResetting(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    const uint32_t infoAddr = arg(ctx, 2);
    if (!guest(rdram).IsValidRange(infoAddr, 32)) {
        ret(ctx, ERR_KERNEL_ILLEGAL_ADDR);  // "invalid buffer, should crash"
        return;
    }
    ResetBufferInfo info;
    bool delay = false;
    int r = a->GetBufferInfoForResetting(&info, (int)arg(ctx, 1), &delay);
    if (r != ERR_SECOND_BUFFER_NEEDED && r != ERR_BAD_SAMPLE) {
        const uint32_t words[8] = {
            info.first.writePosPtr,  info.first.writableBytes,
            info.first.minWriteBytes, info.first.filePos,
            info.second.writePosPtr, info.second.writableBytes,
            info.second.minWriteBytes, info.second.filePos};
        for (int i = 0; i < 8; i++) put32(rdram, infoAddr + 4 * i, words[i]);
    }
    ret(ctx, r);
}

// sceAtracGetSecondBufferInfo(atracID, fileOffsetAddr, desiredSizeAddr)
void hle_sceAtracGetSecondBufferInfo(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    if (!guest(rdram).IsValid(arg(ctx, 1)) ||
        !guest(rdram).IsValid(arg(ctx, 2))) {
        ret(ctx, ERR_KERNEL_ILLEGAL_ADDR);
        return;
    }
    uint32_t fileOffset = 0, desiredSize = 0;
    int r = a->GetSecondBufferInfo(&fileOffset, &desiredSize);
    put32(rdram, arg(ctx, 1), fileOffset);
    put32(rdram, arg(ctx, 2), desiredSize);
    ret(ctx, r);
}

// sceAtracSetSecondBuffer(atracID, secondBuffer, secondBufferSize)
void hle_sceAtracSetSecondBuffer(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    ret(ctx, a->SetSecondBuffer(arg(ctx, 1), arg(ctx, 2)));
}

// sceAtracIsSecondBufferNeeded(atracID)
void hle_sceAtracIsSecondBufferNeeded(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_managed(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    // True whether the buffer is already set or not.
    ret(ctx, a->BufferState() == STATUS_STREAMED_LOOP_WITH_TRAILER ? 1 : 0);
}

// Simple getters: (atracID, outAddr) with the data-validity check. An invalid
// output pointer is silently ignored (PPSSPP returns 0 for those).
template <typename Fn>
void simple_getter(uint8_t* rdram, recomp_context* ctx, Fn fn) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_data(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    put32(rdram, arg(ctx, 1), fn(*a));
    ret(ctx, 0);
}

void hle_sceAtracGetBitrate(uint8_t* rdram, recomp_context* ctx) {
    simple_getter(rdram, ctx, [](const AtracCtx& a) { return (uint32_t)a.Bitrate(); });
}
void hle_sceAtracGetChannel(uint8_t* rdram, recomp_context* ctx) {
    simple_getter(rdram, ctx, [](const AtracCtx& a) { return (uint32_t)a.Channels(); });
}
void hle_sceAtracGetOutputChannel(uint8_t* rdram, recomp_context* ctx) {
    simple_getter(rdram, ctx, [](const AtracCtx& a) { return (uint32_t)a.OutputChannels(); });
}
void hle_sceAtracGetMaxSample(uint8_t* rdram, recomp_context* ctx) {
    simple_getter(rdram, ctx, [](const AtracCtx& a) { return (uint32_t)a.SamplesPerFrame(); });
}
void hle_sceAtracGetInternalErrorInfo(uint8_t* rdram, recomp_context* ctx) {
    simple_getter(rdram, ctx, [](const AtracCtx& a) { return a.InternalCodecError(); });
}

// sceAtracGetNextDecodePosition(atracID, outPosAddr)
void hle_sceAtracGetNextDecodePosition(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_data(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    if (!guest(rdram).IsValid(arg(ctx, 1))) {
        ret(ctx, 0);  // PPSSPP: logs "invalid address", returns 0
        return;
    }
    int pos = 0;
    int r = a->GetNextDecodePosition(&pos);
    if (r < 0) {
        ret(ctx, r);
        return;
    }
    put32(rdram, arg(ctx, 1), (uint32_t)pos);
    ret(ctx, 0);
}

// sceAtracGetLoopStatus(atracID, loopNumAddr, statusAddr)
void hle_sceAtracGetLoopStatus(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_atrac_mtx);
    AtracCtx* a = get_atrac((int)arg(ctx, 0));
    int err = validate_data(a);
    if (err != 0) {
        ret(ctx, err);
        return;
    }
    put32(rdram, arg(ctx, 1), (uint32_t)a->LoopNum());
    put32(rdram, arg(ctx, 2), (uint32_t)a->LoopStatus());
    ret(ctx, 0);
}

}  // namespace

// ---- Registration ----

void psp_hle_register_atrac() {
    psp_hle_register("sceAtracGetAtracID", hle_sceAtracGetAtracID);
    psp_hle_register("sceAtracReleaseAtracID", hle_sceAtracReleaseAtracID);
    psp_hle_register("sceAtracSetData", hle_sceAtracSetData);
    psp_hle_register("sceAtracSetHalfwayBuffer", hle_sceAtracSetHalfwayBuffer);
    psp_hle_register("sceAtracSetDataAndGetID", hle_sceAtracSetDataAndGetID);
    psp_hle_register("sceAtracSetHalfwayBufferAndGetID",
                     hle_sceAtracSetHalfwayBufferAndGetID);
    psp_hle_register("sceAtracReinit", hle_sceAtracReinit);
    psp_hle_register("sceAtracDecodeData", hle_sceAtracDecodeData);
    psp_hle_register("sceAtracGetNextSample", hle_sceAtracGetNextSample);
    psp_hle_register("sceAtracGetStreamDataInfo", hle_sceAtracGetStreamDataInfo);
    psp_hle_register("sceAtracAddStreamData", hle_sceAtracAddStreamData);
    psp_hle_register("sceAtracSetLoopNum", hle_sceAtracSetLoopNum);
    psp_hle_register("sceAtracGetRemainFrame", hle_sceAtracGetRemainFrame);
    psp_hle_register("sceAtracGetSoundSample", hle_sceAtracGetSoundSample);
    psp_hle_register("sceAtracResetPlayPosition", hle_sceAtracResetPlayPosition);
    // NID 0x2DD3E298; 0xCA3CA3D2 is the same call under its historical typo.
    psp_hle_register("sceAtracGetBufferInfoForResetting",
                     hle_sceAtracGetBufferInfoForResetting);
    psp_hle_register("sceAtracGetBufferInfoForReseting",
                     hle_sceAtracGetBufferInfoForResetting);
    psp_hle_register("sceAtracGetSecondBufferInfo", hle_sceAtracGetSecondBufferInfo);
    psp_hle_register("sceAtracSetSecondBuffer", hle_sceAtracSetSecondBuffer);
    psp_hle_register("sceAtracIsSecondBufferNeeded", hle_sceAtracIsSecondBufferNeeded);
    psp_hle_register("sceAtracGetBitrate", hle_sceAtracGetBitrate);
    psp_hle_register("sceAtracGetChannel", hle_sceAtracGetChannel);
    psp_hle_register("sceAtracGetOutputChannel", hle_sceAtracGetOutputChannel);
    psp_hle_register("sceAtracGetMaxSample", hle_sceAtracGetMaxSample);
    psp_hle_register("sceAtracGetInternalErrorInfo", hle_sceAtracGetInternalErrorInfo);
    psp_hle_register("sceAtracGetNextDecodePosition", hle_sceAtracGetNextDecodePosition);
    psp_hle_register("sceAtracGetLoopStatus", hle_sceAtracGetLoopStatus);
}
