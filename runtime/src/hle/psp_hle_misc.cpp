#include "hle/psp_hle.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdint>
#include <cstring>

// Small Patapon 3 imports. Return values and pointer checks follow PPSSPP
// Core/HLE (sceNet, sceOpenPSID, sceHprm, sceUmd, scePspNpDrm_user, sceKernel).

static constexpr uint32_t kAddrMask = 0x07FFFFFFU;
static constexpr uint32_t kNullPage = 0x00010000U;
static constexpr int32_t kInvalidSize = (int32_t)0x80000104;
static constexpr int32_t kNpDrmInvalidFile = (int32_t)0x80550902;
static constexpr int kNpDrmKeyLen = 16;

static const uint8_t kWlanMac[6] = {0x00, 0x1D, 0xD9, 0x50, 0x33, 0x03};
static const uint8_t kOpenPsid[16] = {
    0x10, 0x02, 0xA3, 0x44, 0x13, 0xF5, 0x93, 0xB0,
    0xCC, 0x6E, 0xD1, 0x32, 0x27, 0x85, 0x0F, 0x9D};

static uint8_t g_licensee_key[kNpDrmKeyLen];
static bool g_licensee_key_set = false;

static bool valid_range(uint32_t addr, uint32_t len) {
    if (addr < kNullPage) return false;
    const uint64_t off = static_cast<uint64_t>(addr & kAddrMask);
    return off + len <= PSP_MEM_SIZE;
}

static void write_bytes(uint8_t* rdram, uint32_t addr, const void* src,
                        uint32_t len) {
    std::memcpy(rdram + (addr & kAddrMask), src, len);
}

static void hle_sceWlanGetEtherAddr(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
    if (!valid_range(addr, 6)) {
        ctx->r[2] = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        return;
    }
    write_bytes(rdram, addr, kWlanMac, 6);
    ctx->r[2] = 0;
}

static void hle_sceOpenPSIDGetOpenPSID(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
    if (valid_range(addr, 16)) {
        write_bytes(rdram, addr, kOpenPsid, 16);
    }
    ctx->r[2] = 0;
}

static void hle_sceHprmIsHeadphoneExist(uint8_t* rdram, recomp_context* ctx) {
    ctx->r[2] = 0;
    (void)rdram;
}

static void hle_sceUmdCancelWaitDriveStat(uint8_t* rdram, recomp_context* ctx) {
    ctx->r[2] = 0;
    (void)rdram;
}

static void hle_sceNpDrmSetLicenseeKey(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
    if (!valid_range(addr, 1)) {
        ctx->r[2] = kNpDrmInvalidFile;
        return;
    }
    if (valid_range(addr, kNpDrmKeyLen)) {
        std::memcpy(g_licensee_key, rdram + (addr & kAddrMask), kNpDrmKeyLen);
    }
    g_licensee_key_set = true;
    ctx->r[2] = 0;
}

static void hle_sceNpDrmClearLicenseeKey(uint8_t* rdram, recomp_context* ctx) {
    std::memset(g_licensee_key, 0, kNpDrmKeyLen);
    g_licensee_key_set = false;
    ctx->r[2] = 0;
    (void)rdram;
}

static void hle_sceKernelIcacheInvalidateAll(uint8_t* rdram,
                                             recomp_context* ctx) {
    ctx->r[2] = 0;
    (void)rdram;
}

static void hle_sceKernelDcacheWritebackInvalidateRange(uint8_t* rdram,
                                                        recomp_context* ctx) {
    const int size = ctx->r[5];
    if (size < 0) {
        ctx->r[2] = kInvalidSize;
        return;
    }
    ctx->r[2] = 0;
    (void)rdram;
}

void psp_hle_register_misc() {
    psp_hle_register("sceWlanGetEtherAddr", hle_sceWlanGetEtherAddr);
    psp_hle_register("sceOpenPSIDGetOpenPSID", hle_sceOpenPSIDGetOpenPSID);
    psp_hle_register("sceHprmIsHeadphoneExist", hle_sceHprmIsHeadphoneExist);
    psp_hle_register("sceUmdCancelWaitDriveStat",
                     hle_sceUmdCancelWaitDriveStat);
    psp_hle_register("sceNpDrmSetLicenseeKey", hle_sceNpDrmSetLicenseeKey);
    psp_hle_register("sceNpDrmClearLicenseeKey", hle_sceNpDrmClearLicenseeKey);
    psp_hle_register("sceKernelIcacheInvalidateAll",
                     hle_sceKernelIcacheInvalidateAll);
    psp_hle_register("sceKernelDcacheWritebackInvalidateRange",
                     hle_sceKernelDcacheWritebackInvalidateRange);
}
