// sceNpMatching2 imports (infrastructure-mode room search). The PSP library
// takes the PS3 SceNpMatching2 structs (little-endian, linked lists through a
// leading next pointer) and hands the response data straight to the request
// callback: cb(ctxId, reqId, event, errorCode, data*, cbArg), from the game's
// handlers at 0x089E92E8 / 0x089E93CC. RECON: answers from a local fake
// server (one server, one world, no rooms yet) to map what the game needs.
#include "hle/psp_hle.h"
#include "hle/psp_hle_np.h"
#include "psp_memory.h"
#include "recomp.h"
#include "net/guest_callbacks.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

static constexpr uint32_t kAddrMask = 0x07FFFFFFU;
static constexpr uint32_t kNullPage = 0x00010000U;

static bool valid_range(uint32_t addr, uint32_t len) {
    if (addr < kNullPage) return false;
    return static_cast<uint64_t>(addr & kAddrMask) + len <= PSP_MEM_SIZE;
}

static void log_call(const char* name, const recomp_context* ctx) {
    std::fprintf(stderr, "[NP2] %s(a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X)\n", name,
                 (uint32_t)ctx->r[4], (uint32_t)ctx->r[5], (uint32_t)ctx->r[6], (uint32_t)ctx->r[7],
                 (uint32_t)ctx->r[8], (uint32_t)ctx->r[9]);
}

static void dump(uint8_t* rdram, const char* what, uint32_t addr, uint32_t len) {
    if (!valid_range(addr, len)) {
        std::fprintf(stderr, "[NP2]   %s @%08X (invalid)\n", what, addr);
        return;
    }
    std::fprintf(stderr, "[NP2]   %s @%08X:", what, addr);
    for (uint32_t i = 0; i < len; i++) {
        if (i % 16 == 0) std::fprintf(stderr, "\n[NP2]     +%02X ", i);
        std::fprintf(stderr, "%02X ", rdram[(addr + i) & kAddrMask]);
    }
    std::fprintf(stderr, "\n");
}

// ---- response arena: guest memory the callback data lives in ----
// Four 16 KiB slots used round-robin; the game copies what it needs while
// the callback runs, and a slot is reused only four responses later.
static constexpr uint32_t kSlotSize = 0x4000;
static constexpr int kSlots = 4;

static uint32_t arena_slot() {
    static uint32_t base = 0;
    static int next = 0;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (base == 0) base = psp_alloc_kernel_memory(kSlotSize * kSlots);
    if (base == 0) return 0;
    const uint32_t a = base + kSlotSize * static_cast<uint32_t>(next);
    next = (next + 1) % kSlots;
    return a;
}

struct Writer {
    uint8_t* rdram;
    uint32_t base, used = 0;
    uint32_t alloc(uint32_t n) {
        used = (used + 3) & ~3u;
        const uint32_t a = base + used;
        used += n;
        std::memset(rdram + (a & kAddrMask), 0, n);
        return a;
    }
    void u8(uint32_t a, uint8_t v) { rdram[a & kAddrMask] = v; }
    void u16(uint32_t a, uint16_t v) { psp_mem_write<uint16_t>(rdram, a, v); }
    void u32(uint32_t a, uint32_t v) { psp_mem_write<uint32_t>(rdram, a, v); }
};

// optParam: {cbFunc, cbArg, timeout, u16 appReqId}
static uint32_t g_req_id = 0;

static void respond(uint8_t* rdram, recomp_context* ctx, uint16_t event, uint32_t data, int assigned_reg = 7) {
    const uint32_t ctx_id = static_cast<uint32_t>(ctx->r[4]) & 0xFFFF;
    const uint32_t opt = static_cast<uint32_t>(ctx->r[6]);
    const uint32_t assigned = static_cast<uint32_t>(ctx->r[assigned_reg]);
    const uint32_t req = ++g_req_id;
    if (valid_range(assigned, 4)) psp_mem_write<uint32_t>(rdram, assigned, req);
    if (!valid_range(opt, 8)) {
        std::fprintf(stderr, "[NP2]   no optParam: event 0x%04X dropped\n", event);
        return;
    }
    const uint32_t cb = psp_mem_read<uint32_t>(rdram, opt), arg = psp_mem_read<uint32_t>(rdram, opt + 4);
    GuestCall c;
    c.fn = cb;
    c.a[0] = ctx_id;
    c.a[1] = req;
    c.a[2] = event;
    c.a[3] = 0;     // errorCode
    c.a[4] = data;  // response data
    c.a[5] = arg;
    std::fprintf(stderr, "[NP2]   -> cb %08X(ctx=%u req=%u event=0x%04X data=%08X arg=%08X)\n", cb, ctx_id, req, event,
                 data, arg);
    guest_callbacks_post(rdram, std::move(c));
}

