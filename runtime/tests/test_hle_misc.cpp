// Small HLE import tests. Handlers run through a stub registry against a
// fake guest RAM, the same way test_hle_psmf.cpp drives psp_hle_psmf.cpp.
// Oracle: PPSSPP Core/HLE (sceNet, sceOpenPSID, sceHprm, sceUmd, scePower,
// scePspNpDrm_user, sceKernel).

#include "hle/psp_hle.h"
#include "psp_memory.h"
#include "recomp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

static int failures = 0;
static int tests_run = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        tests_run++;                                                           \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);       \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected, msg)                                        \
    do {                                                                       \
        tests_run++;                                                           \
        const long long a_ = static_cast<long long>(actual);                   \
        const long long e_ = static_cast<long long>(expected);                 \
        if (a_ != e_) {                                                        \
            std::fprintf(stderr,                                               \
                         "FAIL: %s: got %lld (0x%llx), expected %lld (0x%llx) " \
                         "(line %d)\n",                                        \
                         msg, a_, (unsigned long long)a_, e_,                  \
                         (unsigned long long)e_, __LINE__);                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

// PPSSPP Core/HLE/ErrorCodes.h
static constexpr int32_t ERR_ILLEGAL_ADDR = (int32_t)0x800200D3;
static constexpr int32_t ERR_INVALID_SIZE = (int32_t)0x80000104;
static constexpr int32_t ERR_PRIV_REQUIRED = (int32_t)0x80000023;
static constexpr int32_t ERR_TAKEN_SLOT = (int32_t)0x80000020;
static constexpr int32_t ERR_NPDRM_INVALID_FILE = (int32_t)0x80550902;

static const uint8_t kMac[6] = {0x00, 0x1D, 0xD9, 0x50, 0x33, 0x03};
static const uint8_t kOpenPsid[16] = {
    0x10, 0x02, 0xA3, 0x44, 0x13, 0xF5, 0x93, 0xB0,
    0xCC, 0x6E, 0xD1, 0x32, 0x27, 0x85, 0x0F, 0x9D};

static std::map<std::string, HleFunc>& registry() {
    static std::map<std::string, HleFunc> r;
    return r;
}

void psp_hle_register(const char* nid_name, HleFunc fn) {
    registry()[nid_name] = fn;
}

static uint8_t* g_ram = nullptr;

static int32_t callHle(const char* name, int32_t a0 = 0, int32_t a1 = 0,
                       int32_t a2 = 0, int32_t a3 = 0) {
    auto it = registry().find(name);
    if (it == registry().end()) {
        std::fprintf(stderr, "FAIL: HLE function %s not registered\n", name);
        failures++;
        return 0x7FFFFFFF;
    }
    recomp_context ctx{};
    ctx.r[2] = 0x12345678;
    ctx.r[4] = a0;
    ctx.r[5] = a1;
    ctx.r[6] = a2;
    ctx.r[7] = a3;
    it->second(g_ram, &ctx);
    return ctx.r[2];
}

static uint8_t* guest(uint32_t addr) {
    return g_ram + (addr & 0x07FFFFFFu);
}

static void test_registered() {
    const char* names[] = {
        "sceWlanGetEtherAddr",
        "sceOpenPSIDGetOpenPSID",
        "sceHprmIsHeadphoneExist",
        "sceUmdCancelWaitDriveStat",
        "scePowerRegisterCallback",
        "NID_0x469989AD",
        "sceNpDrmSetLicenseeKey",
        "sceNpDrmClearLicenseeKey",
        "sceKernelIcacheInvalidateAll",
        "sceKernelDcacheWritebackInvalidateRange",
    };
    for (const char* n : names) {
        CHECK(registry().count(n) == 1, n);
    }
}

static void test_wlan() {
    const uint32_t addr = 0x08810000u;
    std::memset(guest(addr), 0xA5, 6);
    CHECK_EQ(callHle("sceWlanGetEtherAddr", (int32_t)addr), 0,
             "WlanGetEtherAddr valid");
    CHECK(std::memcmp(guest(addr), kMac, 6) == 0, "Wlan MAC bytes");

    std::memset(g_ram, 0x5A, 16);
    CHECK_EQ(callHle("sceWlanGetEtherAddr", 0), ERR_ILLEGAL_ADDR,
             "WlanGetEtherAddr null");
    bool untouched = true;
    for (int i = 0; i < 6; i++) {
        if (g_ram[i] != 0x5A) untouched = false;
    }
    CHECK(untouched, "Wlan null wrote nothing");
}

static void test_openpsid() {
    const uint32_t addr = 0x08820000u;
    std::memset(guest(addr), 0xA5, 16);
    CHECK_EQ(callHle("sceOpenPSIDGetOpenPSID", (int32_t)addr), 0,
             "OpenPSID valid");
    CHECK(std::memcmp(guest(addr), kOpenPsid, 16) == 0, "OpenPSID bytes");
}

static void test_power_callback() {
    const int32_t s0 = callHle("scePowerRegisterCallback", -1, 1);
    const int32_t s1 = callHle("scePowerRegisterCallback", -1, 2);
    CHECK_EQ(s0, 0, "PowerRegisterCallback slot -1 first");
    CHECK_EQ(s1, 1, "PowerRegisterCallback slot -1 second");
    CHECK(s0 != s1, "PowerRegisterCallback slots distinct");
    CHECK_EQ(callHle("scePowerRegisterCallback", 16, 3), ERR_PRIV_REQUIRED,
             "PowerRegisterCallback slot 16");
    CHECK_EQ(callHle("scePowerRegisterCallback", 0, 4), ERR_TAKEN_SLOT,
             "PowerRegisterCallback taken slot");
}

static void test_returns() {
    CHECK_EQ(callHle("sceHprmIsHeadphoneExist"), 0, "HprmIsHeadphoneExist");
    CHECK_EQ(callHle("sceUmdCancelWaitDriveStat"), 0, "UmdCancelWaitDriveStat");
    const uint32_t key = 0x08830000u;
    std::memset(guest(key), 0x11, 16);
    CHECK_EQ(callHle("sceNpDrmSetLicenseeKey", (int32_t)key), 0,
             "NpDrmSetLicenseeKey");
    CHECK_EQ(callHle("sceNpDrmSetLicenseeKey", 0), ERR_NPDRM_INVALID_FILE,
             "NpDrmSetLicenseeKey null");
    CHECK_EQ(callHle("sceNpDrmClearLicenseeKey"), 0, "NpDrmClearLicenseeKey");
    CHECK_EQ(callHle("NID_0x469989AD", 222, 111, 222), 0, "NID_0x469989AD");
    CHECK_EQ(callHle("sceKernelIcacheInvalidateAll"), 0,
             "IcacheInvalidateAll");
    CHECK_EQ(callHle("sceKernelDcacheWritebackInvalidateRange",
                     (int32_t)0x08810000u, 64),
             0, "DcacheWritebackInvalidateRange");
    CHECK_EQ(callHle("sceKernelDcacheWritebackInvalidateRange",
                     (int32_t)0x08810000u, -1),
             ERR_INVALID_SIZE, "DcacheWritebackInvalidateRange negative size");
}

int main() {
    g_ram = static_cast<uint8_t*>(std::calloc(1, PSP_MEM_SIZE));
    if (!g_ram) {
        std::fprintf(stderr, "no memory for rdram\n");
        return 2;
    }
    psp_hle_register_misc();
    psp_hle_register_power();
    test_registered();
    test_wlan();
    test_openpsid();
    test_power_callback();
    test_returns();
    std::free(g_ram);
    std::printf("%d checks, %d failures\n", tests_run, failures);
    return failures == 0 ? 0 : 1;
}
