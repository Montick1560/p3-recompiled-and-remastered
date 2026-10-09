#include "hle/psp_hle.h"
#include "hle/psp_psmf.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

// scePsmf HLE. Ported from PPSSPP (GPL-2.0-or-later), Core/HLE/scePsmf.cpp.
// Host objects are keyed by the guest psmfStruct address. The guest PsmfData
// writes, the selected-stream bookkeeping and the error codes follow PPSSPP.
// scePsmfGetPsmfVersion returns Psmf::version, the raw LE word (not the
// sceMpeg 0..3 index).

constexpr int32_t kErrIllegalAddress = (int32_t)0x8002006AU;
constexpr int32_t kErrNotInitialized = (int32_t)0x80615001;
constexpr int32_t kErrBadVersion = (int32_t)0x80615002;
constexpr int32_t kErrNotFound = (int32_t)0x80615025;
constexpr int32_t kErrInvalidId = (int32_t)0x80615100;
constexpr int32_t kErrInvalidValue = (int32_t)0x806151FEU;
constexpr int32_t kErrInvalidPsmf = (int32_t)0x80615501;

constexpr uint32_t kAddrMask = 0x07FFFFFFU;
constexpr uint32_t kNullPage = 0x00010000U;
constexpr uint32_t kHeaderBytes = 2048;
constexpr uint32_t kStructBytes = 32;

constexpr uint32_t kOffVersion = 0;
constexpr uint32_t kOffHeaderSize = 4;
constexpr uint32_t kOffHeaderOffset = 8;
constexpr uint32_t kOffStreamSize = 12;
constexpr uint32_t kOffStreamNum = 20;

struct PsmfObj {
    PsmfHeader header;
    int currentStreamNum = 0;
    int currentStreamType = -1;
    int currentStreamChannel = -1;

    bool validStream() const {
        return currentStreamNum >= 0 &&
               static_cast<size_t>(currentStreamNum) < header.streams.size();
    }
};

static std::mutex g_mtx;
static std::unordered_map<uint32_t, PsmfObj> g_psmf;

static void ret(recomp_context* ctx, int32_t v) { ctx->r[2] = v; }

static uint32_t arg(recomp_context* ctx, int n) {
    return static_cast<uint32_t>(ctx->r[4 + n]);
}

static bool valid_range(uint32_t addr, uint32_t len) {
    if (addr < kNullPage) return false;
    const uint64_t off = addr & kAddrMask;
    return off + len <= PSP_MEM_SIZE;
}

static uint8_t* guest_ptr(uint8_t* rdram, uint32_t addr, uint32_t len) {
    if (!rdram || !valid_range(addr, len)) return nullptr;
    return rdram + (addr & kAddrMask);
}

static bool write32(uint8_t* rdram, uint32_t addr, uint32_t v) {
    uint8_t* p = guest_ptr(rdram, addr, 4);
    if (!p) return false;
    std::memcpy(p, &v, 4);
    return true;
}

static bool read32(uint8_t* rdram, uint32_t addr, uint32_t* v) {
    const uint8_t* p = guest_ptr(rdram, addr, 4);
    if (!p) return false;
    std::memcpy(v, p, 4);
    return true;
}

static uint32_t readable(uint32_t addr, uint32_t want) {
    if (addr < kNullPage) return 0;
    const uint64_t off = addr & kAddrMask;
    if (off >= PSP_MEM_SIZE) return 0;
    const uint64_t avail = PSP_MEM_SIZE - off;
    return want < avail ? want : static_cast<uint32_t>(avail);
}

static PsmfObj* get_psmf(uint8_t* rdram, uint32_t addr) {
    if (!valid_range(addr, 4)) return nullptr;
    auto it = g_psmf.find(addr);
    if (it == g_psmf.end()) return nullptr;
    uint32_t streamNum = 0;
    if (!read32(rdram, addr + kOffStreamNum, &streamNum)) return nullptr;
    it->second.currentStreamNum = static_cast<int32_t>(streamNum);
    return &it->second;
}

static bool set_stream_num(uint8_t* rdram, uint32_t structAddr, PsmfObj* p, int num,
                     bool updateCached) {
    p->currentStreamNum = num;
    write32(rdram, structAddr + kOffStreamNum, static_cast<uint32_t>(num));
    if (updateCached) {
        p->currentStreamType = -1;
        p->currentStreamChannel = -1;
    }
    if (!p->validStream()) return false;
    const PsmfStreamInfo& s = p->header.streams[static_cast<size_t>(num)];
    p->currentStreamType = s.type;
    p->currentStreamChannel = s.channel;
    return true;
}