// ---- RECON room store ----
// CreateJoinRoom saves the room's external attributes to
// $PSPRECOMP_INFRA_ROOMS/<roomId>.room so a SearchRoom in another instance
// can list it. Record: u64 roomId, u32 maxSlot, char owner[16], u32 nInt,
// nInt x {u16 id, u32 value}, u32 nBin, nBin x {u16 id, u32 size, data}.
static std::string rooms_dir() {
    const char* d = std::getenv("PSPRECOMP_INFRA_ROOMS");
    return d && *d ? d : "";
}

template <class T> static void put(std::vector<uint8_t>& v, T x) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), b, b + sizeof(T));
}

static void save_room(uint8_t* rdram, uint32_t req, uint64_t room_id, const char* owner) {
    const std::string dir = rooms_dir();
    if (dir.empty() || !valid_range(req, 0x38)) return;
    std::vector<uint8_t> v;
    put<uint64_t>(v, room_id);
    put<uint32_t>(v, psp_mem_read<uint32_t>(rdram, req + 0x10));
    char o[16] = {};
    std::strncpy(o, owner, 15);
    v.insert(v.end(), o, o + 16);
    const uint32_t ia = psp_mem_read<uint32_t>(rdram, req + 0x20), in = psp_mem_read<uint32_t>(rdram, req + 0x24);
    const bool iok = in <= 32 && valid_range(ia, in * 8);
    put<uint32_t>(v, iok ? in : 0);
    for (uint32_t i = 0; iok && i < in; i++) {
        put<uint16_t>(v, psp_mem_read<uint16_t>(rdram, ia + i * 8));
        put<uint32_t>(v, psp_mem_read<uint32_t>(rdram, ia + i * 8 + 4));
    }
    const uint32_t ba = psp_mem_read<uint32_t>(rdram, req + 0x30), bn = psp_mem_read<uint32_t>(rdram, req + 0x34);
    const bool bok = bn <= 32 && valid_range(ba, bn * 12);
    put<uint32_t>(v, bok ? bn : 0);
    for (uint32_t i = 0; bok && i < bn; i++) {
        const uint32_t ptr = psp_mem_read<uint32_t>(rdram, ba + i * 12 + 4);
        uint32_t size = psp_mem_read<uint32_t>(rdram, ba + i * 12 + 8);
        if (size > 1024 || !valid_range(ptr, size)) size = 0;
        put<uint16_t>(v, psp_mem_read<uint16_t>(rdram, ba + i * 12));
        put<uint32_t>(v, size);
        v.insert(v.end(), rdram + (ptr & kAddrMask), rdram + (ptr & kAddrMask) + size);
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[32];
    std::snprintf(name, sizeof(name), "/%016llX.room", static_cast<unsigned long long>(room_id));
    std::ofstream(dir + name, std::ios::binary).write(reinterpret_cast<const char*>(v.data()),
                                                      static_cast<std::streamsize>(v.size()));
    std::fprintf(stderr, "[NP2]   room %016llX saved (%zu bytes)\n", static_cast<unsigned long long>(room_id), v.size());
}

struct StoredRoom {
    uint64_t id = 0;
    uint32_t max_slot = 4;
    char owner[16] = {};
    std::vector<std::pair<uint16_t, uint32_t>> ints;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> bins;
};

static std::vector<StoredRoom> load_rooms() {
    std::vector<StoredRoom> out;
    const std::string dir = rooms_dir();
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<uint8_t> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t o = 0;
        auto get = [&](void* dst, size_t n) {
            if (o + n > v.size()) return false;
            std::memcpy(dst, v.data() + o, n);
            o += n;
            return true;
        };
        StoredRoom r;
        uint32_t n = 0;
        if (!get(&r.id, 8) || !get(&r.max_slot, 4) || !get(r.owner, 16) || !get(&n, 4)) continue;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t id;
            uint32_t val;
            if (!get(&id, 2) || !get(&val, 4)) break;
            r.ints.push_back({id, val});
        }
        if (!get(&n, 4)) continue;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t id;
            uint32_t size;
            if (!get(&id, 2) || !get(&size, 4) || o + size > v.size()) break;
            r.bins.push_back({id, std::vector<uint8_t>(v.begin() + static_cast<long>(o), v.begin() + static_cast<long>(o + size))});
            o += size;
        }
        r.owner[15] = 0;
        if (r.max_slot == 0 || r.max_slot > 16) r.max_slot = 4;
        out.push_back(std::move(r));
    }
    return out;
}

