// sceNetInet imports: the game's P2P sockets in infrastructure mode, carried
// on the virtual network of the infrastructure server (net/infra_client.h).
// Peer addresses are the virtual IPs 10.80.0.n the server hands out.
//
// Patapon 3 uses the PSP's P2P socket types (its wrapper class at 0x089B62E0):
// type 6, UDP P2P, bound to port 3658 with a virtual port at sockaddr +8,
// sent with MSG_DONTWAIT; and type 10, TCP P2P, nonblocking (SO_NBIO), a
// listener on the game's port plus connecting sockets. UDP datagrams go to
// the server as kind 2 frames keyed by the virtual port; TCP streams as kind
// 3 segments through p2p::Streams. Nothing here blocks: an empty socket fails
// with EAGAIN. Struct layouts and errno values after PPSSPP
// Core/HLE/sceNetInet.cpp (GPL-2.0-or-later).
#include "hle/psp_hle.h"
#include "hle/psp_hle_inet.h"
#include "psp_memory.h"
#include "recomp.h"
#include "net/inet_errno.h"
#include "net/infra_client.h"
#include "net/p2p_stream.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t kAddrMask = 0x07FFFFFFU;
constexpr uint32_t kNullPage = 0x00010000U;
constexpr int kMaxSockets = 32;
constexpr uint16_t kFirstAutoPort = 3658;
constexpr uint32_t kVipNet = 0x0A500000, kVipMask = 0xFFFF0000;  // 10.80.0.0/16
constexpr int kSockDgram = 2, kSockDgramP2P = 6, kSockStreamP2P = 10;
constexpr uint32_t kSoNbio = 0x1009;

bool valid_range(uint32_t addr, uint32_t len) {
    if (addr < kNullPage) return false;
    return static_cast<uint64_t>(addr & kAddrMask) + len <= PSP_MEM_SIZE;
}

bool trace() {
    static const bool on = std::getenv("PSPRECOMP_INFRA_TRACE") != nullptr;
    return on;
}

struct Socket {
    bool used = false;
    int type = 0;
    uint16_t port = 0;   // sockaddr +2 of the bind, host order
    uint16_t vport = 0;  // sockaddr +8 of the bind (P2P), host order
    bool nonblock = false;
    // UDP: the queue datagrams for this socket arrive on (the virtual port for UDP P2P).
    uint16_t key() const { return type == kSockDgramP2P && vport ? vport : port; }
};

struct Out {
    uint32_t vip;
    uint16_t dst, src;
    std::vector<uint8_t> bytes;
};

std::mutex g_mu;
Socket g_socks[kMaxSockets];
uint16_t g_next_port = kFirstAutoPort;
std::vector<Out> g_outbox;  // stream segments to send once g_mu is released
p2p::Streams g_streams([](uint32_t vip, uint16_t dst, uint16_t src, const std::vector<uint8_t>& b) {
    g_outbox.push_back(Out{vip, dst, src, b});
});
thread_local int t_errno = 0;

int fail(recomp_context* ctx, int e) {
    t_errno = e;
    ctx->r[2] = -1;
    return -1;
}

// A p2p::Streams result (bytes or -errno) as the call's return value.
void stream_result(recomp_context* ctx, int r) {
    if (r < 0)
        fail(ctx, -r);
    else
        ctx->r[2] = r;
}

Socket* sock(int s) { return s >= 0 && s < kMaxSockets && g_socks[s].used ? &g_socks[s] : nullptr; }

int new_socket(int type, bool nonblock) {
    for (int i = 0; i < kMaxSockets; i++) {
        if (g_socks[i].used) continue;
        g_socks[i] = Socket{true, type, 0, 0, nonblock};
        return i;
    }
    return -1;
}

// SceNetInetSockaddrIn (P2P form) {u8 len; u8 family; u16 port; u32 addr; u16 vport; u8 zero[6]},
// port, addr and vport in network order.
struct Sockaddr {
    uint32_t ip = 0;
    uint16_t port = 0, vport = 0;
};