static void hle_scePsmfSetPsmf(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const uint32_t psmfStruct = arg(ctx, 0);
    const uint32_t psmfData = arg(ctx, 1);
    if (!valid_range(psmfStruct, kStructBytes) || readable(psmfData, 1) == 0) {
        ret(ctx, kErrIllegalAddress);
        return;
    }
    const uint32_t n = readable(psmfData, kHeaderBytes);
    PsmfHeader hdr;
    const PsmfStatus st = psmf_parse(rdram + (psmfData & kAddrMask), n, &hdr);
    if (st == PsmfStatus::BadVersion) {
        ret(ctx, kErrBadVersion);
        return;
    }
    if (st == PsmfStatus::BadStreamOffset) {
        ret(ctx, kErrInvalidValue);
        return;
    }
    if (st != PsmfStatus::Ok) {
        ret(ctx, kErrInvalidPsmf);
        return;
    }

    PsmfObj obj;
    obj.header = std::move(hdr);
    obj.currentStreamNum = 0;
    obj.currentStreamType = -1;
    obj.currentStreamChannel = -1;

    uint8_t* sp = guest_ptr(rdram, psmfStruct, kStructBytes);
    std::memset(sp, 0, kStructBytes);
    write32(rdram, psmfStruct + kOffVersion, obj.header.version);
    write32(rdram, psmfStruct + kOffHeaderSize, 0x800);
    write32(rdram, psmfStruct + kOffHeaderOffset, psmfData);
    write32(rdram, psmfStruct + kOffStreamSize, obj.header.streamSize);
    write32(rdram, psmfStruct + kOffStreamNum, 0);

    g_psmf[psmfStruct] = std::move(obj);
    ret(ctx, 0);
}

static void hle_scePsmfGetNumberOfStreams(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    PsmfObj* p = get_psmf(rdram, arg(ctx, 0));
    if (!p) {
        ret(ctx, kErrNotInitialized);
        return;
    }
    ret(ctx, static_cast<int32_t>(p->header.numStreams));
}

static void hle_scePsmfGetNumberOfSpecificStreams(uint8_t* rdram,
                                                  recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    PsmfObj* p = get_psmf(rdram, arg(ctx, 0));
    if (!p) {
        ret(ctx, kErrNotInitialized);
        return;
    }
    ret(ctx, p->header.countStreams(static_cast<int32_t>(arg(ctx, 1))));
}

static void hle_scePsmfSpecifyStream(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const uint32_t psmfStruct = arg(ctx, 0);
    PsmfObj* p = get_psmf(rdram, psmfStruct);
    if (!p) {
        ret(ctx, kErrNotInitialized);
        return;
    }
    const int streamNum = static_cast<int32_t>(arg(ctx, 1));
    if (!set_stream_num(rdram, psmfStruct, p, streamNum, true)) {
        set_stream_num(rdram, psmfStruct, p, kErrNotInitialized, true);
        ret(ctx, kErrInvalidId);
        return;
    }
    ret(ctx, 0);
}

static void hle_scePsmfGetVideoInfo(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    PsmfObj* p = get_psmf(rdram, arg(ctx, 0));
    if (!p) {
        ret(ctx, kErrNotInitialized);
        return;
    }
    if (!p->validStream()) {
        ret(ctx, kErrNotInitialized);
        return;
    }
    const uint32_t infoAddr = arg(ctx, 1);
    if (!valid_range(infoAddr, 8)) {
        ret(ctx, kErrIllegalAddress);
        return;
    }
    const PsmfStreamInfo& s =
        p->header.streams[static_cast<size_t>(p->currentStreamNum)];
    if (s.videoWidth == -1) {
        ret(ctx, kErrInvalidId);
        return;
    }
    write32(rdram, infoAddr, static_cast<uint32_t>(s.videoWidth));
    write32(rdram, infoAddr + 4, static_cast<uint32_t>(s.videoHeight));
    ret(ctx, 0);
}

static void hle_scePsmfGetCurrentStreamType(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    PsmfObj* p = get_psmf(rdram, arg(ctx, 0));
    if (!p) {
        ret(ctx, kErrNotInitialized);
        return;
    }
    if (p->currentStreamNum == kErrNotInitialized) {
        ret(ctx, kErrNotInitialized);
        return;
    }
    const uint32_t typeAddr = arg(ctx, 1);
    const uint32_t channelAddr = arg(ctx, 2);
    if (!valid_range(typeAddr, 4) || !valid_range(channelAddr, 4)) {
        ret(ctx, kErrIllegalAddress);
        return;
    }
    if (p->currentStreamType != -1) {
        write32(rdram, typeAddr, static_cast<uint32_t>(p->currentStreamType));
        write32(rdram, channelAddr, static_cast<uint32_t>(p->currentStreamChannel));
    }
    ret(ctx, 0);
}

static void hle_scePsmfGetPsmfVersion(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    PsmfObj* p = get_psmf(rdram, arg(ctx, 0));
    if (!p) {
        ret(ctx, kErrNotFound);
        return;
    }
    ret(ctx, static_cast<int32_t>(p->header.version));
}

void psp_hle_register_psmf() {
    psp_hle_register("scePsmfSetPsmf", hle_scePsmfSetPsmf);
    psp_hle_register("scePsmfGetNumberOfStreams", hle_scePsmfGetNumberOfStreams);
    psp_hle_register("scePsmfGetNumberOfSpecificStreams",
                     hle_scePsmfGetNumberOfSpecificStreams);
    psp_hle_register("scePsmfSpecifyStream", hle_scePsmfSpecifyStream);
    psp_hle_register("scePsmfGetVideoInfo", hle_scePsmfGetVideoInfo);
    psp_hle_register("scePsmfGetCurrentStreamType", hle_scePsmfGetCurrentStreamType);
    psp_hle_register("scePsmfGetPsmfVersion", hle_scePsmfGetPsmfVersion);
}