// ---- lifecycle ----
static void hle_m2_ok(const char* n, recomp_context* ctx) {
    log_call(n, ctx);
    ctx->r[2] = SCE_OK;
}
static void hle_sceNpMatching2Init(uint8_t*, recomp_context* ctx) { hle_m2_ok("Init", ctx); }
static void hle_sceNpMatching2Term(uint8_t*, recomp_context* ctx) { hle_m2_ok("Term", ctx); }
static void hle_sceNpMatching2ContextStart(uint8_t*, recomp_context* ctx) { hle_m2_ok("ContextStart", ctx); }
static void hle_sceNpMatching2ContextStop(uint8_t*, recomp_context* ctx) { hle_m2_ok("ContextStop", ctx); }
static void hle_sceNpMatching2DestroyContext(uint8_t*, recomp_context* ctx) { hle_m2_ok("DestroyContext", ctx); }
static void hle_sceNpMatching2AbortRequest(uint8_t*, recomp_context* ctx) { hle_m2_ok("AbortRequest", ctx); }

// CreateContext(commId*, passphrase*, u16* ctxId, option)
static void hle_sceNpMatching2CreateContext(uint8_t* rdram, recomp_context* ctx) {
    log_call("CreateContext", ctx);
    dump(rdram, "commId", static_cast<uint32_t>(ctx->r[4]), 16);
    const uint32_t out = static_cast<uint32_t>(ctx->r[6]);
    if (!valid_range(out, 2)) { ctx->r[2] = (int32_t)0x80550C01; return; }
    psp_mem_write<uint16_t>(rdram, out, 1);
    ctx->r[2] = SCE_OK;
}

static void hle_sceNpMatching2GetMemoryStat(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetMemoryStat", ctx);
    const uint32_t p = static_cast<uint32_t>(ctx->r[4]);
    if (valid_range(p, 12)) {
        psp_mem_write<uint32_t>(rdram, p, 0x14000);
        psp_mem_write<uint32_t>(rdram, p + 4, 0x4050);
        psp_mem_write<uint32_t>(rdram, p + 8, 0x14000);
    }
    ctx->r[2] = SCE_OK;
}

static uint32_t g_signaling_cb = 0, g_signaling_arg = 0;
static void hle_sceNpMatching2RegisterSignalingCallback(uint8_t*, recomp_context* ctx) {
    log_call("RegisterSignalingCallback", ctx);
    g_signaling_cb = static_cast<uint32_t>(ctx->r[5]);
    g_signaling_arg = static_cast<uint32_t>(ctx->r[6]);
    ctx->r[2] = SCE_OK;
}

// ---- requests ----
static constexpr uint16_t kServerId = 1;
static constexpr uint32_t kWorldId = 1;

// GetServerIdListLocal(ctxId, u16* ids, max) -> count
static void hle_sceNpMatching2GetServerIdListLocal(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetServerIdListLocal", ctx);
    const uint32_t ids = static_cast<uint32_t>(ctx->r[5]);
    if (!valid_range(ids, 2) || ctx->r[6] < 1) { ctx->r[2] = (int32_t)0x80550C03; return; }
    psp_mem_write<uint16_t>(rdram, ids, kServerId);
    ctx->r[2] = 1;
}

// GetServerInfo(ctxId, {u16 serverId}, optParam, u32* reqId)
// -> event 0x0001, data {u16 serverId; u8 status (1 = available); u8}
static void hle_sceNpMatching2GetServerInfo(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetServerInfo", ctx);
    Writer w{rdram, arena_slot()};
    const uint32_t d = w.alloc(4);
    w.u16(d, kServerId);
    w.u8(d + 2, 1);
    respond(rdram, ctx, 0x0001, d);
    ctx->r[2] = SCE_OK;
}

