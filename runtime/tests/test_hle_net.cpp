// Network HLE imports (psp_hle_net.cpp) through a stub registry against a
// fake guest RAM, like test_hle_misc.cpp. Oracle: PPSSPP Core/HLE/sceNet.cpp
// and sceNetAdhoc.cpp. No ad hoc server runs: the configured server port is
// closed, so sceNetAdhocctlInit must still succeed quickly.
#include "hle/psp_hle.h"
#include "hle/psp_hle_intr.h"
#include "hle/psp_hle_net.h"
#include "net/guest_callbacks.h"
#include "psp_memory.h"
#include "recomp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

// psp_hle_misc.cpp's sceKernelExitGame sets it (psp_event_loop.cpp in the runtime).
std::atomic<bool> g_should_exit{false};

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// ---- stubs for what psp_hle_net.cpp links against ----
static std::map<std::string, HleFunc>& registry() {
    static std::map<std::string, HleFunc> r;
    return r;
}
void psp_hle_register(const char* nid_name, HleFunc fn) { registry()[nid_name] = fn; }
void psp_cpu_acquire(bool) {}
void psp_cpu_release_reserved() {}
PspFastForward& psp_display_fast_forward() {
    static PspFastForward ff;
    return ff;
}
bool psp_cpu_held() { return false; }
static int g_posted = 0;
void guest_callbacks_post(uint8_t*, GuestCall) { g_posted++; }

static uint8_t* g_ram = nullptr;

static int32_t call(const char* name, int32_t a0 = 0, int32_t a1 = 0, int32_t a2 = 0) {
    auto it = registry().find(name);
    if (it == registry().end()) {
        std::fprintf(stderr, "FAIL: %s not registered\n", name);
        failures++;
        return 0x7FFFFFFF;
    }
    recomp_context ctx{};
    ctx.r[4] = a0;
    ctx.r[5] = a1;
    ctx.r[6] = a2;
    it->second(g_ram, &ctx);
    return ctx.r[2];
}

static uint8_t* guest(uint32_t addr) { return g_ram + (addr & 0x07FFFFFFu); }

static void wlan() {
    CHECK(call("sceWlanGetSwitchState") == 1, "switch on");
    const uint32_t addr = 0x08810000u;
    std::memset(guest(addr), 0xA5, 6);
    CHECK(call("sceWlanGetEtherAddr", (int32_t)addr) == 0, "GetEtherAddr ok");
    const uint8_t want[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0a};
    CHECK(std::memcmp(guest(addr), want, 6) == 0, "MAC from PSPRECOMP_ADHOC_MAC");
    std::memset(g_ram, 0x5A, 16);
    CHECK(call("sceWlanGetEtherAddr", 0) == (int32_t)0x800200D3, "null address rejected");
    CHECK(g_ram[0] == 0x5A, "null address untouched");
}

static void ether_strings() {
    const uint32_t mac = 0x08811000u, buf = 0x08811100u, back = 0x08811200u;
    const uint8_t m[6] = {0x00, 0x1D, 0xD9, 0xAB, 0x0C, 0xFF};
    std::memcpy(guest(mac), m, 6);
    call("sceNetEtherNtostr", (int32_t)mac, (int32_t)buf);
    CHECK(std::strcmp(reinterpret_cast<char*>(guest(buf)), "00:1d:d9:ab:0c:ff") == 0, "Ntostr");
    call("sceNetEtherStrton", (int32_t)buf, (int32_t)back);
    CHECK(std::memcmp(guest(back), m, 6) == 0, "Strton round trip");
}

static void malloc_stat() {
    CHECK(call("sceNetInit", 0) == (int32_t)0x800200D2, "zero pool rejected");
    CHECK(call("sceNetInit", 0x20000, 0x20, 0x1000) == 0, "NetInit");
    const uint32_t st = 0x08812000u;
    CHECK(call("sceNetGetMallocStat", (int32_t)st) == 0, "GetMallocStat");
    int32_t v[3];
    std::memcpy(v, guest(st), 12);
    CHECK(v[0] == 0x1FFE0 && v[1] == 0x4050 && v[2] == 0x1FFE0, "pool - 0x20, dummy max, all free");
}

