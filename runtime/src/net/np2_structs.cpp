// sceNpMatching2 response structs (see net/np2_structs.h). Offsets are the
// PS3 layout from RPCS3 sceNp2.h unless the spec lists a PSP difference.
#include "net/np2_structs.h"

#include <cstring>
#include <string>

#include "net/infra_protocol.h"

namespace np2 {

namespace {

constexpr uint64_t kUnixEpochTick = 62135596800000000ULL;  // CellRtcTick (us since 0001-01-01) of 1970-01-01

struct W {
    GuestBuf& b;
    uint8_t* p(uint32_t a) { return b.base_host + (a - b.base_guest); }
    void u8(uint32_t a, uint8_t v) { *p(a) = v; }
    void u16(uint32_t a, uint16_t v) { std::memcpy(p(a), &v, 2); }
    void u32(uint32_t a, uint32_t v) { std::memcpy(p(a), &v, 4); }
    void u64(uint32_t a, uint64_t v) { std::memcpy(p(a), &v, 8); }
    void bytes(uint32_t a, const void* d, size_t n) { if (n) std::memcpy(p(a), d, n); }
};

uint32_t write_text(W& w, const std::string& s, uint32_t field, size_t cap) {
    w.bytes(field, s.data(), s.size() < cap ? s.size() : cap);
    return field;
}

// Binary data blob; 0 only when the buffer is full (an empty blob gets 4 bytes).
uint32_t write_blob(W& w, const std::vector<uint8_t>& data) {
    const uint32_t a = w.b.alloc(data.empty() ? 4 : static_cast<uint32_t>(data.size()));
    if (a) w.bytes(a, data.data(), data.size());
    return a;
}

// SceNpUserInfo2 (44): npId {online id data[16], term, dummy[3], opt[8], reserved[8]} @0,
// SceNpOnlineName* @36 (52 bytes: data[48], term, padding[3]), avatarUrl* @40.
uint32_t write_user_info(W& w, const std::string& online_id) {
    const uint32_t u = w.b.alloc(44), name = w.b.alloc(52);
    if (!u || !name) return 0;
    write_text(w, online_id, u, 16);
    write_text(w, online_id, name, 48);
    w.u32(u + 36, name);
    return u;
}

// {u16 id; pad; ptr; u32 size} x n
uint32_t write_bin_attrs(W& w, const nlohmann::json& bins, uint32_t* count) {
    *count = 0;
    if (!bins.is_array() || bins.empty()) return 0;
    const uint32_t n = static_cast<uint32_t>(bins.size());
    const uint32_t a = w.b.alloc(n * 12);
    if (!a) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const std::vector<uint8_t> data = infra::from_hex(bins[i][1].get<std::string>());
        const uint32_t d = write_blob(w, data);
        if (!d) return 0;
        w.u16(a + i * 12, bins[i][0].get<uint16_t>());
        w.u32(a + i * 12 + 4, d);
        w.u32(a + i * 12 + 8, static_cast<uint32_t>(data.size()));
    }
    *count = n;
    return a;
}

void write_opt(W& w, uint32_t at, const std::vector<uint8_t>& opt) {
    const size_t n = opt.size() < 16 ? opt.size() : 16;
    w.bytes(at, opt.data(), n);
    w.u32(at + 16, static_cast<uint32_t>(n));
}

}  // namespace

uint32_t GuestBuf::alloc(uint32_t n) {
    const uint32_t start = (used + 7) & ~7u;
    if (start > size || n > size - start) return 0;
    used = start + n;
    std::memset(base_host + start, 0, n);
    return base_guest + start;
}

uint32_t write_server_info(GuestBuf& b, uint16_t server_id, uint8_t status) {
    W w{b};
    const uint32_t d = b.alloc(4);
    if (!d) return 0;
    w.u16(d, server_id);
    w.u8(d + 2, status);
    return d;
}

uint32_t write_world_list(GuestBuf& b, uint32_t world_id, uint32_t rooms, uint32_t members) {
    W w{b};
    const uint32_t d = b.alloc(8), world = b.alloc(64);
    if (!d || !world) return 0;
    w.u32(world + 4, world_id);
    w.u32(world + 0x14, rooms);
    w.u32(world + 0x18, members);
    w.u32(d, world);
    w.u32(d + 4, 1);
    return d;
}

