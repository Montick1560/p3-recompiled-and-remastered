// sceNp / sceNpAuth / sceNpService / sceNetApctl imports for the game's
// infrastructure mode. There is no PSN: sign-in always succeeds and the auth
// ticket is a local dummy, after PPSSPP Core/HLE/sceNp.cpp and sceNet.cpp
// (GPL-2.0-or-later). RECON: only reached with PSPRECOMP_INFRA_RECON set,
// since the netconf dialog aborts infrastructure actions otherwise.
#include "hle/psp_hle.h"
#include "hle/psp_hle_np.h"
#include "psp_memory.h"
#include "recomp.h"
#include "net/guest_callbacks.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static constexpr uint32_t kAddrMask = 0x07FFFFFFU;
static constexpr uint32_t kNullPage = 0x00010000U;

static bool valid_range(uint32_t addr, uint32_t len) {
    if (addr < kNullPage) return false;
    return static_cast<uint64_t>(addr & kAddrMask) + len <= PSP_MEM_SIZE;
}

static void log_call(const char* name, const recomp_context* ctx) {
    std::fprintf(stderr, "[NP] %s(a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X) ra=%08X\n", name,
                 (uint32_t)ctx->r[4], (uint32_t)ctx->r[5], (uint32_t)ctx->r[6], (uint32_t)ctx->r[7],
                 (uint32_t)ctx->r[8], (uint32_t)ctx->r[9], (uint32_t)ctx->r[31]);
}

static std::string online_id() {
    const char* n = std::getenv("PSPRECOMP_ADHOC_NICK");
    std::string s = n && *n ? n : "Patapon";
    if (s.size() > 16) s.resize(16);
    return s;
}

static void write_str(uint8_t* rdram, uint32_t addr, const std::string& s, uint32_t field) {
    std::memset(rdram + (addr & kAddrMask), 0, field);
    std::memcpy(rdram + (addr & kAddrMask), s.data(), s.size() < field ? s.size() : field - 1);
}

// ---- sceNetApctl ----
// GetInfo codes (PPSSPP sceNetApctl.h). STRENGTH is the Wi-Fi signal in %.
static void hle_sceNetApctlGetInfo(uint8_t* rdram, recomp_context* ctx) {
    log_call("sceNetApctlGetInfo", ctx);
    const int code = ctx->r[4];
    const uint32_t out = static_cast<uint32_t>(ctx->r[5]);
    auto str = [&](const char* s, uint32_t len) {
        if (!valid_range(out, len)) return false;
        write_str(rdram, out, s, len);
        return true;
    };
    auto u8v = [&](uint8_t v) {
        if (!valid_range(out, 1)) return false;
        rdram[out & kAddrMask] = v;
        return true;
    };
    auto u32v = [&](uint32_t v) {
        if (!valid_range(out, 4)) return false;
        psp_mem_write<uint32_t>(rdram, out, v);
        return true;
    };
    bool ok = true;
    switch (code) {
    case 0: ok = str("NetConf #1", 64); break;                       // PROFILE_NAME
    case 1: ok = valid_range(out, 6); if (ok) std::memcpy(rdram + (out & kAddrMask), "\1\1\2\2\3\3", 6); break;
    case 2: ok = str("Wifi", 32); break;                             // SSID
    case 3: ok = u32v(4); break;                                     // SSID_LENGTH
    case 4: ok = u32v(0); break;                                     // SECURITY_TYPE
    case 5: ok = u8v(99); break;                                     // STRENGTH
    case 6: ok = u8v(6); break;                                      // CHANNEL
    case 7: ok = u8v(0); break;                                      // POWER_SAVE
    case 8: ok = str("192.168.1.2", 16); break;                      // IP
    case 9: ok = str("255.255.255.0", 16); break;                    // SUBNETMASK
    case 10: ok = str("192.168.1.1", 16); break;                     // GATEWAY
    case 11: ok = str("192.168.1.1", 16); break;                     // PRIMDNS
    case 12: ok = str("0.0.0.0", 16); break;                         // SECDNS
    case 13: ok = u32v(0); break;                                    // USE_PROXY
    default: ok = false; break;
    }
    ctx->r[2] = ok ? SCE_OK : -1;
}

static void hle_ok(const char* name, recomp_context* ctx) {
    log_call(name, ctx);
    ctx->r[2] = SCE_OK;
}

#define NP_OK(fn) static void hle_##fn(uint8_t*, recomp_context* ctx) { hle_ok(#fn, ctx); }
NP_OK(sceNetApctlAddHandler)
NP_OK(sceNetApctlDelHandler)
NP_OK(sceNpInit)
NP_OK(sceNpTerm)
NP_OK(sceNpAuthInit)
NP_OK(sceNpAuthTerm)
NP_OK(sceNpServiceInit)
NP_OK(sceNpServiceTerm)
NP_OK(sceNpAuthAbortRequest)
NP_OK(sceNpAuthDestroyRequest)
#undef NP_OK

