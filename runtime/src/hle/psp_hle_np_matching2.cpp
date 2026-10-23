// sceNpMatching2 imports (infrastructure-mode rooms) over the room server
// tools/infra_server.py (net/infra_client.h). The PSP library takes the PS3
// SceNpMatching2* request structs (little endian, 32-bit pointers) and hands
// the response data straight to the request callback
// cb(ctxId, reqId, event, errorCode, data*, cbArg); rooms report to the room
// event callback cb(ctxId, 0, roomId lo, roomId hi, event, data*, arg) and
// peers to the signaling callback cb(ctxId, 0, roomId lo, roomId hi,
// memberId, event, errorCode, arg). Event numbers and the PSP differences are
// in docs/superpowers/specs/2026-10-22-p3-infrastructure-mode-design.md.
// On VS the game is in two rooms at once (its hideout room and a VS room):
// the state is kept per room (net/np2_rooms.h) and every event or call uses
// the room it names.
//
// The guest CPU never waits on the network: a request is parsed and given
// its id on the guest thread, sent from a worker thread, and its answer is
// written into a response arena and posted through guest_callbacks.
#include "hle/psp_hle.h"
#include "hle/psp_hle_np.h"
#include "hle/psp_hle_inet.h"
#include "hle/psp_hle_net.h"
#include "psp_memory.h"
#include "recomp.h"
#include "net/guest_callbacks.h"
#include "net/infra_client.h"
#include "net/infra_protocol.h"
#include "net/np2_rooms.h"
#include "net/np2_structs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kAddrMask = 0x07FFFFFFU;
constexpr uint32_t kNullPage = 0x00010000U;

// Request events (PSP numbering, checked against the game's handlers).
constexpr uint16_t kEvServerInfo = 0x0001, kEvWorldInfoList = 0x0002, kEvRoomDataExternalList = 0x0007;
// Server answer to a join from a console id already in the room (FORBIDDEN).
constexpr uint32_t kErrDuplicateConsole = 0x80550D07;
constexpr uint16_t kEvCreateJoinRoom = 0x0101, kEvJoinRoom = 0x0102, kEvLeaveRoom = 0x0103;
constexpr uint16_t kEvKickout = 0x0105, kEvSearchRoom = 0x0106, kEvSetRoomDataInternal = 0x0109;
// Room events.
constexpr uint16_t kEvMemberJoined = 0x1101, kEvMemberLeft = 0x1102, kEvKickedOut = 0x1103,
                   kEvRoomDestroyed = 0x1104;
constexpr int kSignalingActive = 2;  // SCE_NP_SIGNALING_CONN_STATUS_ACTIVE

bool valid_range(uint32_t addr, uint32_t len) {
    if (addr < kNullPage) return false;
    return static_cast<uint64_t>(addr & kAddrMask) + len <= PSP_MEM_SIZE;
}

bool trace() {
    static const bool on = std::getenv("PSPRECOMP_INFRA_TRACE") != nullptr;
    return on;
}

void log_call(const char* name, const recomp_context* ctx) {
    if (!trace()) return;
    std::fprintf(stderr, "[NP2] %s(a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X)\n", name,
                 (uint32_t)ctx->r[4], (uint32_t)ctx->r[5], (uint32_t)ctx->r[6], (uint32_t)ctx->r[7],
                 (uint32_t)ctx->r[8], (uint32_t)ctx->r[9]);
}

uint8_t* g_rdram = nullptr;

// ---- response arena: guest memory the callback data lives in ----
// Slots are used round-robin; the game copies what it needs while the
// callback runs, and a slot is reused only kSlots responses later.
constexpr uint32_t kSlotSize = 0x8000;
constexpr int kSlots = 8;

np2::GuestBuf arena_slot() {
    static uint32_t base = 0;
    static int next = 0;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (base == 0) base = psp_alloc_kernel_memory(kSlotSize * kSlots);
    if (base == 0) return np2::GuestBuf{nullptr, 0, 0, 0};
    const uint32_t a = base + kSlotSize * static_cast<uint32_t>(next);
    next = (next + 1) % kSlots;
    return np2::GuestBuf{g_rdram + (a & kAddrMask), a, kSlotSize, 0};
}

// Runs a struct writer in a fresh slot; 0 when the data does not fit or the
// server sent JSON the writer cannot read.
uint32_t build(const std::function<uint32_t(np2::GuestBuf&)>& write) {
    np2::GuestBuf b = arena_slot();
    if (!b.base_host) return 0;
    try {
        return write(b);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[NP2] malformed server data: %s\n", e.what());
        return 0;
    }
}

// ---- worker thread: connects and sends, off the guest CPU ----
class Worker {
public:
    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!started_) {
                started_ = true;
                std::thread([this] { run(); }).detach();
            }
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return !q_.empty(); });
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool started_ = false;
};

