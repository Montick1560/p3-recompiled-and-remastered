// sceNetAdhocMatching imports over PDP (runtime/src/net/matching.cpp).
// Argument checks and return codes follow PPSSPP
// Core/HLE/sceNetAdhocMatching.cpp (GPL-2.0-or-later). Each started
// context gets one host thread that drains its PDP socket, runs the
// matching timers and posts the game's handler calls:
// handler(id, event, mac*, optlen, opt*) with the MAC at buf+0 and the
// optional data at buf+8, like PPSSPP's notifyMatchingHandler.
#include "hle/psp_hle.h"
#include "hle/psp_hle_net.h"
#include "psp_memory.h"
#include "recomp.h"
#include "net/adhocctl.h"
#include "net/guest_callbacks.h"
#include "net/matching.h"
#include "net/pdp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kAddrMask = 0x07FFFFFFU;
constexpr int32_t kErrInvalidMaxnum = (int32_t)0x80410803;
constexpr int32_t kErrRxbufTooShort = (int32_t)0x80410804;
constexpr int32_t kErrInvalidOptlen = (int32_t)0x80410805;
constexpr int32_t kErrInvalidArg = (int32_t)0x80410806;
constexpr int32_t kErrInvalidId = (int32_t)0x80410807;
constexpr int32_t kErrNotRunning = (int32_t)0x8041080B;
constexpr int32_t kErrAlreadyInitialized = (int32_t)0x80410812;
constexpr int32_t kErrNotInitialized = (int32_t)0x80410813;
constexpr int32_t kErrPortInUse = (int32_t)0x80410814;

struct Slot {
    std::mutex mu;
    std::unique_ptr<matching::Context> ctx;
    uint32_t handler = 0;
    int rxbuflen = 0;
    int pdp_id = 0;
    std::atomic<bool> pumping{false};
    std::thread pump;
};

std::mutex g_mu;  // guards g_slots and g_inited
std::map<int, std::shared_ptr<Slot>> g_slots;
bool g_inited = false;
uint8_t* g_rdram = nullptr;

uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool valid_range(uint32_t addr, uint32_t len) {
    if (addr < 0x00010000U) return false;
    return static_cast<uint64_t>(addr & kAddrMask) + len <= PSP_MEM_SIZE;
}

std::shared_ptr<Slot> find(int id) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_slots.find(id);
    return it == g_slots.end() ? nullptr : it->second;
}

std::vector<uint8_t> read_opt(uint8_t* rdram, uint32_t addr, int len) {
    if (len <= 0 || !valid_range(addr, static_cast<uint32_t>(len))) return {};
    const uint8_t* p = rdram + (addr & kAddrMask);
    return std::vector<uint8_t>(p, p + len);
}

const char* event_name(int e) {
    static const char* names[] = {"?", "HELLO", "REQUEST", "LEAVE", "DENY", "CANCEL", "ACCEPT",
                                  "ESTABLISHED", "TIMEOUT", "ERROR", "BYE", "DATA"};
    return e >= 0 && e <= 11 ? names[e] : "?";
}

void pump(int id, std::shared_ptr<Slot> slot) {
    std::vector<uint8_t> buf(static_cast<size_t>(slot->rxbuflen > 0 ? slot->rxbuflen : 4096));
    while (slot->pumping) {
        for (;;) {
            pro::Mac from{};
            uint16_t port = 0;
            int len = static_cast<int>(buf.size());
            const int rc = pdp::instance().recv(slot->pdp_id, &from, &port, buf.data(), &len, 0, true);
            if (rc == pdp::kErrNotEnoughSpace) {
                buf.resize(static_cast<size_t>(len));
                continue;
            }
            if (rc != 0) break;
            std::lock_guard<std::mutex> lock(slot->mu);
            slot->ctx->on_packet(from, port, buf.data(), static_cast<size_t>(len), now_us());
        }
        {
            std::lock_guard<std::mutex> lock(slot->mu);
            slot->ctx->tick(now_us());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)id;
}

void stop_slot(Slot& slot) {
    if (slot.pumping.exchange(false) && slot.pump.joinable()) slot.pump.join();
    std::lock_guard<std::mutex> lock(slot.mu);
    if (slot.ctx && slot.ctx->running()) slot.ctx->stop();
    if (slot.pdp_id > 0) {
        pdp::instance().remove(slot.pdp_id);
        slot.pdp_id = 0;
    }
}

}  // namespace