// ---- sceNp ----
static void hle_sceNpGetContentRatingFlag(uint8_t* rdram, recomp_context* ctx) {
    log_call("sceNpGetContentRatingFlag", ctx);
    const uint32_t pc = static_cast<uint32_t>(ctx->r[4]), age = static_cast<uint32_t>(ctx->r[5]);
    if (!valid_range(pc, 4) || !valid_range(age, 4)) { ctx->r[2] = (int32_t)0x80550003; return; }
    psp_mem_write<uint32_t>(rdram, pc, 1);   // parental control enabled, like PPSSPP
    psp_mem_write<uint32_t>(rdram, age, 24);
    ctx->r[2] = SCE_OK;
}

static void hle_sceNpGetChatRestrictionFlag(uint8_t* rdram, recomp_context* ctx) {
    log_call("sceNpGetChatRestrictionFlag", ctx);
    const uint32_t f = static_cast<uint32_t>(ctx->r[4]);
    if (!valid_range(f, 4)) { ctx->r[2] = (int32_t)0x80550003; return; }
    psp_mem_write<uint32_t>(rdram, f, 0);
    ctx->r[2] = SCE_OK;
}

static void hle_sceNpGetOnlineId(uint8_t* rdram, recomp_context* ctx) {
    log_call("sceNpGetOnlineId", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[4]);
    if (!valid_range(p, 20)) { ctx->r[2] = (int32_t)0x80550003; return; }
    write_str(rdram, p, online_id(), 20);  // SceNpOnlineId: char data[16] + term + dummy[3]
    ctx->r[2] = SCE_OK;
}

// ---- sceNpAuth ----
// SceNpAuthRequestParameter: size, version, serviceId*, cookie*, cookieSize,
// entitlementId*, consumedCount, ticketCb, cbArg. The callback gets
// (requestId, ticketLength, cbArg) and then calls sceNpAuthGetTicket.
static constexpr uint32_t kTicketLength = 248;
static std::string g_service_id;

static std::string guest_cstr(uint8_t* rdram, uint32_t addr, size_t max = 64) {
    if (!valid_range(addr, 1)) return {};
    const char* s = reinterpret_cast<const char*>(rdram + (addr & kAddrMask));
    return std::string(s, strnlen(s, max));
}

static void hle_sceNpAuthCreateStartRequest(uint8_t* rdram, recomp_context* ctx) {
    log_call("sceNpAuthCreateStartRequest", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[4]);
    if (!valid_range(p, 36)) { ctx->r[2] = (int32_t)0x80550003; return; }
    const uint32_t size = psp_mem_read<uint32_t>(rdram, p);
    g_service_id = guest_cstr(rdram, psp_mem_read<uint32_t>(rdram, p + 8));
    const uint32_t cb = size >= 32 ? psp_mem_read<uint32_t>(rdram, p + 28) : 0;
    const uint32_t arg = size >= 36 ? psp_mem_read<uint32_t>(rdram, p + 32) : 0;
    std::fprintf(stderr, "[NP] auth request: size=%u service=\"%s\" entitlement=\"%s\" cb=%08X arg=%08X\n", size,
                 g_service_id.c_str(), guest_cstr(rdram, psp_mem_read<uint32_t>(rdram, p + 20)).c_str(), cb, arg);
    const uint32_t request_id = 1;  // the game only aborts/destroys ids > 0 (PPSSPP note)
    if (cb) guest_callbacks_post(rdram, cb, request_id, kTicketLength, arg);
    ctx->r[2] = static_cast<int32_t>(request_id);
}