Worker& worker() {
    static Worker* w = new Worker();  // leaked: used until exit
    return *w;
}

struct State {
    std::mutex mu;
    uint32_t next_req = 0;
    bool started = false;  // between ContextStart and ContextStop/Term
    uint16_t ctx_id = 1;
    uint32_t sig_cb = 0, sig_arg = 0;
    np2::Rooms rooms;  // every room the game is in
};

State& st() {
    static State* s = new State();
    return *s;
}

std::atomic<bool> g_connecting{false};

void on_event(const nlohmann::json& e);

bool ensure_connected() {
    infra::Client& c = infra::client();
    if (c.connected()) return true;
    static std::once_flag hooked;
    std::call_once(hooked, [&c] { c.on_event(on_event); });
    // The console id (the MAC that starts the OpenPSID) lets the server refuse
    // a second member that the game would also take for player 1.
    const AdhocMac& mac = psp_net_config().mac;
    // PSPRECOMP_INFRA_INVITE: the beta invite code the launcher passes (none outside a beta).
    return c.connect(psp_np_online_id(), 10000,
                     infra::login_fields(infra::to_hex(mac.data(), mac.size()), std::getenv("PSPRECOMP_INFRA_INVITE")));
}

// ---- requests ----
struct Reply {
    uint32_t cb = 0, arg = 0, req = 0;
    uint16_t ctx = 1, event = 0;
    int timeout_ms = 15000;
};

// Reads optParam {cbFunc, cbArg, timeout_us, u16 appReqId} (a2), gives the
// request its id and writes it to *assignedReqId (register reg).
Reply begin_request(uint8_t* rdram, recomp_context* ctx, uint16_t event, int reg) {
    g_rdram = rdram;
    Reply r;
    r.event = event;
    r.ctx = static_cast<uint16_t>(ctx->r[4]);
    const uint32_t opt = static_cast<uint32_t>(ctx->r[6]);
    if (valid_range(opt, 12)) {
        r.cb = psp_mem_read<uint32_t>(rdram, opt);
        r.arg = psp_mem_read<uint32_t>(rdram, opt + 4);
        const uint32_t us = psp_mem_read<uint32_t>(rdram, opt + 8);
        if (us >= 1000000 && us <= 60000000) r.timeout_ms = static_cast<int>(us / 1000);
    }
    {
        std::lock_guard<std::mutex> lock(st().mu);
        r.req = ++st().next_req;
    }
    const uint32_t assigned = static_cast<uint32_t>(ctx->r[reg]);
    if (valid_range(assigned, 4)) psp_mem_write<uint32_t>(rdram, assigned, r.req);
    if (!r.cb) std::fprintf(stderr, "[NP2] request 0x%04X without a callback\n", event);
    return r;
}

void post_reply(const Reply& r, uint32_t error, uint32_t data) {
    if (error)
        std::fprintf(stderr, "[NP2] 0x%04X req %u error 0x%08X\n", r.event, r.req, error);
    else
        std::fprintf(stderr, "[NP2] 0x%04X req %u ok\n", r.event, r.req);
    if (!r.cb) return;
    GuestCall c;
    c.fn = r.cb;
    c.a[0] = r.ctx;
    c.a[1] = r.req;
    c.a[2] = r.event;
    c.a[3] = error;
    c.a[4] = error ? 0 : data;
    c.a[5] = r.arg;
    guest_callbacks_post(g_rdram, std::move(c));
}

using Make = std::function<uint32_t(np2::GuestBuf&, const nlohmann::json& answer)>;

// Sends req to the server from the worker; the answer is written by make
// (none: null data) and handed to the game's callback.
void send_request(const Reply& r, nlohmann::json req, Make make) {
    worker().post([r, req = std::move(req), make = std::move(make)]() mutable {
        if (!ensure_connected()) {
            post_reply(r, infra::kErrTimeout, 0);
            return;
        }
        const bool join = req.value("op", "") == "join";
        infra::client().request(std::move(req), [r, make, join](const nlohmann::json& a) {
            if (!a.value("ok", false)) {
                const uint32_t error = a.value("error", infra::kErrTimeout);
                if (join && error == kErrDuplicateConsole) {
                    std::fprintf(stderr, "[NP2] join refused: another player in the room has this console id "
                                         "(copied adhoc_mac.txt or same PSPRECOMP_ADHOC_MAC)\n");
                }
                post_reply(r, error, 0);
                return;
            }
            uint32_t data = 0;
            if (make) {
                data = build([&](np2::GuestBuf& b) { return make(b, a); });
                if (!data) {
                    post_reply(r, infra::kErrTimeout, 0);
                    return;
                }
            }
            post_reply(r, 0, data);
        }, r.timeout_ms);
    });
}