bool read_sockaddr(uint8_t* rdram, uint32_t p, Sockaddr* a) {
    if (!valid_range(p, 10)) return false;
    const uint8_t* b = rdram + (p & kAddrMask);
    a->port = static_cast<uint16_t>(b[2] << 8 | b[3]);
    a->ip = static_cast<uint32_t>(b[4]) << 24 | b[5] << 16 | b[6] << 8 | b[7];
    a->vport = static_cast<uint16_t>(b[8] << 8 | b[9]);
    return true;
}

void write_sockaddr(uint8_t* rdram, uint32_t p, const Sockaddr& a) {
    if (!valid_range(p, 16)) return;
    uint8_t* b = rdram + (p & kAddrMask);
    std::memset(b, 0, 16);
    b[0] = 16;
    b[1] = 2;  // AF_INET
    b[2] = static_cast<uint8_t>(a.port >> 8);
    b[3] = static_cast<uint8_t>(a.port);
    b[4] = static_cast<uint8_t>(a.ip >> 24);
    b[5] = static_cast<uint8_t>(a.ip >> 16);
    b[6] = static_cast<uint8_t>(a.ip >> 8);
    b[7] = static_cast<uint8_t>(a.ip);
    b[8] = static_cast<uint8_t>(a.vport >> 8);
    b[9] = static_cast<uint8_t>(a.vport);
}

bool virtual_ip(uint32_t ip) { return (ip & kVipMask) == kVipNet; }

void report_bound(uint16_t port) {
    infra::client().request({{"op", "bound"}, {"port", port}}, [](const nlohmann::json&) {}, 5000);
}

// Feeds the stream segments that arrived into g_streams (g_mu held).
void pump_streams() {
    infra::Client::Datagram d;
    while (infra::client().pop_stream(&d)) g_streams.on_segment(p2p::Segment{d.vip, d.dst_port, d.src_port, d.payload});
}

// Sends what g_streams queued; call without g_mu.
void flush_streams() {
    std::vector<Out> out;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        out.swap(g_outbox);
    }
    if (out.empty()) return;
    for (const Out& o : out) infra::client().send_stream(o.vip, o.dst, o.src, o.bytes.data(), o.bytes.size());
}

void hle_sceNetInetInit(uint8_t*, recomp_context* ctx) { ctx->r[2] = SCE_OK; }
void hle_sceNetInetTerm(uint8_t*, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int i = 0; i < kMaxSockets; i++) {
        if (g_socks[i].used && g_socks[i].type == kSockStreamP2P) g_streams.close(i);
        g_socks[i] = Socket{};
    }
    g_outbox.clear();
    ctx->r[2] = SCE_OK;
}

// Socket(domain, type, protocol)
void hle_sceNetInetSocket(uint8_t*, recomp_context* ctx) {
    const int domain = ctx->r[4], type = ctx->r[5] & 0x0F, proto = ctx->r[6];
    if (domain != 2 || (type != kSockDgram && type != kSockDgramP2P && type != kSockStreamP2P)) {
        std::fprintf(stderr, "[INET] Socket(%d, %d, %d): not supported\n", domain, ctx->r[5], proto);
        fail(ctx, inet_errno::kEPROTONOSUPPORT);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    const int s = new_socket(type, (ctx->r[5] & 0x20000000) != 0);
    if (s < 0) {
        fail(ctx, inet_errno::kEINVAL);
        return;
    }
    std::fprintf(stderr, "[INET] Socket(%d, %d, %d) -> %d\n", domain, type, proto, s);
    ctx->r[2] = s;
}

// Setsockopt(s, level, name, value*, len)
void hle_sceNetInetSetsockopt(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    const uint32_t level = static_cast<uint32_t>(ctx->r[5]), name = static_cast<uint32_t>(ctx->r[6]),
                   val = static_cast<uint32_t>(ctx->r[7]), len = static_cast<uint32_t>(ctx->r[8]);
    const uint32_t v = len >= 4 && valid_range(val, 4) ? psp_mem_read<uint32_t>(rdram, val) : 0;
    std::lock_guard<std::mutex> lock(g_mu);
    Socket* so = sock(s);
    if (!so) {
        fail(ctx, inet_errno::kEBADF);
        return;
    }
    if (level == 0xFFFF && name == kSoNbio) so->nonblock = v != 0;
    if (trace()) std::fprintf(stderr, "[INET] Setsockopt(%d, 0x%X, 0x%X, %u) accepted\n", s, level, name, v);
    ctx->r[2] = SCE_OK;
}

// Bind(s, sockaddr*, len)
void hle_sceNetInetBind(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    Sockaddr a;
    read_sockaddr(rdram, static_cast<uint32_t>(ctx->r[5]), &a);
    bool udp;
    uint16_t port;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Socket* so = sock(s);
        if (!so) {
            fail(ctx, inet_errno::kEBADF);
            return;
        }
        Socket want = *so;
        want.port = a.port ? a.port : g_next_port++;
        want.vport = a.vport;
        for (const Socket& o : g_socks)
            if (&o != so && o.used && (o.type == kSockStreamP2P) == (want.type == kSockStreamP2P) &&
                (want.type == kSockStreamP2P ? o.port == want.port : o.key() == want.key())) {
                fail(ctx, inet_errno::kEADDRINUSE);
                return;
            }
        *so = want;
        udp = so->type != kSockStreamP2P;
        port = so->port;
    }
    std::fprintf(stderr, "[INET] Bind(%d, port %u vport %u)\n", s, a.port, a.vport);
    if (udp) report_bound(port);  // the port peers learn through signaling
    ctx->r[2] = SCE_OK;
}

