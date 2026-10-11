// sceNet / sceNetAdhoc / sceNetAdhocctl / sceWlan imports for ad hoc play
// over a PRO ad hoc server (runtime/src/net). Argument checks and return
// codes follow PPSSPP Core/HLE/sceNet.cpp and sceNetAdhoc.cpp.
#include "hle/psp_hle.h"
#include "hle/psp_hle_net.h"
#include "psp_cpu.h"
#include "psp_memory.h"
#include "recomp.h"
#include "net/adhoc_config.h"
#include "net/adhocctl.h"
#include "net/guest_callbacks.h"
#include "net/pdp.h"
#include "net/ptp.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

static constexpr uint32_t kAddrMask = 0x07FFFFFFU;
static constexpr uint32_t kNullPage = 0x00010000U;
static constexpr int32_t kAdhocAlreadyInitialized = (int32_t)0x80410713;
static constexpr int32_t kIllegalMemsize = (int32_t)0x800200D2;

static uint8_t* g_rdram = nullptr;
static bool g_adhoc_inited = false;
static int32_t g_net_pool = 0;

static bool valid_range(uint32_t addr, uint32_t len) {
    if (addr < kNullPage) return false;
    const uint64_t off = static_cast<uint64_t>(addr & kAddrMask);
    return off + len <= PSP_MEM_SIZE;
}

const AdhocConfig& psp_net_config() {
    static const AdhocConfig cfg = [] {
        const char* mac_file = std::getenv("PSPRECOMP_ADHOC_MAC_FILE");
        const auto seed = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() ^
            std::chrono::system_clock::now().time_since_epoch().count());
        AdhocConfig c = adhoc_config_load([](const char* n) { return std::getenv(n); },
                                          mac_file ? mac_file : "psprecomp_adhoc_mac.txt", seed);
        std::fprintf(stderr, "[ADHOC] config: server %s:%u nick \"%s\" mac %s port offset %u\n",
                     c.server_host.c_str(), c.server_port, c.nickname.c_str(),
                     adhoc_format_mac(c.mac).c_str(), c.port_offset);
        return c;
    }();
    return cfg;
}

static void install_notifier() {
    static std::once_flag once;
    std::call_once(once, [] {
        adhocctl::instance().set_notifier([](const adhocctl::Handler& h, int event, int error) {
            guest_callbacks_post(g_rdram, h.entry, static_cast<uint32_t>(event),
                                 static_cast<uint32_t>(error), h.arg);
        });
    });
}

// sceNetInit(poolSize, calloutPri, calloutStack, netinitPri, netinitStack)
static void hle_sceNetInit(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t pool = static_cast<uint32_t>(ctx->r[4]);
    if (pool == 0) {
        ctx->r[2] = kIllegalMemsize;
        return;
    }
    g_net_pool = static_cast<int32_t>(pool);
    ctx->r[2] = SCE_OK;
}

static void hle_sceNetTerm(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r[2] = SCE_OK;
}

static void hle_sceNetAdhocInit(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    if (g_adhoc_inited) {
        ctx->r[2] = kAdhocAlreadyInitialized;
        return;
    }
    g_adhoc_inited = true;
    ctx->r[2] = SCE_OK;
}

static void hle_sceNetAdhocTerm(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    g_adhoc_inited = false;
    ctx->r[2] = SCE_OK;
}

// sceNetAdhocctlInit(stackSize, prio, SceNetAdhocctlAdhocId* {s32 type; char product[9]})
static void hle_sceNetAdhocctlInit(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    const uint32_t id_addr = static_cast<uint32_t>(ctx->r[6]);
    std::string product;
    if (valid_range(id_addr, 4 + 9)) {
        const char* p = reinterpret_cast<const char*>(rdram + ((id_addr + 4) & kAddrMask));
        product.assign(p, strnlen(p, 9));
    }
    install_notifier();
    const AdhocConfig& cfg = psp_net_config();
    int32_t rc;
    {
        PspCpuReleaseScope release;  // the server login can take a moment
        rc = adhocctl::instance().init(cfg, product);
    }
    std::fprintf(stderr, "[ADHOC] sceNetAdhocctlInit(product=%s) -> 0x%08X\n", product.c_str(),
                 static_cast<uint32_t>(rc));
    ctx->r[2] = rc;
}

static void hle_sceNetAdhocctlTerm(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    {
        PspCpuReleaseScope release;
        adhocctl::instance().term();
    }
    ctx->r[2] = SCE_OK;
}