// Answered here once the server connection is up (one server, one world).
void local_reply(const Reply& r, std::function<uint32_t(np2::GuestBuf&)> write) {
    worker().post([r, write = std::move(write)] {
        if (!ensure_connected()) {
            post_reply(r, infra::kErrTimeout, 0);
            return;
        }
        const uint32_t data = build(write);
        post_reply(r, data ? 0 : infra::kErrTimeout, data);
    });
}

// ---- guest struct readers ----
uint64_t read_u64(uint8_t* rdram, uint32_t a) {
    return psp_mem_read<uint32_t>(rdram, a) | (static_cast<uint64_t>(psp_mem_read<uint32_t>(rdram, a + 4)) << 32);
}

std::string read_hex(uint8_t* rdram, uint32_t ptr, uint32_t size) {
    if (size > 1024 || !valid_range(ptr, size)) return "";
    return infra::to_hex(rdram + (ptr & kAddrMask), size);
}

// {u16 id; pad; ptr; u32 size} x n
nlohmann::json read_bin_attrs(uint8_t* rdram, uint32_t p, uint32_t n) {
    nlohmann::json out = nlohmann::json::array();
    if (n > 16 || !valid_range(p, n * 12)) return out;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t e = p + i * 12;
        out.push_back({psp_mem_read<uint16_t>(rdram, e),
                       read_hex(rdram, psp_mem_read<uint32_t>(rdram, e + 4), psp_mem_read<uint32_t>(rdram, e + 8))});
    }
    return out;
}

// {u16 id; pad; u32 value} x n
nlohmann::json read_int_attrs(uint8_t* rdram, uint32_t p, uint32_t n) {
    nlohmann::json out = nlohmann::json::array();
    if (n > 32 || !valid_range(p, n * 8)) return out;
    for (uint32_t i = 0; i < n; i++)
        out.push_back({psp_mem_read<uint16_t>(rdram, p + i * 8), psp_mem_read<uint32_t>(rdram, p + i * 8 + 4)});
    return out;
}

// SceNpMatching2PresenceOptionData {u8 data[16]; u32 len}
std::string read_opt(uint8_t* rdram, uint32_t p) {
    if (!valid_range(p, 20)) return "";
    uint32_t len = psp_mem_read<uint32_t>(rdram, p + 16);
    if (len > 16) len = 16;
    return infra::to_hex(rdram + (p & kAddrMask), len);
}

std::vector<uint8_t> opt_bytes(const nlohmann::json& e) { return infra::from_hex(e.value("opt", "")); }

// Remembers a room the game entered after a create/join answer.
void enter_room(const nlohmann::json& room, np2::RoomCallback cb) {
    std::lock_guard<std::mutex> lock(st().mu);
    st().rooms.enter(room, cb);
    std::fprintf(stderr, "[NP2] in %zu room(s)\n", st().rooms.ids().size());
}

// ---- pushed events ----
void post_room_event(uint64_t room, uint16_t event, uint32_t data) {
    State& s = st();
    np2::RoomCallback cb;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        cb = s.rooms.callback(room);
    }
    std::fprintf(stderr, "[NP2] room event 0x%04X room %llu\n", event, static_cast<unsigned long long>(room));
    if (!cb.cb || !data) return;
    GuestCall c;
    c.fn = cb.cb;
    c.a[0] = s.ctx_id;
    c.a[1] = 0;
    c.a[2] = static_cast<uint32_t>(room);
    c.a[3] = static_cast<uint32_t>(room >> 32);
    c.a[4] = event;
    c.a[5] = data;
    c.a[6] = cb.arg;
    guest_callbacks_post(g_rdram, std::move(c));
}