// ---- UDP ----

// Sendto(s, buf*, len, flags, to*, tolen)
void hle_sceNetInetSendto(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    const uint32_t buf = static_cast<uint32_t>(ctx->r[5]), len = static_cast<uint32_t>(ctx->r[6]);
    Sockaddr to;
    if (!read_sockaddr(rdram, static_cast<uint32_t>(ctx->r[8]), &to) || !valid_range(buf, len) || len > 4096) {
        fail(ctx, inet_errno::kEINVAL);
        return;
    }
    uint16_t src, dst;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Socket* so = sock(s);
        if (!so || so->type == kSockStreamP2P) {
            fail(ctx, inet_errno::kEBADF);
            return;
        }
        if (!so->port) {
            so->port = g_next_port++;
            fresh = true;
        }
        src = so->port;
        dst = so->type == kSockDgramP2P && to.vport ? to.vport : to.port;
    }
    if (fresh) report_bound(src);
    if (!virtual_ip(to.ip) || !infra::client().connected()) {
        static int logged = 0;
        if (logged++ < 8)
            std::fprintf(stderr, "[INET] Sendto(%d) %u.%u.%u.%u:%u: not on the virtual network\n", s, to.ip >> 24,
                         (to.ip >> 16) & 255, (to.ip >> 8) & 255, to.ip & 255, to.port);
        fail(ctx, inet_errno::kENETUNREACH);
        return;
    }
    infra::client().send_datagram(to.ip, dst, src, rdram + (buf & kAddrMask), len);  // queues, never waits
    if (trace()) std::fprintf(stderr, "[INET] Sendto(%d) %u bytes to %08X:%u/%u\n", s, len, to.ip, to.port, to.vport);
    ctx->r[2] = static_cast<int32_t>(len);
}

// Recvfrom(s, buf*, len, flags, from*, fromlen*)
void hle_sceNetInetRecvfrom(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    const uint32_t buf = static_cast<uint32_t>(ctx->r[5]), len = static_cast<uint32_t>(ctx->r[6]),
                   from = static_cast<uint32_t>(ctx->r[8]), fromlen = static_cast<uint32_t>(ctx->r[9]);
    uint16_t key, vport;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Socket* so = sock(s);
        if (!so || so->type == kSockStreamP2P) {
            fail(ctx, inet_errno::kEBADF);
            return;
        }
        key = so->key();
        vport = so->vport;
    }
    infra::Client::Datagram d;
    if (!key || !infra::client().pop_datagram(key, &d)) {
        fail(ctx, inet_errno::kEAGAIN);
        return;
    }
    const uint32_t n = static_cast<uint32_t>(d.payload.size()) < len ? static_cast<uint32_t>(d.payload.size()) : len;
    if (n && valid_range(buf, n)) std::memcpy(rdram + (buf & kAddrMask), d.payload.data(), n);
    write_sockaddr(rdram, from, Sockaddr{d.vip, d.src_port, vport});
    if (valid_range(fromlen, 4)) psp_mem_write<uint32_t>(rdram, fromlen, 16);
    if (trace()) std::fprintf(stderr, "[INET] Recvfrom(%d) %u bytes from %08X:%u\n", s, n, d.vip, d.src_port);
    ctx->r[2] = static_cast<int32_t>(n);
}