// GetWorldInfoList(ctxId, {u16 serverId}, optParam, u32* reqId)
// -> event 0x0002, data {World* list; u32 count}; World: next*, worldId,
//    numOfLobby, maxLobbyMembers, curLobbyMembers, curRooms, curRoomMembers,
//    withEntitlementId, entitlementId[32]  (64 bytes)
static void hle_sceNpMatching2GetWorldInfoList(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetWorldInfoList", ctx);
    Writer w{rdram, arena_slot()};
    const uint32_t d = w.alloc(8);
    const uint32_t world = w.alloc(64);
    w.u32(world + 4, kWorldId);
    w.u32(world + 0x14, 0);  // rooms
    w.u32(d, world);
    w.u32(d + 4, 1);
    respond(rdram, ctx, 0x0002, d);
    ctx->r[2] = SCE_OK;
}

static uint32_t write_room_ext(Writer& w, uint8_t* rdram, const StoredRoom& r) {
    const uint32_t e = w.alloc(0x58);
    w.u16(e + 4, kServerId);
    w.u32(e + 8, kWorldId);
    w.u16(e + 0xC, static_cast<uint16_t>(r.max_slot));
    w.u32(e + 0x18, static_cast<uint32_t>(r.id));
    w.u32(e + 0x1C, static_cast<uint32_t>(r.id >> 32));
    w.u16(e + 0x20, static_cast<uint16_t>(r.max_slot - 1));
    w.u16(e + 0x22, static_cast<uint16_t>(r.max_slot));
    w.u32(e + 0x24, 1);  // PSP reads the member count as a u32 here (PS3: u16 openPrivateSlotNum, curMemberNum)
    const uint32_t owner = w.alloc(44), on = w.alloc(52);
    for (uint32_t i = 0; r.owner[i] && i < 16; i++) {
        w.u8(owner + i, static_cast<uint8_t>(r.owner[i]));
        w.u8(on + i, static_cast<uint8_t>(r.owner[i]));
    }
    w.u32(owner + 36, on);
    w.u32(e + 0x30, owner);
    const uint32_t ia = w.alloc(static_cast<uint32_t>(r.ints.size()) * 8);
    for (size_t i = 0; i < r.ints.size(); i++) {
        w.u16(ia + static_cast<uint32_t>(i) * 8, r.ints[i].first);
        w.u32(ia + static_cast<uint32_t>(i) * 8 + 4, r.ints[i].second);
    }
    w.u32(e + 0x40, ia);
    w.u32(e + 0x44, static_cast<uint32_t>(r.ints.size()));
    const uint32_t ba = w.alloc(static_cast<uint32_t>(r.bins.size()) * 12);
    for (size_t i = 0; i < r.bins.size(); i++) {
        const uint32_t data = w.alloc(static_cast<uint32_t>(r.bins[i].second.size()));
        std::memcpy(rdram + (data & kAddrMask), r.bins[i].second.data(), r.bins[i].second.size());
        w.u16(ba + static_cast<uint32_t>(i) * 12, r.bins[i].first);
        w.u32(ba + static_cast<uint32_t>(i) * 12 + 4, data);
        w.u32(ba + static_cast<uint32_t>(i) * 12 + 8, static_cast<uint32_t>(r.bins[i].second.size()));
    }
    w.u32(e + 0x50, ba);
    w.u32(e + 0x54, static_cast<uint32_t>(r.bins.size()));
    return e;
}