void handle_event(const nlohmann::json& e) {
    State& s = st();
    const std::string ev = e.value("ev", "");
    const uint64_t room = e.value("room_id", uint64_t{0});
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (!s.started || !s.rooms.has(room)) {
            if (trace()) std::fprintf(stderr, "[NP2] event %s ignored\n", e.dump().c_str());
            return;
        }
    }
    if (ev == "member_joined" || ev == "member_left") {
        const nlohmann::json m = e.value("member", nlohmann::json::object());
        const uint16_t id = m.value("member_id", uint16_t{0});
        {
            std::lock_guard<std::mutex> lock(s.mu);
            if (ev == "member_joined")
                s.rooms.member_joined(room, id, np2::Peer{m.value("vip", 0u), m.value("port", uint16_t{0})});
            else
                s.rooms.member_left(room, id);
        }
        const uint8_t cause = static_cast<uint8_t>(e.value("cause", 0));
        const std::vector<uint8_t> opt = opt_bytes(e);
        post_room_event(room, ev == "member_joined" ? kEvMemberJoined : kEvMemberLeft,
                        build([&](np2::GuestBuf& b) { return np2::write_member_update(b, m, cause, opt); }));
    } else if (ev == "kicked" || ev == "room_destroyed") {
        const uint8_t cause = static_cast<uint8_t>(e.value("cause", 0));
        const std::vector<uint8_t> opt = opt_bytes(e);
        post_room_event(room, ev == "kicked" ? kEvKickedOut : kEvRoomDestroyed,
                        build([&](np2::GuestBuf& b) { return np2::write_room_update(b, cause, 0, opt); }));
        std::lock_guard<std::mutex> lock(s.mu);
        s.rooms.leave(room);
    } else if (ev == "signaling") {
        const uint16_t id = e.value("member_id", uint16_t{0});
        const uint16_t event = e.value("event", uint16_t{0});
        uint32_t cb, arg;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            if (event == 0x5102)
                s.rooms.signaled(room, id, np2::Peer{e.value("vip", 0u), e.value("port", uint16_t{0})});
            cb = s.sig_cb;
            arg = s.sig_arg;
        }
        std::fprintf(stderr, "[NP2] signaling 0x%04X member 0x%X room %llu\n", event, id,
                     static_cast<unsigned long long>(room));
        if (!cb) return;
        GuestCall c;
        c.fn = cb;
        c.a[0] = s.ctx_id;
        c.a[1] = 0;
        c.a[2] = static_cast<uint32_t>(room);
        c.a[3] = static_cast<uint32_t>(room >> 32);
        c.a[4] = id;
        c.a[5] = event;
        c.a[6] = 0;
        c.a[7] = arg;
        guest_callbacks_post(g_rdram, std::move(c));
    }
}