static void hle_sceNetAdhocMatchingInit(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_rdram = rdram;
    if (g_inited) {
        ctx->r[2] = kErrAlreadyInitialized;
        return;
    }
    g_inited = true;
    ctx->r[2] = 0;
}

static void hle_sceNetAdhocMatchingTerm(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    std::map<int, std::shared_ptr<Slot>> slots;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        slots.swap(g_slots);
        g_inited = false;
    }
    for (auto& kv : slots) stop_slot(*kv.second);
    ctx->r[2] = 0;
}

// Create(mode, maxnum, port, rxbuflen, hello_int, keepalive_int, init_count, rexmt_int, callback)
static void hle_sceNetAdhocMatchingCreate(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const int mode = ctx->r[4], maxnum = ctx->r[5];
    const auto port = static_cast<uint16_t>(ctx->r[6]);
    const int rxbuflen = ctx->r[7];
    const auto hello = static_cast<uint32_t>(ctx->r[8]);
    const int32_t keepalive = ctx->r[9];
    const int init_count = ctx->r[10];
    const auto rexmt = static_cast<uint32_t>(ctx->r[11]);
    const auto callback = static_cast<uint32_t>(ctx->r[12]);  // 9th argument (PPSSPP PARAM(8))
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_inited) { ctx->r[2] = kErrNotInitialized; return; }
    if (maxnum <= 1 || maxnum > 16) { ctx->r[2] = kErrInvalidMaxnum; return; }
    if (rxbuflen < 1) { ctx->r[2] = kErrRxbufTooShort; return; }
    if (mode < 1 || mode > 3) { ctx->r[2] = kErrInvalidArg; return; }
    for (const auto& kv : g_slots) {
        if (kv.second->ctx->port() == port) { ctx->r[2] = kErrPortInUse; return; }
    }
    int id = 1;
    while (g_slots.count(id)) id++;
    auto slot = std::make_shared<Slot>();
    slot->handler = callback;
    slot->rxbuflen = rxbuflen;
    matching::Io io;
    std::weak_ptr<Slot> weak = slot;
    io.send = [weak](const pro::Mac& to, uint16_t to_port, const std::vector<uint8_t>& bytes) {
        if (auto s = weak.lock()) pdp::instance().send(s->pdp_id, to, to_port, bytes.data(), static_cast<int>(bytes.size()));
    };
    io.group_peers = [] {
        std::vector<pro::Mac> out;
        for (const auto& p : adhocctl::instance().peers()) out.push_back(p.mac);
        return out;
    };
    io.notify = [weak, id](int event, const pro::Mac& peer, const std::vector<uint8_t>& opt) {
        auto s = weak.lock();
        if (!s || s->handler == 0) return;
        GuestCall c;
        c.fn = s->handler;
        c.a[0] = static_cast<uint32_t>(id);
        c.a[1] = static_cast<uint32_t>(event);
        c.a[2] = 0;
        c.a[3] = static_cast<uint32_t>(opt.size());
        c.a[4] = 8;
        c.buf.assign(8, 0);
        std::memcpy(c.buf.data(), peer.data(), 6);
        c.buf.insert(c.buf.end(), opt.begin(), opt.end());
        c.buf_args = (1 << 2) | (1 << 4);
        std::fprintf(stderr, "[ADHOC] matching %d event %s from %02x:%02x:%02x:%02x:%02x:%02x optlen=%zu\n", id,
                     event_name(event), peer[0], peer[1], peer[2], peer[3], peer[4], peer[5], opt.size());
        guest_callbacks_post(g_rdram, std::move(c));
    };
    slot->ctx = std::make_unique<matching::Context>(static_cast<matching::Mode>(mode), maxnum, port, hello,
                                                     keepalive, init_count, rexmt, psp_net_config().mac, io);
    g_slots[id] = slot;
    std::fprintf(stderr, "[ADHOC] sceNetAdhocMatchingCreate(mode=%d max=%d port=%u hello=%u keepalive=%d) -> %d\n",
                 mode, maxnum, port, hello, keepalive, id);
    ctx->r[2] = id;
}