static void adhoc_lifecycle() {
    CHECK(call("sceNetAdhocInit") == 0, "AdhocInit");
    CHECK(call("sceNetAdhocInit") == (int32_t)0x80410713, "AdhocInit twice");
    const uint32_t id = 0x08813000u;
    std::memset(guest(id), 0, 16);
    std::memcpy(guest(id + 4), "UCES01421", 9);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(call("sceNetAdhocctlInit", 0x2000, 0x30, (int32_t)id) == 0, "AdhocctlInit without a server");
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4), "returns promptly");
    CHECK(call("sceNetAdhocctlInit", 0x2000, 0x30, (int32_t)id) == (int32_t)0x80410B07, "AdhocctlInit twice");
    psp_display_fast_forward().request(4);
    CHECK(!psp_display_fast_forward().active(), "ad hoc session: no display fast-forward");
    CHECK(call("sceNetAdhocctlAddHandler", 0x08900000, 0x77) == 0, "AddHandler id 0");
    CHECK(call("sceNetAdhocctlAddHandler", 0, 0) == (int32_t)0x80410B04, "AddHandler null");
    CHECK(call("sceNetAdhocctlDelHandler", 0) == 0, "DelHandler");
    CHECK(call("sceNetAdhocctlTerm") == 0, "AdhocctlTerm");
    psp_display_fast_forward().request(4);
    CHECK(psp_display_fast_forward().active(), "after the session: fast-forward allowed again");
    CHECK(call("sceNetAdhocctlDelHandler", 0) == (int32_t)0x80410B08, "DelHandler after term");
    CHECK(call("sceNetAdhocTerm") == 0 && call("sceNetAdhocInit") == 0, "AdhocTerm allows re-init");
    CHECK(psp_net_config().nickname == "Tester", "nickname from env");
}


static int32_t call7(const char* name, int32_t a0, int32_t a1, int32_t a2 = 0, int32_t a3 = 0, int32_t a4 = 0,
                     int32_t a5 = 0, int32_t a6 = 0, int32_t a7 = 0) {
    auto it = registry().find(name);
    if (it == registry().end()) {
        failures++;
        return 0x7FFFFFFF;
    }
    recomp_context ctx{};
    ctx.r[4] = a0; ctx.r[5] = a1; ctx.r[6] = a2; ctx.r[7] = a3;
    ctx.r[8] = a4; ctx.r[9] = a5; ctx.r[10] = a6; ctx.r[11] = a7;
    it->second(g_ram, &ctx);
    return ctx.r[2];
}

static void pdp_hle() {
    const uint32_t mac = 0x08814000u, buf = 0x08814100u, port = 0x08814200u, len = 0x08814300u;
    CHECK(call7("sceNetAdhocPdpCreate", 0, 300, 8192, 0, 0) == (int32_t)0x80410711, "PdpCreate null mac");
    const int id = call7("sceNetAdhocPdpCreate", (int32_t)mac, 31300, 8192, 0, 0);
    CHECK(id >= 1, "PdpCreate");
    const uint8_t want[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0a};
    CHECK(std::memcmp(guest(mac), want, 6) == 0, "PdpCreate writes the local MAC");
    std::memset(guest(len), 0, 4);
    *reinterpret_cast<int32_t*>(guest(len)) = 64;
    CHECK(call7("sceNetAdhocPdpRecv", id, (int32_t)mac, (int32_t)port, (int32_t)buf, (int32_t)len, 0, 1) ==
              (int32_t)0x80410709, "PdpRecv nonblocking, nothing queued");
    CHECK(call7("sceNetAdhocPdpRecv", id, (int32_t)mac, (int32_t)port, (int32_t)buf, (int32_t)len, 100000, 0) ==
              (int32_t)0x80410715, "PdpRecv blocking times out");
    CHECK(call7("sceNetAdhocPdpRecv", id, 0, (int32_t)port, (int32_t)buf, (int32_t)len, 0, 1) ==
              (int32_t)0x80410711, "PdpRecv null mac");
    CHECK(call7("sceNetAdhocPdpSend", id, (int32_t)mac, 0, (int32_t)buf, 4, 0, 1) == (int32_t)0x80410703,
          "PdpSend port 0");
    CHECK(call7("sceNetAdhocPdpDelete", id, 0, 0, 0, 0) == 0, "PdpDelete");
    CHECK(call7("sceNetAdhocPdpDelete", id, 0, 0, 0, 0) == (int32_t)0x80410701, "PdpDelete twice");
}