static void hle_sceNetAdhocctlAddHandler(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t entry = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t arg = static_cast<uint32_t>(ctx->r[5]);
    const int rc = adhocctl::instance().add_handler(entry, arg, valid_range(entry, 4));
    std::fprintf(stderr, "[ADHOC] sceNetAdhocctlAddHandler(0x%08X, 0x%08X) -> %d\n", entry, arg, rc);
    ctx->r[2] = rc;
}

static void hle_sceNetAdhocctlDelHandler(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r[2] = adhocctl::instance().del_handler(static_cast<int>(ctx->r[4]));
}

static void hle_sceWlanGetSwitchState(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r[2] = 1;  // switch on: ad hoc goes through the PRO server
}

static void hle_sceWlanGetEtherAddr(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
    if (!valid_range(addr, 6)) {
        ctx->r[2] = SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        return;
    }
    std::memcpy(rdram + (addr & kAddrMask), psp_net_config().mac.data(), 6);
    ctx->r[2] = SCE_OK;
}

static void hle_sceNetEtherNtostr(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mac_addr = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t buf_addr = static_cast<uint32_t>(ctx->r[5]);
    if (valid_range(mac_addr, 6) && valid_range(buf_addr, 18)) {
        const uint8_t* m = rdram + (mac_addr & kAddrMask);
        std::snprintf(reinterpret_cast<char*>(rdram + (buf_addr & kAddrMask)), 18,
                      "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    ctx->r[2] = 0;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hle_sceNetEtherStrton(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t buf_addr = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t mac_addr = static_cast<uint32_t>(ctx->r[5]);
    if (valid_range(buf_addr, 1) && valid_range(mac_addr, 6)) {
        const char* s = reinterpret_cast<const char*>(rdram + (buf_addr & kAddrMask));
        uint8_t* m = rdram + (mac_addr & kAddrMask);
        // PPSSPP: six hex pairs, one separator skipped between them.
        for (int i = 0; i < 6 && *s != 0; ++i) {
            uint8_t v = 0;
            int c = hex_digit(*s++);
            if (c >= 0) v |= static_cast<uint8_t>(c << 4);
            if (*s == 0) { *m++ = v; break; }
            c = hex_digit(*s++);
            if (c >= 0) v |= static_cast<uint8_t>(c);
            *m++ = v;
            if (*s++ == 0) break;
        }
    }
    ctx->r[2] = 0;
}

static void hle_sceNetGetMallocStat(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
    if (!valid_range(addr, 12)) {
        ctx->r[2] = 0;
        return;
    }
    // PPSSPP: pool = poolSize - 0x20, a dummy maximum, everything free.
    const int32_t pool = g_net_pool > 0x20 ? g_net_pool - 0x20 : 0x20000 - 0x20;
    psp_mem_write<int32_t>(rdram, addr + 0, pool);
    psp_mem_write<int32_t>(rdram, addr + 4, 0x4050);
    psp_mem_write<int32_t>(rdram, addr + 8, pool);
    ctx->r[2] = SCE_OK;
}

// ---- PDP (datagrams), PPSSPP sceNetAdhocPdp* ----
static constexpr int32_t kAdhocNotInitialized = (int32_t)0x80410712;

void psp_net_wire_pdp() {
    adhocctl::Adhocctl& ac = adhocctl::instance();
    pdp::Resolver r;
    r.peers = [] {
        std::vector<pdp::Peer> out;
        for (const auto& p : adhocctl::instance().peers()) out.push_back({p.mac, p.ip});
        return out;
    };
    r.bind_ip = ac.inited() ? ac.bind_ip() : psp_net_config().bind_ip;
    r.port_offset = ac.inited() ? ac.port_offset() : psp_net_config().port_offset;
    r.relay = psp_net_config().relay;
    r.relay_host = psp_net_config().server_host;
    r.relay_port = psp_net_config().relay_port;
    r.self_mac = psp_net_config().mac;
    ptp::instance().set_resolver(r);
    pdp::instance().set_resolver(std::move(r));
}

// sceNetAdhocPdpCreate(mac*, port, bufsize, flag)
static void hle_sceNetAdhocPdpCreate(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mac_addr = static_cast<uint32_t>(ctx->r[4]);
    const auto port = static_cast<uint16_t>(ctx->r[5]);
    const int32_t bufsize = ctx->r[6];
    if (!g_adhoc_inited) {
        ctx->r[2] = kAdhocNotInitialized;
        return;
    }
    if (!valid_range(mac_addr, 6) || bufsize <= 0) {
        ctx->r[2] = pdp::kErrInvalidArg;
        return;
    }
    std::memcpy(rdram + (mac_addr & kAddrMask), psp_net_config().mac.data(), 6);
    psp_net_wire_pdp();
    const int rc = pdp::instance().create(port, bufsize);
    std::fprintf(stderr, "[ADHOC] sceNetAdhocPdpCreate(port=%u, buf=%d) -> %d\n", port, bufsize, rc);
    ctx->r[2] = rc;
}

static void hle_sceNetAdhocPdpDelete(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r[2] = g_adhoc_inited ? pdp::instance().remove(ctx->r[4]) : kAdhocNotInitialized;
}

// sceNetAdhocPdpSend(id, mac*, port, data*, len, timeout, flag)
static void hle_sceNetAdhocPdpSend(uint8_t* rdram, recomp_context* ctx) {
    const int id = ctx->r[4];
    const uint32_t mac_addr = static_cast<uint32_t>(ctx->r[5]);
    const auto port = static_cast<uint16_t>(ctx->r[6]);
    const uint32_t data = static_cast<uint32_t>(ctx->r[7]);
    const int32_t len = ctx->r[8];
    if (!g_adhoc_inited) {
        ctx->r[2] = kAdhocNotInitialized;
        return;
    }
    if (!valid_range(mac_addr, 6)) {
        ctx->r[2] = pdp::kErrInvalidAddr;
        return;
    }
    if (len > 0 && !valid_range(data, static_cast<uint32_t>(len))) {
        ctx->r[2] = pdp::kErrInvalidArg;
        return;
    }
    pro::Mac to;
    std::memcpy(to.data(), rdram + (mac_addr & kAddrMask), 6);
    ctx->r[2] = pdp::instance().send(id, to, port, len > 0 ? rdram + (data & kAddrMask) : nullptr, len);
}

// sceNetAdhocPdpRecv(id, mac*, port* (u16), buf*, len* (s32), timeout_us, flag)
static void hle_sceNetAdhocPdpRecv(uint8_t* rdram, recomp_context* ctx) {
    const int id = ctx->r[4];
    const uint32_t mac_addr = static_cast<uint32_t>(ctx->r[5]);
    const uint32_t port_addr = static_cast<uint32_t>(ctx->r[6]);
    const uint32_t buf_addr = static_cast<uint32_t>(ctx->r[7]);
    const uint32_t len_addr = static_cast<uint32_t>(ctx->r[8]);
    const uint32_t timeout = static_cast<uint32_t>(ctx->r[9]);
    const bool nonblock = ctx->r[10] != 0;
    if (!g_adhoc_inited) {
        ctx->r[2] = kAdhocNotInitialized;
        return;
    }
    if (!valid_range(mac_addr, 6) || !valid_range(port_addr, 2) || !valid_range(len_addr, 4)) {
        ctx->r[2] = pdp::kErrInvalidArg;
        return;
    }
    int len = psp_mem_read<int32_t>(rdram, len_addr);
    if (len < 0 || !valid_range(buf_addr, static_cast<uint32_t>(len > 0 ? len : 1))) {
        ctx->r[2] = pdp::kErrInvalidArg;
        return;
    }
    pro::Mac from{};
    uint16_t from_port = 0;
    int rc;
    if (nonblock) {
        rc = pdp::instance().recv(id, &from, &from_port, rdram + (buf_addr & kAddrMask), &len, 0, true);
    } else {
        PspCpuReleaseScope release;
        rc = pdp::instance().recv(id, &from, &from_port, rdram + (buf_addr & kAddrMask), &len, timeout, false);
    }
    if (rc == 0 || rc == pdp::kErrNotEnoughSpace) psp_mem_write<int32_t>(rdram, len_addr, len);
    if (rc == 0) {
        std::memcpy(rdram + (mac_addr & kAddrMask), from.data(), 6);
        psp_mem_write<uint16_t>(rdram, port_addr, from_port);
    }
    ctx->r[2] = rc;
}

// ---- PTP (streams), PPSSPP sceNetAdhocPtp* ----

static bool read_mac(uint8_t* rdram, uint32_t addr, pro::Mac* out) {
    if (!valid_range(addr, 6)) return false;
    std::memcpy(out->data(), rdram + (addr & kAddrMask), 6);
    return true;
}

// PtpOpen(srcmac*, sport, dstmac*, dport, bufsize, rexmt_int, rexmt_cnt, flag)
static void hle_sceNetAdhocPtpOpen(uint8_t* rdram, recomp_context* ctx) {
    const auto src = static_cast<uint32_t>(ctx->r[4]);
    const auto sport = static_cast<uint16_t>(ctx->r[5]);
    const auto dst = static_cast<uint32_t>(ctx->r[6]);
    const auto dport = static_cast<uint16_t>(ctx->r[7]);
    const int32_t bufsize = ctx->r[8];
    if (!g_adhoc_inited) { ctx->r[2] = kAdhocNotInitialized; return; }
    pro::Mac peer{};
    if (!valid_range(src, 6) || !read_mac(rdram, dst, &peer)) { ctx->r[2] = ptp::kErrInvalidAddr; return; }
    std::memcpy(rdram + (src & kAddrMask), psp_net_config().mac.data(), 6);
    psp_net_wire_pdp();
    const int rc = ptp::instance().open(sport, peer, dport, bufsize);
    std::fprintf(stderr, "[ADHOC] sceNetAdhocPtpOpen(sport=%u -> %02x:%02x:%02x:%02x:%02x:%02x:%u) -> %d\n", sport,
                 peer[0], peer[1], peer[2], peer[3], peer[4], peer[5], dport, rc);
    ctx->r[2] = rc;
}

// PtpListen(srcmac*, sport, bufsize, rexmt_int, rexmt_cnt, backlog, flag)
static void hle_sceNetAdhocPtpListen(uint8_t* rdram, recomp_context* ctx) {
    const auto src = static_cast<uint32_t>(ctx->r[4]);
    const auto sport = static_cast<uint16_t>(ctx->r[5]);
    const int32_t bufsize = ctx->r[6];
    const int32_t backlog = ctx->r[9];
    if (!g_adhoc_inited) { ctx->r[2] = kAdhocNotInitialized; return; }
    if (!valid_range(src, 6)) { ctx->r[2] = ptp::kErrInvalidAddr; return; }
    std::memcpy(rdram + (src & kAddrMask), psp_net_config().mac.data(), 6);
    psp_net_wire_pdp();
    const int rc = ptp::instance().listen(sport, bufsize, backlog);
    std::fprintf(stderr, "[ADHOC] sceNetAdhocPtpListen(sport=%u) -> %d\n", sport, rc);
    ctx->r[2] = rc;
}

// PtpAccept(id, mac*, port* (u16), timeout, flag)
static void hle_sceNetAdhocPtpAccept(uint8_t* rdram, recomp_context* ctx) {
    const int id = ctx->r[4];
    const auto mac_addr = static_cast<uint32_t>(ctx->r[5]);
    const auto port_addr = static_cast<uint32_t>(ctx->r[6]);
    const auto timeout = static_cast<uint32_t>(ctx->r[7]);
    const bool nonblock = ctx->r[8] != 0;
    if (!g_adhoc_inited) { ctx->r[2] = kAdhocNotInitialized; return; }
    pro::Mac peer{};
    uint16_t port = 0;
    int rc;
    if (nonblock) {
        rc = ptp::instance().accept(id, &peer, &port, 0, true);
    } else {
        PspCpuReleaseScope release;
        rc = ptp::instance().accept(id, &peer, &port, timeout, false);
    }
    if (rc > 0) {
        if (valid_range(mac_addr, 6)) std::memcpy(rdram + (mac_addr & kAddrMask), peer.data(), 6);
        if (valid_range(port_addr, 2)) psp_mem_write<uint16_t>(rdram, port_addr, port);
        std::fprintf(stderr, "[ADHOC] sceNetAdhocPtpAccept(%d) -> %d from %02x:%02x:%02x:%02x:%02x:%02x:%u\n", id, rc,
                     peer[0], peer[1], peer[2], peer[3], peer[4], peer[5], port);
    }
    ctx->r[2] = rc;
}

// PtpConnect(id, timeout, flag)
static void hle_sceNetAdhocPtpConnect(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const int id = ctx->r[4];
    const auto timeout = static_cast<uint32_t>(ctx->r[5]);
    const bool nonblock = ctx->r[6] != 0;
    if (!g_adhoc_inited) { ctx->r[2] = kAdhocNotInitialized; return; }
    int rc;
    if (nonblock) {
        rc = ptp::instance().connect(id, 0, true);
    } else {
        PspCpuReleaseScope release;
        rc = ptp::instance().connect(id, timeout, false);
    }
    if (rc != ptp::kErrWouldBlock) std::fprintf(stderr, "[ADHOC] sceNetAdhocPtpConnect(%d) -> 0x%08X\n", id, (uint32_t)rc);
    ctx->r[2] = rc;
}

// PtpSend/PtpRecv(id, data*, len* (s32), timeout, flag)
static void ptp_io(uint8_t* rdram, recomp_context* ctx, bool send) {
    const int id = ctx->r[4];
    const auto data = static_cast<uint32_t>(ctx->r[5]);
    const auto len_addr = static_cast<uint32_t>(ctx->r[6]);
    const auto timeout = static_cast<uint32_t>(ctx->r[7]);
    const bool nonblock = ctx->r[8] != 0;
    if (!g_adhoc_inited) { ctx->r[2] = kAdhocNotInitialized; return; }
    if (!valid_range(len_addr, 4)) { ctx->r[2] = ptp::kErrInvalidArg; return; }
    int len = psp_mem_read<int32_t>(rdram, len_addr);
    if (len <= 0 || !valid_range(data, static_cast<uint32_t>(len))) { ctx->r[2] = ptp::kErrInvalidArg; return; }
    uint8_t* p = rdram + (data & kAddrMask);
    int rc;
    if (nonblock) {
        rc = send ? ptp::instance().send(id, p, &len, 0, true) : ptp::instance().recv(id, p, &len, 0, true);
    } else {
        PspCpuReleaseScope release;
        rc = send ? ptp::instance().send(id, p, &len, timeout, false) : ptp::instance().recv(id, p, &len, timeout, false);
    }
    if (rc == 0) psp_mem_write<int32_t>(rdram, len_addr, len);
    ctx->r[2] = rc;
}

static void hle_sceNetAdhocPtpSend(uint8_t* rdram, recomp_context* ctx) { ptp_io(rdram, ctx, true); }
static void hle_sceNetAdhocPtpRecv(uint8_t* rdram, recomp_context* ctx) { ptp_io(rdram, ctx, false); }

static void hle_sceNetAdhocPtpFlush(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r[2] = g_adhoc_inited ? ptp::instance().flush(ctx->r[4]) : kAdhocNotInitialized;
}

static void hle_sceNetAdhocPtpClose(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r[2] = g_adhoc_inited ? ptp::instance().close(ctx->r[4]) : kAdhocNotInitialized;
}

void psp_hle_register_net() {
    psp_hle_register("sceNetInit", hle_sceNetInit);
    psp_hle_register("sceNetTerm", hle_sceNetTerm);
    psp_hle_register("sceNetAdhocInit", hle_sceNetAdhocInit);
    psp_hle_register("sceNetAdhocTerm", hle_sceNetAdhocTerm);
    psp_hle_register("sceNetAdhocctlInit", hle_sceNetAdhocctlInit);
    psp_hle_register("sceNetAdhocctlTerm", hle_sceNetAdhocctlTerm);
    psp_hle_register("sceNetAdhocctlAddHandler", hle_sceNetAdhocctlAddHandler);
    psp_hle_register("sceNetAdhocctlDelHandler", hle_sceNetAdhocctlDelHandler);
    psp_hle_register("sceWlanGetSwitchState", hle_sceWlanGetSwitchState);
    psp_hle_misc_set_mac_source([]() -> const uint8_t* { return psp_net_config().mac.data(); });
    psp_hle_register("sceWlanGetEtherAddr", hle_sceWlanGetEtherAddr);
    psp_hle_register("sceNetEtherNtostr", hle_sceNetEtherNtostr);
    psp_hle_register("sceNetEtherStrton", hle_sceNetEtherStrton);
    psp_hle_register("sceNetGetMallocStat", hle_sceNetGetMallocStat);
    psp_hle_register("sceNetAdhocPdpCreate", hle_sceNetAdhocPdpCreate);
    psp_hle_register("sceNetAdhocPdpDelete", hle_sceNetAdhocPdpDelete);
    psp_hle_register("sceNetAdhocPdpSend", hle_sceNetAdhocPdpSend);
    psp_hle_register("sceNetAdhocPdpRecv", hle_sceNetAdhocPdpRecv);
    psp_hle_register("sceNetAdhocPtpOpen", hle_sceNetAdhocPtpOpen);
    psp_hle_register("sceNetAdhocPtpListen", hle_sceNetAdhocPtpListen);
    psp_hle_register("sceNetAdhocPtpAccept", hle_sceNetAdhocPtpAccept);
    psp_hle_register("sceNetAdhocPtpConnect", hle_sceNetAdhocPtpConnect);
    psp_hle_register("sceNetAdhocPtpSend", hle_sceNetAdhocPtpSend);
    psp_hle_register("sceNetAdhocPtpRecv", hle_sceNetAdhocPtpRecv);
    psp_hle_register("sceNetAdhocPtpFlush", hle_sceNetAdhocPtpFlush);
    psp_hle_register("sceNetAdhocPtpClose", hle_sceNetAdhocPtpClose);
    psp_hle_register_net_matching();
}