// Start(id, evthPri, evthStack, inthPri, inthStack, optLen, optData)
static void hle_sceNetAdhocMatchingStart(uint8_t* rdram, recomp_context* ctx) {
    const int id = ctx->r[4];
    const int opt_len = ctx->r[9];
    const auto opt_addr = static_cast<uint32_t>(ctx->r[10]);
    auto slot = find(id);
    if (!slot) { ctx->r[2] = 0; return; }  // PPSSPP: unknown id is not an error here
    {
        std::lock_guard<std::mutex> lock(slot->mu);
        if (slot->ctx->running()) { ctx->r[2] = 0; return; }
        psp_net_wire_pdp();
        const int sock = pdp::instance().create(slot->ctx->port(), slot->rxbuflen);
        if (sock < 1) { ctx->r[2] = kErrPortInUse; return; }
        slot->pdp_id = sock;
        slot->ctx->start(read_opt(rdram, opt_addr, opt_len));
    }
    slot->pumping = true;
    slot->pump = std::thread(pump, id, slot);
    std::fprintf(stderr, "[ADHOC] sceNetAdhocMatchingStart(%d, hello opt %d bytes)\n", id, opt_len);
    ctx->r[2] = 0;
}

static void hle_sceNetAdhocMatchingStop(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (auto slot = find(ctx->r[4])) stop_slot(*slot);
    ctx->r[2] = 0;
}

static void hle_sceNetAdhocMatchingDelete(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_slots.find(ctx->r[4]);
        if (it != g_slots.end()) {
            slot = it->second;
            g_slots.erase(it);
        }
    }
    if (slot) stop_slot(*slot);
    ctx->r[2] = 0;
}

static int target_call(uint8_t* rdram, recomp_context* ctx, bool select, bool with_opt) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_inited) return kErrNotInitialized;
    }
    const auto mac_addr = static_cast<uint32_t>(ctx->r[5]);
    const int opt_len = with_opt ? ctx->r[6] : 0;
    const auto opt_addr = with_opt ? static_cast<uint32_t>(ctx->r[7]) : 0U;
    if (!valid_range(mac_addr, 6)) return kErrInvalidArg;
    auto slot = find(ctx->r[4]);
    if (!slot) return kErrInvalidId;
    if (opt_len != 0 && (opt_len < 0 || !valid_range(opt_addr, static_cast<uint32_t>(opt_len)))) {
        return select ? kErrInvalidOptlen : kErrInvalidArg;
    }
    pro::Mac mac;
    std::memcpy(mac.data(), rdram + (mac_addr & kAddrMask), 6);
    const auto opt = read_opt(rdram, opt_addr, opt_len);
    std::lock_guard<std::mutex> lock(slot->mu);
    if (!slot->ctx->running()) return kErrNotRunning;
    return select ? slot->ctx->select_target(mac, opt, now_us()) : slot->ctx->cancel_target(mac, opt, now_us());
}

static void hle_sceNetAdhocMatchingSelectTarget(uint8_t* rdram, recomp_context* ctx) {
    ctx->r[2] = target_call(rdram, ctx, true, true);
    std::fprintf(stderr, "[ADHOC] sceNetAdhocMatchingSelectTarget(%d) -> 0x%08X\n", ctx->r[4], (uint32_t)ctx->r[2]);
}

static void hle_sceNetAdhocMatchingCancelTarget(uint8_t* rdram, recomp_context* ctx) {
    ctx->r[2] = target_call(rdram, ctx, false, false);
}

static void hle_sceNetAdhocMatchingCancelTargetWithOpt(uint8_t* rdram, recomp_context* ctx) {
    ctx->r[2] = target_call(rdram, ctx, false, true);
}