// ---- TCP P2P ----

// Listen(s, backlog)
void hle_sceNetInetListen(uint8_t*, recomp_context* ctx) {
    const int s = ctx->r[4];
    int r;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Socket* so = sock(s);
        if (!so || so->type != kSockStreamP2P || !so->port) {
            fail(ctx, inet_errno::kEINVAL);
            return;
        }
        port = so->port;
        r = g_streams.listen(s, port);
    }
    std::fprintf(stderr, "[INET] Listen(%d) on port %u -> %d\n", s, port, r);
    stream_result(ctx, r);
}

// Accept(s, from*, fromlen*) -> new socket
void hle_sceNetInetAccept(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    const uint32_t from = static_cast<uint32_t>(ctx->r[5]), fromlen = static_cast<uint32_t>(ctx->r[6]);
    int r;
    Sockaddr peer;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        pump_streams();
        Socket* so = sock(s);
        if (!so || so->type != kSockStreamP2P) {
            fail(ctx, inet_errno::kEBADF);
            return;
        }
        const Socket listener = *so;
        const int n = new_socket(kSockStreamP2P, listener.nonblock);
        if (n < 0) {
            fail(ctx, inet_errno::kEINVAL);
            return;
        }
        r = g_streams.accept(s, n, &peer.ip, &peer.port);
        if (r < 0) {
            g_socks[n] = Socket{};
        } else {
            g_socks[n].port = listener.port;
            g_socks[n].vport = listener.vport;
            peer.vport = listener.vport;
            r = n;
        }
    }
    flush_streams();
    if (r < 0) {
        fail(ctx, -r);
        return;
    }
    write_sockaddr(rdram, from, peer);
    if (valid_range(fromlen, 4)) psp_mem_write<uint32_t>(rdram, fromlen, 16);
    std::fprintf(stderr, "[INET] Accept(%d) -> %d from %u.%u.%u.%u:%u\n", s, r, peer.ip >> 24, (peer.ip >> 16) & 255,
                 (peer.ip >> 8) & 255, peer.ip & 255, peer.port);
    ctx->r[2] = r;
}

// Connect(s, to*, len)
void hle_sceNetInetConnect(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    Sockaddr to;
    if (!read_sockaddr(rdram, static_cast<uint32_t>(ctx->r[5]), &to)) {
        fail(ctx, inet_errno::kEINVAL);
        return;
    }
    if (!virtual_ip(to.ip)) {
        std::fprintf(stderr, "[INET] Connect(%d) %08X:%u: not on the virtual network\n", s, to.ip, to.port);
        fail(ctx, inet_errno::kENETUNREACH);
        return;
    }
    int r;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        pump_streams();
        Socket* so = sock(s);
        if (!so || so->type != kSockStreamP2P) {
            fail(ctx, inet_errno::kEBADF);
            return;
        }
        r = g_streams.connect(s, to.ip, to.port);
    }
    flush_streams();
    if (r != -inet_errno::kEALREADY || trace())
        std::fprintf(stderr, "[INET] Connect(%d) to %u.%u.%u.%u:%u (vport %u) -> %d\n", s, to.ip >> 24,
                     (to.ip >> 16) & 255, (to.ip >> 8) & 255, to.ip & 255, to.port, to.vport, r);
    stream_result(ctx, r);
}

// Send(s, buf*, len, flags)
void hle_sceNetInetSend(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    const uint32_t buf = static_cast<uint32_t>(ctx->r[5]), len = static_cast<uint32_t>(ctx->r[6]);
    if (!valid_range(buf, len) || len > 0x10000) {
        fail(ctx, inet_errno::kEINVAL);
        return;
    }
    int r;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        pump_streams();
        r = g_streams.send(s, rdram + (buf & kAddrMask), len);
    }
    flush_streams();
    if (trace()) std::fprintf(stderr, "[INET] Send(%d, %u) -> %d\n", s, len, r);
    stream_result(ctx, r);
}