// RoomDataExternal (0x58): next @0, serverId u16 @4, worldId @8, publicSlotNum u16 @0xC,
// privateSlotNum u16 @0xE, lobbyId u64 @0x10, roomId u64 @0x18, openPublicSlotNum u16 @0x20,
// maxSlot u16 @0x22, member count u32 @0x24 (PSP), passwordSlotMask u64 @0x28, owner* @0x30,
// roomGroup* @0x34, num @0x38, flagAttr @0x3C, searchable int attrs* @0x40, num @0x44,
// searchable bin attrs* @0x48, num @0x4C, room bin attrs external* @0x50, num @0x54
uint32_t write_room_ext(GuestBuf& b, const nlohmann::json& r) {
    W w{b};
    const uint32_t e = b.alloc(0x58);
    if (!e) return 0;
    const uint64_t id = r.value("id", uint64_t{0});
    const uint32_t max_slot = r.value("max_slot", 4u), members = r.value("members", 0u);
    w.u16(e + 4, kServerId);
    w.u32(e + 8, kWorldId);
    w.u16(e + 0xC, static_cast<uint16_t>(max_slot));
    w.u64(e + 0x18, id);
    w.u16(e + 0x20, static_cast<uint16_t>(members < max_slot ? max_slot - members : 0));
    w.u16(e + 0x22, static_cast<uint16_t>(max_slot));
    w.u32(e + 0x24, members);
    const uint32_t owner = write_user_info(w, r.value("owner", ""));
    if (!owner) return 0;
    w.u32(e + 0x30, owner);
    const nlohmann::json ints = r.value("ints", nlohmann::json::array());
    if (!ints.empty()) {
        const uint32_t n = static_cast<uint32_t>(ints.size());
        const uint32_t ia = b.alloc(n * 8);
        if (!ia) return 0;
        for (uint32_t i = 0; i < n; i++) {
            w.u16(ia + i * 8, ints[i][0].get<uint16_t>());
            w.u32(ia + i * 8 + 4, ints[i][1].get<uint32_t>());
        }
        w.u32(e + 0x40, ia);
        w.u32(e + 0x44, n);
    }
    uint32_t nb = 0;
    const nlohmann::json bins = r.value("bins", nlohmann::json::array());
    const uint32_t ba = write_bin_attrs(w, bins, &nb);
    if (!bins.empty() && !ba) return 0;
    w.u32(e + 0x50, ba);
    w.u32(e + 0x54, nb);
    return e;
}

static uint32_t write_ext_list(GuestBuf& b, const nlohmann::json& rooms, uint32_t* first, uint32_t* n) {
    W w{b};
    *first = 0;
    *n = 0;
    uint32_t prev = 0;
    for (const auto& r : rooms) {
        const uint32_t e = write_room_ext(b, r);
        if (!e) return 0;
        if (prev) w.u32(prev, e); else *first = e;
        prev = e;
        (*n)++;
    }
    return 1;
}

uint32_t write_search_response(GuestBuf& b, uint32_t start, uint32_t total, const nlohmann::json& rooms) {
    W w{b};
    const uint32_t d = b.alloc(16);
    uint32_t first, n;
    if (!d || !write_ext_list(b, rooms, &first, &n)) return 0;
    w.u32(d, start);
    w.u32(d + 4, total);
    w.u32(d + 8, n);
    w.u32(d + 12, first);
    return d;
}

uint32_t write_ext_list_response(GuestBuf& b, const nlohmann::json& rooms) {
    W w{b};
    const uint32_t d = b.alloc(8);
    uint32_t first, n;
    if (!d || !write_ext_list(b, rooms, &first, &n)) return 0;
    w.u32(d, first);
    w.u32(d + 4, n);
    return d;
}

// RoomMemberDataInternal (0x50): next @0, SceNpUserInfo2 userInfo @4 (online id @4,
// onlineName* @0x28, avatarUrl* @0x2C), joinDate u64 @0x30, memberId u16 @0x38, teamId @0x3A,
// roomGroup* @0x3C, natType @0x40, flagAttr @0x44, member bin attrs internal* @0x48, num @0x4C.
// Member bin attr internal (0x18): updateDate u64 @0, {u16 id @8, ptr @0xC, size @0x10}.
uint32_t write_member(GuestBuf& b, const nlohmann::json& m) {
    W w{b};
    const uint32_t a = b.alloc(0x50), name = b.alloc(52);
    if (!a || !name) return 0;
    const std::string oid = m.value("online_id", "");
    write_text(w, oid, a + 4, 16);
    write_text(w, oid, name, 48);
    w.u32(a + 0x28, name);
    const uint64_t tick = kUnixEpochTick + m.value("join_ms", uint64_t{0}) * 1000ULL;
    w.u64(a + 0x30, tick);
    w.u16(a + 0x38, m.value("member_id", uint16_t{0}));
    const nlohmann::json bins = m.value("bins", nlohmann::json::array());
    if (!bins.empty()) {
        const uint32_t n = static_cast<uint32_t>(bins.size());
        const uint32_t ba = b.alloc(n * 0x18);
        if (!ba) return 0;
        for (uint32_t i = 0; i < n; i++) {
            const std::vector<uint8_t> data = infra::from_hex(bins[i][1].get<std::string>());
            const uint32_t d = write_blob(w, data);
            if (!d) return 0;
            w.u64(ba + i * 0x18, tick);
            w.u16(ba + i * 0x18 + 8, bins[i][0].get<uint16_t>());
            w.u32(ba + i * 0x18 + 0xC, d);
            w.u32(ba + i * 0x18 + 0x10, static_cast<uint32_t>(data.size()));
        }
        w.u32(a + 0x48, ba);
        w.u32(a + 0x4C, n);
    }
    return a;
}