// SearchRoom(ctxId, SearchRoomRequest*, optParam, u32* reqId)
// -> event 0x0106, data {range {startIndex, total, size}; RoomDataExternal* list}
static void hle_sceNpMatching2SearchRoom(uint8_t* rdram, recomp_context* ctx) {
    log_call("SearchRoom", ctx);
    const uint32_t req = static_cast<uint32_t>(ctx->r[5]);
    dump(rdram, "SearchRoomRequest", req, 0x38);
    if (valid_range(req, 0x38)) {
        const uint32_t f = psp_mem_read<uint32_t>(rdram, req + 0x20), n = psp_mem_read<uint32_t>(rdram, req + 0x24);
        if (n && n <= 16) dump(rdram, "intFilters", f, n * 12);
    }
    const std::vector<StoredRoom> rooms = load_rooms();
    Writer w{rdram, arena_slot()};
    const uint32_t d = w.alloc(16);
    w.u32(d, 1);  // startIndex
    w.u32(d + 4, static_cast<uint32_t>(rooms.size()));
    w.u32(d + 8, static_cast<uint32_t>(rooms.size()));
    // RoomDataExternal (0x58): next*, serverId @4, worldId @8, publicSlotNum @0xC,
    // privateSlotNum @0xE, lobbyId @0x10, roomId @0x18, openPublicSlotNum @0x20,
    // maxSlot @0x22, openPrivateSlotNum @0x24, curMemberNum @0x26, passwordSlotMask @0x28,
    // owner* (SceNpUserInfo2) @0x30, roomGroup* @0x34, num @0x38, flagAttr @0x3C,
    // searchableIntAttrExternal* @0x40, num @0x44, searchableBinAttrExternal* @0x48,
    // num @0x4C, roomBinAttrExternal* @0x50, num @0x54
    uint32_t prev = 0;
    for (const StoredRoom& r : rooms) {
        const uint32_t e = write_room_ext(w, rdram, r);
        if (prev) w.u32(prev, e); else w.u32(d + 12, e);
        prev = e;
        std::fprintf(stderr, "[NP2]   listing room %016llX owner \"%s\" (%zu int, %zu bin attrs)\n",
                     static_cast<unsigned long long>(r.id), r.owner, r.ints.size(), r.bins.size());
    }
    respond(rdram, ctx, 0x0106, d);
    ctx->r[2] = SCE_OK;
}

// Dumps the request and, two levels deep, 64 bytes behind every word in it
// that is a guest pointer (attribute arrays and their binary data).
static void dump_deep(uint8_t* rdram, const char* what, uint32_t addr, uint32_t len, int depth) {
    dump(rdram, what, addr, len);
    if (depth == 0 || !valid_range(addr, len)) return;
    for (uint32_t o = 0; o + 4 <= len; o += 4) {
        const uint32_t p = psp_mem_read<uint32_t>(rdram, addr + o);
        if (((p >= 0x08800000U && p < 0x0A000000U) || (p >= 0x0BF00000U && p < 0x0C000000U)) && p != addr) {
            char name[48];
            std::snprintf(name, sizeof(name), "%s+%02X->", what, o);
            dump_deep(rdram, name, p, 0x40, depth - 1);
        }
    }
}

static void hle_unknown_req(const char* n, uint8_t* rdram, recomp_context* ctx, uint32_t req_len) {
    log_call(n, ctx);
    dump_deep(rdram, "req", static_cast<uint32_t>(ctx->r[5]), req_len, 2);
    dump(rdram, "opt", static_cast<uint32_t>(ctx->r[6]), 0x10);
    ctx->r[2] = SCE_OK;
}
// RoomMemberDataInternal (0x50): next*, npId[36] @4, onlineName* @0x28,
// avatarUrl* @0x2C, joinDate u64 @0x30, memberId u16 @0x38, teamId @0x3A,
// roomGroup* @0x3C, natType @0x40, flagAttr @0x44, binAttrInternal* @0x48, num @0x4C
static uint32_t write_member(Writer& w, const char* name, uint16_t member_id) {
    const uint32_t m = w.alloc(0x50);
    const uint32_t on = w.alloc(52);
    for (size_t i = 0; name[i] && i < 16; i++) {
        w.u8(m + 4 + static_cast<uint32_t>(i), static_cast<uint8_t>(name[i]));
        w.u8(on + static_cast<uint32_t>(i), static_cast<uint8_t>(name[i]));
    }
    w.u32(m + 0x28, on);
    w.u16(m + 0x38, member_id);
    return m;
}

static uint64_t g_room_id = 0x0001000000000001ULL;