// Recv(s, buf*, len, flags)
void hle_sceNetInetRecv(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    const uint32_t buf = static_cast<uint32_t>(ctx->r[5]), len = static_cast<uint32_t>(ctx->r[6]);
    if (!valid_range(buf, len)) {
        fail(ctx, inet_errno::kEINVAL);
        return;
    }
    int r;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        pump_streams();
        r = g_streams.recv(s, rdram + (buf & kAddrMask), len);
    }
    flush_streams();
    if (trace() && r != -inet_errno::kEAGAIN) std::fprintf(stderr, "[INET] Recv(%d, %u) -> %d\n", s, len, r);
    stream_result(ctx, r);
}

// Trace aid: the return address and the code addresses found on the guest stack (callers).
void trace_callers(uint8_t* rdram, const recomp_context* ctx, const char* what) {
    if (!trace()) return;
    const uint32_t sp = static_cast<uint32_t>(ctx->r[29]);
    std::fprintf(stderr, "[INET] %s ra=%08X stack:", what, static_cast<uint32_t>(ctx->r[31]));
    int found = 0;
    for (uint32_t o = 0; o < 0x400 && found < 16 && valid_range(sp + o, 4); o += 4) {
        const uint32_t v = psp_mem_read<uint32_t>(rdram, sp + o);
        if (v >= 0x08804000 && v < 0x08A52000 && (v & 3) == 0) {
            std::fprintf(stderr, " %08X", v);
            found++;
        }
    }
    std::fprintf(stderr, "\n");
}

void hle_sceNetInetClose(uint8_t* rdram, recomp_context* ctx) {
    const int s = ctx->r[4];
    trace_callers(rdram, ctx, "Close");
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Socket* so = sock(s);
        if (!so) {
            fail(ctx, inet_errno::kEBADF);
            return;
        }
        if (so->type == kSockStreamP2P) g_streams.close(s);
        *so = Socket{};
    }
    flush_streams();
    std::fprintf(stderr, "[INET] Close(%d)\n", s);
    ctx->r[2] = SCE_OK;
}

void hle_sceNetInetSocketAbort(uint8_t*, recomp_context* ctx) { ctx->r[2] = SCE_OK; }

void hle_sceNetInetGetPspError(uint8_t*, recomp_context* ctx) {
    ctx->r[2] = static_cast<int32_t>(inet_errno_to_psp(t_errno));
}

void hle_sceNetInetGetErrno(uint8_t*, recomp_context* ctx) { ctx->r[2] = t_errno; }

}  // namespace

bool psp_inet_streams_idle() {
    bool idle;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        pump_streams();
        idle = g_streams.idle();
        for (const Socket& so : g_socks)
            if (idle && so.used && so.type != kSockStreamP2P && so.key() && infra::client().has_datagram(so.key()))
                idle = false;
    }
    flush_streams();
    return idle;
}

void psp_hle_register_inet() {
    psp_hle_register("sceNetInetInit", hle_sceNetInetInit);
    psp_hle_register("sceNetInetTerm", hle_sceNetInetTerm);
    psp_hle_register("sceNetInetSocket", hle_sceNetInetSocket);
    psp_hle_register("sceNetInetSetsockopt", hle_sceNetInetSetsockopt);
    psp_hle_register("sceNetInetBind", hle_sceNetInetBind);
    psp_hle_register("sceNetInetSendto", hle_sceNetInetSendto);
    psp_hle_register("sceNetInetRecvfrom", hle_sceNetInetRecvfrom);
    psp_hle_register("sceNetInetListen", hle_sceNetInetListen);
    psp_hle_register("sceNetInetAccept", hle_sceNetInetAccept);
    psp_hle_register("sceNetInetConnect", hle_sceNetInetConnect);
    psp_hle_register("sceNetInetSend", hle_sceNetInetSend);
    psp_hle_register("sceNetInetRecv", hle_sceNetInetRecv);
    psp_hle_register("sceNetInetClose", hle_sceNetInetClose);
    psp_hle_register("sceNetInetSocketAbort", hle_sceNetInetSocketAbort);
    psp_hle_register("sceNetInetGetPspError", hle_sceNetInetGetPspError);
    psp_hle_register("sceNetInetGetErrno", hle_sceNetInetGetErrno);
}