static int put_param(uint8_t* b, uint16_t type, const void* data, uint16_t len) {
    b[0] = type >> 8; b[1] = type & 0xFF; b[2] = len >> 8; b[3] = len & 0xFF;
    if (len && data) std::memcpy(b + 4, data, len);
    else if (len) std::memset(b + 4, 0, len);
    return 4 + len;
}
static int put_u32(uint8_t* b, uint16_t type, uint32_t v) {
    const uint8_t be[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    return put_param(b, type, be, 4);
}
static int put_u64(uint8_t* b, uint16_t type, uint64_t v) {
    uint8_t be[8];
    for (int i = 0; i < 8; i++) be[i] = uint8_t(v >> (56 - 8 * i));
    return put_param(b, type, be, 8);
}

// Dummy X-I-5 ticket v2.1 (big-endian), field for field as PPSSPP builds it.
static void hle_sceNpAuthGetTicket(uint8_t* rdram, recomp_context* ctx) {
    log_call("sceNpAuthGetTicket", ctx);
    const uint32_t buf = static_cast<uint32_t>(ctx->r[5]), len = static_cast<uint32_t>(ctx->r[6]);
    if (!valid_range(buf, len) || len < kTicketLength) { ctx->r[2] = (int32_t)0x80550003; return; }
    uint8_t* t = rdram + (buf & kAddrMask);
    std::memset(t, 0, len);
    uint8_t* body = t + 12;
    int o = 0;
    o += put_param(body + o, 8, "\x4c\x47\x56\x3b\x81\x39\x4a\x22\xd8\x6b\xc1\x57\x71\x6e\xfd\xb8\xab\x63\xcc\x51", 20);
    o += put_u32(body + o, 1, 0x0100);
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    o += put_u64(body + o, 7, now);
    o += put_u64(body + o, 7, now + 10 * 60 * 1000);
    o += put_u64(body + o, 2, 0x592e71c546e86859ULL);
    char id[32] = {};
    std::strncpy(id, online_id().c_str(), sizeof(id) - 1);
    o += put_param(body + o, 4, id, 32);
    o += put_param(body + o, 8, "us\0\2", 4);
    o += put_param(body + o, 4, "c9\0\0", 4);
    char svc[24] = {};
    std::strncpy(svc, g_service_id.c_str(), sizeof(svc) - 1);
    o += put_param(body + o, 8, svc, 24);
    o += put_u32(body + o, 1, 0x200 | (24u << 24));  // parental control on, age 24
    o += put_param(body + o, 0, nullptr, 0);
    o += put_param(body + o, 0, nullptr, 0);
    // header: version 0x21010000, size 0xF0; body section 0x3000 + size
    const uint8_t hdr[12] = {0x21, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0,
                             0x30, 0x00, uint8_t(o >> 8), uint8_t(o & 0xFF)};
    std::memcpy(t, hdr, sizeof(hdr));
    uint8_t* f = body + o;
    f[0] = 0x30; f[1] = 0x02; f[2] = 0x00; f[3] = 32;  // footer section
    int fo = 4;
    fo += put_param(f + fo, 8, "\x34\xcd\x3c\xa9", 4);
    fo += put_param(f + fo, 8, "\x3a\x4b\x42\x66\x92\xda\x6b\x7c\xb7\x4c\xe8\xd9\x4f\x2b\x77\x15\x91\xb8\xa4\xa9", 20);
    ctx->r[2] = static_cast<int32_t>(kTicketLength);
}

static void hle_sceNpAuthGetEntitlementById(uint8_t* rdram, recomp_context* ctx) {
    log_call("sceNpAuthGetEntitlementById", ctx);
    std::fprintf(stderr, "[NP] entitlement \"%s\"\n", guest_cstr(rdram, static_cast<uint32_t>(ctx->r[6])).c_str());
    ctx->r[2] = 1;  // PPSSPP: dummy "owned"
}

static void hle_sceNpAuthGetMemoryStat(uint8_t* rdram, recomp_context* ctx) {
    log_call("sceNpAuthGetMemoryStat", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[4]);
    if (!valid_range(p, 12)) { ctx->r[2] = (int32_t)0x80550003; return; }
    psp_mem_write<uint32_t>(rdram, p, 0x10000);
    psp_mem_write<uint32_t>(rdram, p + 4, 0x4050);
    psp_mem_write<uint32_t>(rdram, p + 8, 0x10000);
    ctx->r[2] = SCE_OK;
}

void psp_hle_register_np() {
    psp_hle_register("sceNetApctlGetInfo", hle_sceNetApctlGetInfo);
    psp_hle_register("sceNetApctlAddHandler", hle_sceNetApctlAddHandler);
    psp_hle_register("sceNetApctlDelHandler", hle_sceNetApctlDelHandler);
    // The generated import table names unknown NIDs NID_0x...: register both.
    struct { uint32_t nid; const char* name; HleFunc fn; } const t[] = {
        {0x857B47D3, "sceNpInit", hle_sceNpInit},
        {0x37E1E274, "sceNpTerm", hle_sceNpTerm},
        {0xBB069A87, "sceNpGetContentRatingFlag", hle_sceNpGetContentRatingFlag},
        {0x1D60AE4B, "sceNpGetChatRestrictionFlag", hle_sceNpGetChatRestrictionFlag},
        {0x4B5C71C8, "sceNpGetOnlineId", hle_sceNpGetOnlineId},
        {0xA1DE86F8, "sceNpAuthInit", hle_sceNpAuthInit},
        {0x4EC1F667, "sceNpAuthTerm", hle_sceNpAuthTerm},
        {0xF4531ADC, "sceNpAuthGetMemoryStat", hle_sceNpAuthGetMemoryStat},
        {0xCD86A656, "sceNpAuthCreateStartRequest", hle_sceNpAuthCreateStartRequest},
        {0x3F1C1F70, "sceNpAuthGetTicket", hle_sceNpAuthGetTicket},
        {0x6900F084, "sceNpAuthGetEntitlementById", hle_sceNpAuthGetEntitlementById},
        {0xD99455DD, "sceNpAuthAbortRequest", hle_sceNpAuthAbortRequest},
        {0x72BB0467, "sceNpAuthDestroyRequest", hle_sceNpAuthDestroyRequest},
        {0x0F8F5821, "sceNpServiceInit", hle_sceNpServiceInit},
        {0x00ACFAC3, "sceNpServiceTerm", hle_sceNpServiceTerm},
    };
    static char names[sizeof(t) / sizeof(t[0])][16];
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        psp_hle_register(t[i].name, t[i].fn);
        std::snprintf(names[i], sizeof(names[i]), "NID_0x%08X", t[i].nid);
        psp_hle_register(names[i], t[i].fn);
    }
}