static void matching_hle() {
    CHECK(call7("sceNetAdhocMatchingCreate", 3, 2, 31400, 4096, 0) == (int32_t)0x80410813,
          "Create before Init");
    CHECK(call7("sceNetAdhocMatchingInit", 0x20000, 0, 0, 0, 0) == 0, "MatchingInit");
    CHECK(call7("sceNetAdhocMatchingInit", 0x20000, 0, 0, 0, 0) == (int32_t)0x80410812, "MatchingInit twice");
    CHECK(call7("sceNetAdhocMatchingCreate", 3, 1, 31400, 4096, 0) == (int32_t)0x80410803, "maxnum 1");
    CHECK(call7("sceNetAdhocMatchingCreate", 3, 2, 31400, 0, 0) == (int32_t)0x80410804, "rxbuf 0");
    CHECK(call7("sceNetAdhocMatchingCreate", 4, 2, 31400, 4096, 0) == (int32_t)0x80410806, "mode 4");
    // The 9th argument (handler) is on the stack at sp+0, not in $t4.
    const uint32_t sp = 0x08815F00u;
    *reinterpret_cast<uint32_t*>(guest(sp)) = 0x089BC808u;
    int id = 0;
    {
        recomp_context ctx{};
        const int32_t args[8] = {3, 2, 31400, 4096, 200000, 300000, 3, 0};
        for (int i = 0; i < 8; i++) ctx.r[4 + i] = args[i];
        ctx.r[12] = 0x1234;
        ctx.r[29] = static_cast<int32_t>(sp);
        registry()["sceNetAdhocMatchingCreate"](g_ram, &ctx);
        id = ctx.r[2];
    }
    CHECK(id >= 1, "Create P2P");
    CHECK(psp_net_matching_handler(id) == 0x089BC808u, "handler read from the stack");
    CHECK(call7("sceNetAdhocMatchingCreate", 1, 4, 31400, 4096, 0) == (int32_t)0x80410814, "same port twice");
    const uint32_t mac = 0x08815000u, size = 0x08815100u, buf = 0x08815200u;
    std::memset(guest(mac), 0x11, 6);
    CHECK(call7("sceNetAdhocMatchingSelectTarget", id, (int32_t)mac, 0, 0) == (int32_t)0x8041080B,
          "SelectTarget before Start: not running");
    CHECK(call7("sceNetAdhocMatchingStart", id, 0x10, 0x2000, 0x10, 0x2000, 0, 0) == 0, "Start");
    CHECK(call7("sceNetAdhocMatchingSelectTarget", id, (int32_t)mac, 0, 0) == (int32_t)0x8041080C,
          "unknown target");
    *reinterpret_cast<int32_t*>(guest(size)) = 0;
    CHECK(call7("sceNetAdhocMatchingGetMembers", id, (int32_t)size, 0) == 0 &&
              *reinterpret_cast<int32_t*>(guest(size)) == 12, "GetMembers size: self only");
    std::memset(guest(buf), 0xEE, 24);
    CHECK(call7("sceNetAdhocMatchingGetMembers", id, (int32_t)size, (int32_t)buf) == 0, "GetMembers");
    const uint8_t self[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0a};
    CHECK(*reinterpret_cast<uint32_t*>(guest(buf)) == 0 && std::memcmp(guest(buf + 4), self, 6) == 0,
          "member entry: next 0, own MAC");
    CHECK(call7("sceNetAdhocMatchingSetHelloOpt", id, 4, (int32_t)buf) == 0, "SetHelloOpt");
    CHECK(call7("sceNetAdhocMatchingStop", id, 0, 0, 0, 0) == 0, "Stop");
    CHECK(call7("sceNetAdhocMatchingDelete", id, 0, 0, 0, 0) == 0, "Delete");
    CHECK(call7("sceNetAdhocMatchingGetMembers", id, (int32_t)size, 0) == (int32_t)0x80410807, "deleted id");
    CHECK(call7("sceNetAdhocMatchingTerm", 0, 0, 0, 0, 0) == 0, "Term");
}