// RoomDataInternal (0x48): serverId u16 @0, worldId @4, lobbyId u64 @8, roomId u64 @0x10,
// passwordSlotMask u64 @0x18, maxSlot @0x20, members* @0x24, num @0x28, me* @0x2C, owner* @0x30,
// roomGroup* @0x34, num @0x38, flagAttr @0x3C, room bin attrs internal* @0x40, num @0x44.
// Room bin attr internal (0x18): updateDate u64 @0, updateMemberId u16 @8, {u16 id @0xC, ptr @0x10, size @0x14}.
uint32_t write_room_int_response(GuestBuf& b, const nlohmann::json& r) {
    W w{b};
    const uint32_t d = b.alloc(4), room = b.alloc(0x48);
    if (!d || !room) return 0;
    w.u16(room, kServerId);
    w.u32(room + 4, kWorldId);
    w.u64(room + 0x10, r.value("id", uint64_t{0}));
    w.u32(room + 0x20, r.value("max_slot", 4u));
    w.u32(room + 0x3C, r.value("flag_attr", 0u));
    const uint32_t me_id = r.value("me", 0u), owner_id = r.value("owner", 0u);
    uint32_t prev = 0, n = 0;
    for (const auto& m : r.value("members", nlohmann::json::array())) {
        const uint32_t a = write_member(b, m);
        if (!a) return 0;
        if (prev) w.u32(prev, a); else w.u32(room + 0x24, a);
        prev = a;
        n++;
        const uint32_t id = m.value("member_id", 0u);
        if (id == me_id) w.u32(room + 0x2C, a);
        if (id == owner_id) w.u32(room + 0x30, a);
    }
    w.u32(room + 0x28, n);
    const nlohmann::json bins = r.value("bins_internal", nlohmann::json::array());
    if (!bins.empty()) {
        const uint32_t nb = static_cast<uint32_t>(bins.size());
        const uint32_t ba = b.alloc(nb * 0x18);
        if (!ba) return 0;
        for (uint32_t i = 0; i < nb; i++) {
            const std::vector<uint8_t> data = infra::from_hex(bins[i][1].get<std::string>());
            const uint32_t blob = write_blob(w, data);
            if (!blob) return 0;
            w.u16(ba + i * 0x18 + 8, static_cast<uint16_t>(owner_id));
            w.u16(ba + i * 0x18 + 0xC, bins[i][0].get<uint16_t>());
            w.u32(ba + i * 0x18 + 0x10, blob);
            w.u32(ba + i * 0x18 + 0x14, static_cast<uint32_t>(data.size()));
        }
        w.u32(room + 0x40, ba);
        w.u32(room + 0x44, nb);
    }
    w.u32(d, room);
    return d;
}

uint32_t write_member_update(GuestBuf& b, const nlohmann::json& member, uint8_t cause, const std::vector<uint8_t>& opt) {
    W w{b};
    const uint32_t u = b.alloc(0x1C);
    if (!u) return 0;
    const uint32_t m = write_member(b, member);
    if (!m) return 0;
    w.u32(u, m);
    w.u8(u + 4, cause);
    write_opt(w, u + 8, opt);
    return u;
}

uint32_t write_room_update(GuestBuf& b, uint8_t cause, int32_t error, const std::vector<uint8_t>& opt) {
    W w{b};
    const uint32_t u = b.alloc(0x1C);
    if (!u) return 0;
    w.u8(u, cause);
    w.u32(u + 4, static_cast<uint32_t>(error));
    write_opt(w, u + 8, opt);
    return u;
}

}  // namespace np2