// SetHelloOpt(id, optLen, optData)
static void hle_sceNetAdhocMatchingSetHelloOpt(uint8_t* rdram, recomp_context* ctx) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_inited) { ctx->r[2] = kErrNotInitialized; return; }
    }
    auto slot = find(ctx->r[4]);
    if (!slot) { ctx->r[2] = kErrInvalidId; return; }
    const int opt_len = ctx->r[5];
    const auto opt_addr = static_cast<uint32_t>(ctx->r[6]);
    if (opt_len != 0 && opt_addr == 0) { ctx->r[2] = kErrInvalidOptlen; return; }
    std::lock_guard<std::mutex> lock(slot->mu);
    ctx->r[2] = slot->ctx->set_hello_opt(read_opt(rdram, opt_addr, opt_len));
}

// GetMembers(id, size*, buf*): entries {u32 next; u8 mac[6]; u8 pad[2]} (12 bytes).
static void hle_sceNetAdhocMatchingGetMembers(uint8_t* rdram, recomp_context* ctx) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_inited) { ctx->r[2] = kErrNotInitialized; return; }
    }
    const auto size_addr = static_cast<uint32_t>(ctx->r[5]);
    const auto buf = static_cast<uint32_t>(ctx->r[6]);
    if (!valid_range(size_addr, 4)) { ctx->r[2] = kErrInvalidArg; return; }
    auto slot = find(ctx->r[4]);
    if (!slot) { ctx->r[2] = kErrInvalidId; return; }
    std::vector<pro::Mac> members;
    {
        std::lock_guard<std::mutex> lock(slot->mu);
        if (!slot->ctx->running()) { ctx->r[2] = kErrNotRunning; return; }
        members = slot->ctx->members();
    }
    constexpr int kEntry = 12;
    const int available = kEntry * static_cast<int>(members.size());
    if (buf == 0) {
        psp_mem_write<int32_t>(rdram, size_addr, available);
        ctx->r[2] = 0;
        return;
    }
    int len = psp_mem_read<int32_t>(rdram, size_addr);
    if (len < 0) len = 0;
    if (len > available) len = available;
    const int n = len / kEntry;
    if (n > 0 && !valid_range(buf, static_cast<uint32_t>(n * kEntry))) { ctx->r[2] = kErrInvalidArg; return; }
    for (int i = 0; i < n; i++) {
        uint8_t* e = rdram + ((buf + i * kEntry) & kAddrMask);
        std::memset(e, 0, kEntry);
        const uint32_t next = i + 1 < n ? buf + kEntry * (i + 1) : 0;
        psp_mem_write<uint32_t>(rdram, buf + i * kEntry, next);
        std::memcpy(e + 4, members[static_cast<size_t>(i)].data(), 6);
    }
    psp_mem_write<int32_t>(rdram, size_addr, n * kEntry);
    ctx->r[2] = 0;
}

void psp_hle_register_net_matching() {
    psp_hle_register("sceNetAdhocMatchingInit", hle_sceNetAdhocMatchingInit);
    psp_hle_register("sceNetAdhocMatchingTerm", hle_sceNetAdhocMatchingTerm);
    psp_hle_register("sceNetAdhocMatchingCreate", hle_sceNetAdhocMatchingCreate);
    psp_hle_register("sceNetAdhocMatchingStart", hle_sceNetAdhocMatchingStart);
    psp_hle_register("sceNetAdhocMatchingStop", hle_sceNetAdhocMatchingStop);
    psp_hle_register("sceNetAdhocMatchingDelete", hle_sceNetAdhocMatchingDelete);
    psp_hle_register("sceNetAdhocMatchingSelectTarget", hle_sceNetAdhocMatchingSelectTarget);
    psp_hle_register("sceNetAdhocMatchingCancelTarget", hle_sceNetAdhocMatchingCancelTarget);
    psp_hle_register("sceNetAdhocMatchingCancelTargetWithOpt", hle_sceNetAdhocMatchingCancelTargetWithOpt);
    psp_hle_register("sceNetAdhocMatchingSetHelloOpt", hle_sceNetAdhocMatchingSetHelloOpt);
    psp_hle_register("sceNetAdhocMatchingGetMembers", hle_sceNetAdhocMatchingGetMembers);
}