static void ptp_hle() {
    const uint32_t src = 0x08816000u, dst = 0x08816010u, data = 0x08816100u, len = 0x08816200u;
    std::memset(guest(dst), 0, 6);
    guest(dst)[5] = 0x0b;  // not a group peer
    const int s = call7("sceNetAdhocPtpOpen", (int32_t)src, 31500, (int32_t)dst, 31500, 8192, 200000, 5, 0);
    CHECK(s >= 1, "PtpOpen");
    const uint8_t want[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x0a};
    CHECK(std::memcmp(guest(src), want, 6) == 0, "PtpOpen writes the local MAC");
    CHECK(call7("sceNetAdhocPtpConnect", s, 100000, 0) == (int32_t)0x80410702, "connect to a non-peer");
    *reinterpret_cast<int32_t*>(guest(len)) = 4;
    CHECK(call7("sceNetAdhocPtpSend", s, (int32_t)data, (int32_t)len, 0, 1) == (int32_t)0x8041070B,
          "send before connect");
    const int l = call7("sceNetAdhocPtpListen", (int32_t)src, 31501, 8192, 200000, 5, 2, 0);
    CHECK(l >= 1 && l != s, "PtpListen");
    CHECK(call7("sceNetAdhocPtpAccept", l, (int32_t)dst, (int32_t)len, 0, 1) == (int32_t)0x80410709,
          "accept nonblocking: nobody");
    CHECK(call7("sceNetAdhocPtpClose", s, 0) == 0 && call7("sceNetAdhocPtpClose", l, 0) == 0, "PtpClose");
    CHECK(call7("sceNetAdhocPtpClose", s, 0) == (int32_t)0x80410701, "close twice");
}

int main() {
#ifdef _WIN32
    _putenv_s("PSPRECOMP_ADHOC_MAC", "02:00:00:00:00:0a");
    _putenv_s("PSPRECOMP_ADHOC_SERVER", "127.0.0.1:1");
    _putenv_s("PSPRECOMP_ADHOC_NICK", "Tester");
    _putenv_s("PSPRECOMP_ADHOC_MAC_FILE", "test_hle_net_mac.txt");
#else
    setenv("PSPRECOMP_ADHOC_MAC", "02:00:00:00:00:0a", 1);
    setenv("PSPRECOMP_ADHOC_SERVER", "127.0.0.1:1", 1);
    setenv("PSPRECOMP_ADHOC_NICK", "Tester", 1);
    setenv("PSPRECOMP_ADHOC_MAC_FILE", "test_hle_net_mac.txt", 1);
#endif
    g_ram = static_cast<uint8_t*>(std::calloc(PSP_MEM_SIZE, 1));
    psp_hle_register_net();
    wlan();
    ether_strings();
    malloc_stat();
    adhoc_lifecycle();
    pdp_hle();
    matching_hle();
    ptp_hle();
    std::free(g_ram);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("hle_net: all passed\n");
    return 0;
}