// CreateJoinRoom(ctxId, CreateJoinRoomRequest*, optParam, roomEventCb*, msgCb*, u32* reqId)
// -> event 0x0101, data {RoomDataInternal*}; RoomDataInternal (0x48): serverId,
//    worldId @4, lobbyId @8, roomId @0x10, passwordSlotMask @0x18, maxSlot @0x20,
//    members* @0x24, membersNum @0x28, me* @0x2C, owner* @0x30, roomGroup* @0x34,
//    roomGroupNum @0x38, flagAttr @0x3C, binAttrInternal* @0x40, num @0x44
static void hle_sceNpMatching2CreateJoinRoom(uint8_t* rdram, recomp_context* ctx) {
    hle_unknown_req("CreateJoinRoom", rdram, ctx, 0x70);
    dump(rdram, "roomEventCb", static_cast<uint32_t>(ctx->r[7]), 0x18);
    dump(rdram, "msgCb", static_cast<uint32_t>(ctx->r[8]), 0x8);
    const uint32_t req = static_cast<uint32_t>(ctx->r[5]);
    Writer w{rdram, arena_slot()};
    const uint32_t d = w.alloc(4);
    const uint32_t room = w.alloc(0x48);
    const uint32_t me = write_member(w, "Patapon", 0x10);
    w.u16(room, kServerId);
    w.u32(room + 4, kWorldId);
    w.u32(room + 0x10, static_cast<uint32_t>(g_room_id));
    w.u32(room + 0x14, static_cast<uint32_t>(g_room_id >> 32));
    w.u32(room + 0x20, valid_range(req, 0x14) ? psp_mem_read<uint32_t>(rdram, req + 0x10) : 4);
    w.u32(room + 0x24, me);
    w.u32(room + 0x28, 1);
    w.u32(room + 0x2C, me);
    w.u32(room + 0x30, me);
    w.u32(d, room);
    save_room(rdram, req, g_room_id, "Patapon");
    respond(rdram, ctx, 0x0101, d, 9);
    ctx->r[2] = SCE_OK;
}
static void hle_sceNpMatching2JoinRoom(uint8_t* r, recomp_context* c) { hle_unknown_req("JoinRoom", r, c, 0x40); }
static void hle_sceNpMatching2LeaveRoom(uint8_t* r, recomp_context* c) { hle_unknown_req("LeaveRoom", r, c, 0x20); }
static void hle_sceNpMatching2KickoutRoomMember(uint8_t* r, recomp_context* c) { hle_unknown_req("KickoutRoomMember", r, c, 0x20); }
static void hle_sceNpMatching2SetRoomDataInternal(uint8_t* r, recomp_context* c) { hle_unknown_req("SetRoomDataInternal", r, c, 0x40); }
// GetRoomDataExternalList(ctxId, {u64* roomId, num, u16* attrId, num}, optParam, u32* reqId)
// -> event 0x0007 (PSP), data {RoomDataExternal* list; u32 num}
static void hle_sceNpMatching2GetRoomDataExternalList(uint8_t* rdram, recomp_context* ctx) {
    log_call("GetRoomDataExternalList", ctx);
    const uint32_t req = static_cast<uint32_t>(ctx->r[5]);
    const std::vector<StoredRoom> rooms = load_rooms();
    Writer w{rdram, arena_slot()};
    const uint32_t d = w.alloc(8);
    uint32_t prev = 0, n = 0;
    const uint32_t ids = valid_range(req, 8) ? psp_mem_read<uint32_t>(rdram, req) : 0;
    const uint32_t num = valid_range(req, 8) ? psp_mem_read<uint32_t>(rdram, req + 4) : 0;
    for (uint32_t i = 0; i < num && i < 20 && valid_range(ids + i * 8, 8); i++) {
        const uint64_t id = psp_mem_read<uint32_t>(rdram, ids + i * 8) |
                            (static_cast<uint64_t>(psp_mem_read<uint32_t>(rdram, ids + i * 8 + 4)) << 32);
        for (const StoredRoom& r : rooms) {
            if (r.id != id) continue;
            const uint32_t e = write_room_ext(w, rdram, r);
            if (prev) w.u32(prev, e); else w.u32(d, e);
            prev = e;
            n++;
            std::fprintf(stderr, "[NP2]   details for room %016llX\n", static_cast<unsigned long long>(id));
        }
    }
    w.u32(d + 4, n);
    respond(rdram, ctx, 0x0007, d);  // PSP: 7 (PS3 numbers it 0x0005); the game handles only 7/0x106
    ctx->r[2] = SCE_OK;
}
static void hle_sceNpMatching2SignalingGetConnectionStatus(uint8_t* r, recomp_context* c) { hle_unknown_req("SignalingGetConnectionStatus", r, c, 0x10); }

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