// Server events go through the worker in arrival order. A member leaving or
// the room going away waits (up to 1.5 s) until the game has read the stream
// data and datagrams that came before it, as with a real host whose last
// packets arrive before the matching server notices it left.
void on_event(const nlohmann::json& e) {
    worker().post([e] {
        const std::string ev = e.value("ev", "");
        const bool leave = ev == "member_left" || ev == "kicked" || ev == "room_destroyed" ||
                           (ev == "signaling" && e.value("event", 0) == 0x5101);
        for (int i = 0; leave && i < 75 && !psp_inet_streams_idle(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        handle_event(e);
    });
}

// ---- lifecycle ----
void hle_m2_ok(const char* n, recomp_context* ctx) {
    log_call(n, ctx);
    ctx->r[2] = SCE_OK;
}

void leave_server() {
    {
        std::lock_guard<std::mutex> lock(st().mu);
        st().started = false;
        st().rooms.clear();
    }
    worker().post([] { infra::client().disconnect(); });
}

void hle_sceNpMatching2Init(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    hle_m2_ok("Init", ctx);
}
void hle_sceNpMatching2Term(uint8_t*, recomp_context* ctx) {
    hle_m2_ok("Term", ctx);
    leave_server();
}
void hle_sceNpMatching2ContextStart(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    {
        std::lock_guard<std::mutex> lock(st().mu);
        st().started = true;
    }
    hle_m2_ok("ContextStart", ctx);
}
void hle_sceNpMatching2ContextStop(uint8_t*, recomp_context* ctx) {
    hle_m2_ok("ContextStop", ctx);
    leave_server();
}
void hle_sceNpMatching2DestroyContext(uint8_t*, recomp_context* ctx) { hle_m2_ok("DestroyContext", ctx); }
void hle_sceNpMatching2AbortRequest(uint8_t*, recomp_context* ctx) { hle_m2_ok("AbortRequest", ctx); }

// CreateContext(commId*, passphrase*, u16* ctxId, option)
void hle_sceNpMatching2CreateContext(uint8_t* rdram, recomp_context* ctx) {
    log_call("CreateContext", ctx);
    const uint32_t out = static_cast<uint32_t>(ctx->r[6]);
    if (!valid_range(out, 2)) {
        ctx->r[2] = (int32_t)0x80550C01;
        return;
    }
    psp_mem_write<uint16_t>(rdram, out, st().ctx_id);
    ctx->r[2] = SCE_OK;
}

void hle_sceNpMatching2GetMemoryStat(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetMemoryStat", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[4]);
    if (valid_range(p, 12)) {
        psp_mem_write<uint32_t>(rdram, p, 0x14000);
        psp_mem_write<uint32_t>(rdram, p + 4, 0x4050);
        psp_mem_write<uint32_t>(rdram, p + 8, 0x14000);
    }
    ctx->r[2] = SCE_OK;
}

// RegisterSignalingCallback(ctxId, cb, arg)
void hle_sceNpMatching2RegisterSignalingCallback(uint8_t*, recomp_context* ctx) {
    log_call("RegisterSignalingCallback", ctx);
    std::lock_guard<std::mutex> lock(st().mu);
    st().sig_cb = static_cast<uint32_t>(ctx->r[5]);
    st().sig_arg = static_cast<uint32_t>(ctx->r[6]);
    ctx->r[2] = SCE_OK;
}

// GetServerIdListLocal(ctxId, u16* ids, max) -> count
void hle_sceNpMatching2GetServerIdListLocal(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetServerIdListLocal", ctx);
    const uint32_t ids = static_cast<uint32_t>(ctx->r[5]);
    if (!valid_range(ids, 2) || ctx->r[6] < 1) {
        ctx->r[2] = (int32_t)0x80550C03;
        return;
    }
    psp_mem_write<uint16_t>(rdram, ids, np2::kServerId);
    ctx->r[2] = 1;
}

// GetServerInfo(ctxId, {u16 serverId}*, optParam, u32* reqId) -> 0x0001 {u16 id, u8 status 1}
void hle_sceNpMatching2GetServerInfo(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetServerInfo", ctx);
    local_reply(begin_request(rdram, ctx, kEvServerInfo, 7),
                [](np2::GuestBuf& b) { return np2::write_server_info(b, np2::kServerId, 1); });
    ctx->r[2] = SCE_OK;
}

// GetWorldInfoList(ctxId, {u16 serverId}*, optParam, u32* reqId) -> 0x0002
void hle_sceNpMatching2GetWorldInfoList(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetWorldInfoList", ctx);
    local_reply(begin_request(rdram, ctx, kEvWorldInfoList, 7),
                [](np2::GuestBuf& b) { return np2::write_world_list(b, np2::kWorldId, 0, 0); });
    ctx->r[2] = SCE_OK;
}

// SearchRoom(ctxId, req*, optParam, u32* reqId) -> 0x0106 {range; RoomDataExternal* list}
// SearchRoomRequest: option, worldId @4, lobbyId u64 @8, range {startIndex @0x10, max @0x14},
// flagFilter @0x18, flagAttr @0x1C, intFilter* @0x20, num @0x24, binFilter* @0x28, num @0x2C,
// attrId (u16)* @0x30, num @0x34. IntSearchFilter (12): u8 op @0, {u16 id @4, u32 value @8}.
void hle_sceNpMatching2SearchRoom(uint8_t* rdram, recomp_context* ctx) {
    log_call("SearchRoom", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[5]);
    const Reply r = begin_request(rdram, ctx, kEvSearchRoom, 7);
    nlohmann::json req = {{"op", "search"}, {"ints", nlohmann::json::array()}, {"attrs", nlohmann::json::array()}};
    uint32_t start = 1;
    if (valid_range(p, 0x38)) {
        req["world"] = psp_mem_read<uint32_t>(rdram, p + 4);
        start = psp_mem_read<uint32_t>(rdram, p + 0x10);
        if (start == 0) start = 1;
        req["start"] = start;
        req["max"] = psp_mem_read<uint32_t>(rdram, p + 0x14);
        if (const uint32_t flag_filter = psp_mem_read<uint32_t>(rdram, p + 0x18)) {
            req["flag_filter"] = flag_filter;
            req["flag_attr"] = psp_mem_read<uint32_t>(rdram, p + 0x1C);
        }
        const uint32_t f = psp_mem_read<uint32_t>(rdram, p + 0x20), n = psp_mem_read<uint32_t>(rdram, p + 0x24);
        if (n <= 16 && valid_range(f, n * 12))
            for (uint32_t i = 0; i < n; i++)
                req["ints"].push_back({rdram[(f + i * 12) & kAddrMask], psp_mem_read<uint16_t>(rdram, f + i * 12 + 4),
                                       psp_mem_read<uint32_t>(rdram, f + i * 12 + 8)});
        const uint32_t a = psp_mem_read<uint32_t>(rdram, p + 0x30), na = psp_mem_read<uint32_t>(rdram, p + 0x34);
        if (na <= 16 && valid_range(a, na * 2))
            for (uint32_t i = 0; i < na; i++) req["attrs"].push_back(psp_mem_read<uint16_t>(rdram, a + i * 2));
    }
    if (trace()) std::fprintf(stderr, "[NP2] search %s\n", req.dump().c_str());
    send_request(r, std::move(req), [start](np2::GuestBuf& b, const nlohmann::json& a) {
        const nlohmann::json rooms = a.value("rooms", nlohmann::json::array());
        std::fprintf(stderr, "[NP2] search: %zu of %u rooms\n", rooms.size(), a.value("total", 0u));
        return np2::write_search_response(b, start, a.value("total", 0u), rooms);
    });
    ctx->r[2] = SCE_OK;
}

// GetRoomDataExternalList(ctxId, {u64* roomId, num, u16* attrId, num}*, optParam, u32* reqId)
// -> 0x0007 (PSP) {RoomDataExternal* list; u32 num}
void hle_sceNpMatching2GetRoomDataExternalList(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetRoomDataExternalList", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[5]);
    const Reply r = begin_request(rdram, ctx, kEvRoomDataExternalList, 7);
    nlohmann::json req = {{"op", "details"}, {"room_ids", nlohmann::json::array()}, {"attrs", nlohmann::json::array()}};
    if (valid_range(p, 16)) {
        const uint32_t ids = psp_mem_read<uint32_t>(rdram, p), n = psp_mem_read<uint32_t>(rdram, p + 4);
        if (n <= 20 && valid_range(ids, n * 8))
            for (uint32_t i = 0; i < n; i++) req["room_ids"].push_back(read_u64(rdram, ids + i * 8));
        const uint32_t a = psp_mem_read<uint32_t>(rdram, p + 8), na = psp_mem_read<uint32_t>(rdram, p + 12);
        if (na <= 16 && valid_range(a, na * 2))
            for (uint32_t i = 0; i < na; i++) req["attrs"].push_back(psp_mem_read<uint16_t>(rdram, a + i * 2));
    }
    send_request(r, std::move(req), [](np2::GuestBuf& b, const nlohmann::json& a) {
        return np2::write_ext_list_response(b, a.value("rooms", nlohmann::json::array()));
    });
    ctx->r[2] = SCE_OK;
}

// Room event callback struct (CreateJoinRoom/JoinRoom 4th argument): {cb, arg, ...}
np2::RoomCallback read_room_callback(uint8_t* rdram, uint32_t p) {
    if (!valid_range(p, 8)) return {};
    return np2::RoomCallback{psp_mem_read<uint32_t>(rdram, p), psp_mem_read<uint32_t>(rdram, p + 4)};
}

// A room's events go to the callback its own create/join request named.
Make room_answer(np2::RoomCallback cb) {
    return [cb](np2::GuestBuf& b, const nlohmann::json& a) {
        const nlohmann::json room = a.value("room", nlohmann::json::object());
        enter_room(room, cb);
        return np2::write_room_int_response(b, room);
    };
}

// CreateJoinRoom(ctxId, req*, optParam, roomEventCb*, msgCb*, u32* reqId) -> 0x0101 {RoomDataInternal*}
// CreateJoinRoomRequest: worldId @0, lobbyId u64 @8, maxSlot @0x10, flagAttr @0x14,
// roomBinAttrInternal* @0x18, num @0x1C, searchable int attrs* @0x20, num @0x24, searchable bin
// attrs* @0x28, num @0x2C, roomBinAttrExternal* @0x30, num @0x34, ..., roomMemberBinAttrInternal*
// @0x60, num @0x64.
void hle_sceNpMatching2CreateJoinRoom(uint8_t* rdram, recomp_context* ctx) {
    log_call("CreateJoinRoom", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[5]);
    const np2::RoomCallback cb = read_room_callback(rdram, static_cast<uint32_t>(ctx->r[7]));
    const Reply r = begin_request(rdram, ctx, kEvCreateJoinRoom, 9);
    nlohmann::json req = {{"op", "create"}, {"max_slot", 4}};
    if (valid_range(p, 0x68)) {
        auto u = [&](uint32_t o) { return psp_mem_read<uint32_t>(rdram, p + o); };
        req["max_slot"] = u(0x10);
        req["flag_attr"] = u(0x14);
        req["bins_internal"] = read_bin_attrs(rdram, u(0x18), u(0x1C));
        req["ints"] = read_int_attrs(rdram, u(0x20), u(0x24));
        req["bins"] = read_bin_attrs(rdram, u(0x30), u(0x34));
        req["member_bins"] = read_bin_attrs(rdram, u(0x60), u(0x64));
        if (u(0x2C)) std::fprintf(stderr, "[NP2] CreateJoinRoom: %u searchable bin attrs not published\n", u(0x2C));
    }
    if (trace()) std::fprintf(stderr, "[NP2] create %s\n", req.dump().c_str());
    send_request(r, std::move(req), room_answer(cb));
    ctx->r[2] = SCE_OK;
}

// JoinRoom(ctxId, req*, optParam, roomEventCb*, msgCb*, u32* reqId) -> 0x0102 {RoomDataInternal*}
// JoinRoomRequest: roomId u64 @0, password* @8, label* @0xC, roomMemberBinAttrInternal* @0x10,
// num @0x14, optData {data[16] @0x18, len @0x28}, teamId @0x2C.
void hle_sceNpMatching2JoinRoom(uint8_t* rdram, recomp_context* ctx) {
    log_call("JoinRoom", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[5]);
    const np2::RoomCallback cb = read_room_callback(rdram, static_cast<uint32_t>(ctx->r[7]));
    const Reply r = begin_request(rdram, ctx, kEvJoinRoom, 9);
    nlohmann::json req = {{"op", "join"}, {"room_id", 0}};
    if (valid_range(p, 0x30)) {
        req["room_id"] = read_u64(rdram, p);
        req["member_bins"] = read_bin_attrs(rdram, psp_mem_read<uint32_t>(rdram, p + 0x10),
                                            psp_mem_read<uint32_t>(rdram, p + 0x14));
        req["opt"] = read_opt(rdram, p + 0x18);
    }
    std::fprintf(stderr, "[NP2] join room %llu\n", static_cast<unsigned long long>(req["room_id"].get<uint64_t>()));
    send_request(r, std::move(req), room_answer(cb));
    ctx->r[2] = SCE_OK;
}

// LeaveRoom(ctxId, {u64 roomId; optData @8}*, optParam, u32* reqId) -> 0x0103
void hle_sceNpMatching2LeaveRoom(uint8_t* rdram, recomp_context* ctx) {
    log_call("LeaveRoom", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[5]);
    const Reply r = begin_request(rdram, ctx, kEvLeaveRoom, 7);
    const uint64_t room = valid_range(p, 0x1C) ? read_u64(rdram, p) : 0;
    nlohmann::json req = {{"op", "leave"}, {"room_id", room}, {"opt", read_opt(rdram, p + 8)}};
    send_request(r, std::move(req), [room](np2::GuestBuf& b, const nlohmann::json&) {
        std::lock_guard<std::mutex> lock(st().mu);
        st().rooms.leave(room);  // the game's other rooms stay
        return b.alloc(4);  // the game's handler reads no data; any non-null block
    });
    ctx->r[2] = SCE_OK;
}

// KickoutRoomMember(ctxId, {u64 roomId; u16 target @8; optData @0xC}*, optParam, u32* reqId) -> 0x0105
void hle_sceNpMatching2KickoutRoomMember(uint8_t* rdram, recomp_context* ctx) {
    log_call("KickoutRoomMember", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[5]);
    const Reply r = begin_request(rdram, ctx, kEvKickout, 7);
    nlohmann::json req = {{"op", "kick"}, {"room_id", 0}, {"member_id", 0}};
    if (valid_range(p, 0x20)) {
        req["room_id"] = read_u64(rdram, p);
        req["member_id"] = psp_mem_read<uint16_t>(rdram, p + 8);
        req["opt"] = read_opt(rdram, p + 0xC);
    }
    send_request(r, std::move(req), [](np2::GuestBuf& b, const nlohmann::json&) { return b.alloc(4); });
    ctx->r[2] = SCE_OK;
}

// SetRoomDataInternal(ctxId, {u64 roomId; flagFilter @8; flagAttr @0xC; binAttr* @0x10; num @0x14}*,
// optParam, u32* reqId) -> 0x0109
void hle_sceNpMatching2SetRoomDataInternal(uint8_t* rdram, recomp_context* ctx) {
    log_call("SetRoomDataInternal", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[5]);
    const Reply r = begin_request(rdram, ctx, kEvSetRoomDataInternal, 7);
    nlohmann::json req = {{"op", "set_internal"}, {"room_id", 0}};
    if (valid_range(p, 0x18)) {
        req["room_id"] = read_u64(rdram, p);
        req["bins"] = read_bin_attrs(rdram, psp_mem_read<uint32_t>(rdram, p + 0x10), psp_mem_read<uint32_t>(rdram, p + 0x14));
        if (const uint32_t flag_filter = psp_mem_read<uint32_t>(rdram, p + 8)) {
            req["flag_filter"] = flag_filter;
            req["flag_attr"] = psp_mem_read<uint32_t>(rdram, p + 0xC);
        }
    }
    if (trace()) std::fprintf(stderr, "[NP2] set_internal %s\n", req.dump().c_str());
    send_request(r, std::move(req), [](np2::GuestBuf& b, const nlohmann::json&) { return b.alloc(4); });
    ctx->r[2] = SCE_OK;
}

// SignalingGetConnectionStatus(ctxId, -, roomId lo, roomId hi, memberId, int* status,
// in_addr* addr, u16* port): the member's virtual IP and game port in that room, both in network order.
void hle_sceNpMatching2SignalingGetConnectionStatus(uint8_t* rdram, recomp_context* ctx) {
    log_call("SignalingGetConnectionStatus", ctx);
    const uint64_t room = static_cast<uint32_t>(ctx->r[6]) | (static_cast<uint64_t>(static_cast<uint32_t>(ctx->r[7])) << 32);
    const uint16_t member = static_cast<uint16_t>(ctx->r[8]);
    const uint32_t status = static_cast<uint32_t>(ctx->r[9]), addr = static_cast<uint32_t>(ctx->r[10]),
                   port = static_cast<uint32_t>(ctx->r[11]);
    np2::Peer peer;
    bool known;
    {
        std::lock_guard<std::mutex> lock(st().mu);
        known = st().rooms.peer(room, member, &peer);
    }
    if (valid_range(status, 4)) psp_mem_write<int32_t>(rdram, status, known ? kSignalingActive : 0);
    if (known && valid_range(addr, 4)) {
        const uint8_t b[4] = {uint8_t(peer.vip >> 24), uint8_t(peer.vip >> 16), uint8_t(peer.vip >> 8), uint8_t(peer.vip)};
        std::memcpy(rdram + (addr & kAddrMask), b, 4);
    }
    if (known && valid_range(port, 2)) {
        const uint8_t b[2] = {uint8_t(peer.port >> 8), uint8_t(peer.port)};
        std::memcpy(rdram + (port & kAddrMask), b, 2);
    }
    std::fprintf(stderr, "[NP2] connection status room %llu member 0x%X: %s %u.%u.%u.%u:%u\n",
                 static_cast<unsigned long long>(room), member, known ? "active" : "unknown", peer.vip >> 24, (peer.vip >> 16) & 255, (peer.vip >> 8) & 255,
                 peer.vip & 255, peer.port);
    ctx->r[2] = SCE_OK;
}

}  // namespace

void psp_infra_connect_async() {
    if (g_connecting.exchange(true)) return;
    worker().post([] {
        ensure_connected();
        g_connecting = false;
    });
}

bool psp_infra_connecting() { return g_connecting; }

void psp_hle_register_np_matching2() {
    struct { uint32_t nid; const char* name; HleFunc fn; } const t[] = {
        {0x2E61F6E1, "sceNpMatching2Init", hle_sceNpMatching2Init},
        {0x8BF37D8C, "sceNpMatching2Term", hle_sceNpMatching2Term},
        {0x5030CC53, "sceNpMatching2CreateContext", hle_sceNpMatching2CreateContext},
        {0x190FF903, "sceNpMatching2ContextStart", hle_sceNpMatching2ContextStart},
        {0x2B3892FC, "sceNpMatching2ContextStop", hle_sceNpMatching2ContextStop},
        {0x3DE70241, "sceNpMatching2DestroyContext", hle_sceNpMatching2DestroyContext},
        {0x22F38DAF, "sceNpMatching2GetMemoryStat", hle_sceNpMatching2GetMemoryStat},
        {0xA3C298D1, "sceNpMatching2RegisterSignalingCallback", hle_sceNpMatching2RegisterSignalingCallback},
        {0xF47342FC, "sceNpMatching2GetServerIdListLocal", hle_sceNpMatching2GetServerIdListLocal},
        {0x4EE3A8EC, "sceNpMatching2GetServerInfo", hle_sceNpMatching2GetServerInfo},
        {0xA53E7C69, "sceNpMatching2GetWorldInfoList", hle_sceNpMatching2GetWorldInfoList},
        {0x81C13E6D, "sceNpMatching2SearchRoom", hle_sceNpMatching2SearchRoom},
        {0xAAD0946A, "sceNpMatching2CreateJoinRoom", hle_sceNpMatching2CreateJoinRoom},
        {0x7BBFC427, "sceNpMatching2JoinRoom", hle_sceNpMatching2JoinRoom},
        {0xC870535A, "sceNpMatching2LeaveRoom", hle_sceNpMatching2LeaveRoom},
        {0x97529ECC, "sceNpMatching2KickoutRoomMember", hle_sceNpMatching2KickoutRoomMember},
        {0xE6C93DBD, "sceNpMatching2SetRoomDataInternal", hle_sceNpMatching2SetRoomDataInternal},
        {0x12C5A111, "sceNpMatching2GetRoomDataExternalList", hle_sceNpMatching2GetRoomDataExternalList},
        {0x6D6D0C75, "sceNpMatching2SignalingGetConnectionStatus", hle_sceNpMatching2SignalingGetConnectionStatus},
        {0xFADBA9DB, "sceNpMatching2AbortRequest", hle_sceNpMatching2AbortRequest},
    };
    static char names[sizeof(t) / sizeof(t[0])][16];
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        psp_hle_register(t[i].name, t[i].fn);
        std::snprintf(names[i], sizeof(names[i]), "NID_0x%08X", t[i].nid);
        psp_hle_register(names[i], t[i].fn);
    }
}
