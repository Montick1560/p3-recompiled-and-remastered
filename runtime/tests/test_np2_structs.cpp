// np2_structs: the sceNpMatching2 response structs (PS3 layout, little
// endian, PSP differences from the spec) written into a guest buffer.
#include "net/np2_structs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static constexpr uint32_t kBase = 0x09000000;

struct Mem {
    std::vector<uint8_t> host = std::vector<uint8_t>(0x4000, 0xCD);
    np2::GuestBuf buf{host.data(), kBase, static_cast<uint32_t>(host.size()), 0};
    uint8_t* at(uint32_t guest) { return host.data() + (guest - kBase); }
    uint32_t u32(uint32_t guest) {
        uint32_t v;
        std::memcpy(&v, at(guest), 4);
        return v;
    }
    uint16_t u16(uint32_t guest) {
        uint16_t v;
        std::memcpy(&v, at(guest), 2);
        return v;
    }
    uint8_t u8(uint32_t guest) { return *at(guest); }
    std::string str(uint32_t guest) { return reinterpret_cast<const char*>(at(guest)); }
    bool inside(uint32_t guest) { return guest >= kBase && guest < kBase + host.size(); }
};

static nlohmann::json room_ext_json() {
    return {{"id", 0x0001000000000007LL}, {"owner", "P3A"}, {"max_slot", 4}, {"members", 2},
            {"ints", {{0x4C, 0}, {0x4D, 0x30005}}},
            {"bins", {{0x55, "4500640073006f006e00"}, {0x56, "503341"}}}};
}

static void room_ext_offsets() {
    Mem m;
    const uint32_t e = np2::write_room_ext(m.buf, room_ext_json());
    CHECK(e != 0 && e % 4 == 0 && m.inside(e), "entry allocated");
    CHECK(m.u32(e) == 0, "next = null");
    CHECK(m.u16(e + 4) == np2::kServerId && m.u32(e + 8) == np2::kWorldId, "server and world ids");
    CHECK(m.u16(e + 0xC) == 4, "publicSlotNum");
    CHECK(m.u32(e + 0x18) == 7 && m.u32(e + 0x1C) == 0x00010000, "roomId u64 @0x18");
    CHECK(m.u16(e + 0x20) == 2, "openPublicSlotNum = max - members");
    CHECK(m.u16(e + 0x22) == 4, "maxSlot u16 @0x22");
    CHECK(m.u32(e + 0x24) == 2, "member count u32 @0x24 (PSP)");
    const uint32_t owner = m.u32(e + 0x30);
    CHECK(m.inside(owner) && m.str(owner) == "P3A", "owner SceNpUserInfo2: online id @+0");
    CHECK(m.inside(m.u32(owner + 36)) && m.str(m.u32(owner + 36)) == "P3A", "owner online name pointer @+36");
    const uint32_t ia = m.u32(e + 0x40);
    CHECK(m.u32(e + 0x44) == 2 && m.inside(ia), "int attrs @0x40/0x44");
    CHECK(m.u16(ia) == 0x4C && m.u32(ia + 4) == 0 && m.u16(ia + 8) == 0x4D && m.u32(ia + 12) == 0x30005,
          "int attr {u16 id, u32 value} x 8 bytes");
    const uint32_t ba = m.u32(e + 0x50);
    CHECK(m.u32(e + 0x54) == 2 && m.inside(ba), "bin attrs @0x50/0x54");
    CHECK(m.u16(ba) == 0x55 && m.u32(ba + 8) == 10 && m.inside(m.u32(ba + 4)) && m.u8(m.u32(ba + 4)) == 'E',
          "bin attr {u16 id, ptr, size}");
    CHECK(m.u16(ba + 12) == 0x56 && m.u32(ba + 20) == 3 && m.str(m.u32(ba + 16)).substr(0, 3) == "P3A",
          "second bin attr, 12 bytes on");
}

static void room_int_offsets() {
    Mem m;
    const nlohmann::json room = {
        {"id", 9}, {"max_slot", 4}, {"flag_attr", 0x04000000}, {"me", 0x11}, {"owner", 0x10},
        {"members",
         {{{"member_id", 0x10}, {"online_id", "P3A"}, {"vip", 0x0A500001}, {"port", 3658}, {"join_ms", 1000},
           {"bins", nlohmann::json::array()}},
          {{"member_id", 0x11}, {"online_id", "P3B"}, {"vip", 0x0A500002}, {"port", 3659}, {"join_ms", 2000},
           {"bins", {{0x57, "00ff"}}}}}},
        {"bins_internal", {{0x58, "aabbcc"}}}};
    const uint32_t d = np2::write_room_int_response(m.buf, room);
    CHECK(d != 0, "response allocated");
    const uint32_t r = m.u32(d);
    CHECK(m.inside(r), "{RoomDataInternal*}");
    CHECK(m.u16(r) == np2::kServerId && m.u32(r + 4) == np2::kWorldId, "server/world");
    CHECK(m.u32(r + 0x10) == 9 && m.u32(r + 0x14) == 0, "roomId @0x10");
    CHECK(m.u32(r + 0x20) == 4, "maxSlot @0x20");
    CHECK(m.u32(r + 0x28) == 2, "member num @0x28");
    CHECK(m.u32(r + 0x3C) == 0x04000000, "flagAttr @0x3C");
    const uint32_t first = m.u32(r + 0x24);
    const uint32_t second = m.u32(first);
    CHECK(m.inside(first) && m.inside(second) && m.u32(second) == 0, "members linked through +0");
    CHECK(m.str(first + 4) == "P3A" && m.u16(first + 0x38) == 0x10, "member: online id @4, memberId @0x38");
    CHECK(m.str(m.u32(first + 0x28)) == "P3A", "member: online name ptr @0x28");
    CHECK(m.str(second + 4) == "P3B" && m.u16(second + 0x38) == 0x11, "second member");
    CHECK(m.u32(r + 0x2C) == second, "me @0x2C");
    CHECK(m.u32(r + 0x30) == first, "owner @0x30");
    const uint64_t tick = static_cast<uint64_t>(m.u32(second + 0x30)) | (static_cast<uint64_t>(m.u32(second + 0x34)) << 32);
    CHECK(tick == 62135596800000000ULL + 2000ULL * 1000ULL, "joinDate CellRtcTick @0x30");
    const uint32_t mb = m.u32(second + 0x48);
    CHECK(m.u32(second + 0x4C) == 1 && m.inside(mb) && m.u16(mb + 8) == 0x57 && m.u32(mb + 0x10) == 2 &&
              m.u8(m.u32(mb + 0xC) + 1) == 0xFF, "member bin attr internal {tick, {id, ptr, size}}");
    const uint32_t rb = m.u32(r + 0x40);
    CHECK(m.u32(r + 0x44) == 1 && m.inside(rb) && m.u16(rb + 0xC) == 0x58 && m.u32(rb + 0x14) == 3 &&
              m.u8(m.u32(rb + 0x10)) == 0xAA, "room bin attr internal {tick, memberId, {id, ptr, size}}");
}

static void search_response_layout() {
    Mem m;
    nlohmann::json rooms = nlohmann::json::array({room_ext_json(), room_ext_json()});
    rooms[1]["id"] = 8;
    const uint32_t d = np2::write_search_response(m.buf, 1, 5, rooms);
    CHECK(d != 0 && m.u32(d) == 1 && m.u32(d + 4) == 5 && m.u32(d + 8) == 2, "{start, total, n}");
    const uint32_t a = m.u32(d + 12), b = m.u32(a);
    CHECK(m.inside(a) && m.inside(b) && m.u32(b) == 0, "list linked through next");
    CHECK(m.u32(a + 0x18) == 7 && m.u32(b + 0x18) == 8, "rooms in order");
    const uint32_t e = np2::write_ext_list_response(m.buf, rooms);
    CHECK(e != 0 && m.u32(e + 4) == 2 && m.u32(m.u32(e) + 0x18) == 7, "ext list {list, n}");
    const uint32_t empty = np2::write_search_response(m.buf, 1, 0, nlohmann::json::array());
    CHECK(empty != 0 && m.u32(empty + 8) == 0 && m.u32(empty + 12) == 0, "empty search: null list");
}

static void member_update_layout() {
    Mem m;
    const nlohmann::json member = {{"member_id", 0x11}, {"online_id", "P3B"}, {"vip", 0x0A500002}, {"port", 3659},
                                   {"join_ms", 0}, {"bins", nlohmann::json::array()}};
    std::vector<uint8_t> opt(16, 0);
    opt[15] = 2;
    const uint32_t u = np2::write_member_update(m.buf, member, 1, opt);
    CHECK(u != 0 && m.inside(m.u32(u)) && m.u16(m.u32(u) + 0x38) == 0x11, "member @0");
    CHECK(m.u8(u + 4) == 1, "cause u8 @4");
    CHECK(m.u8(u + 8 + 15) == 2 && m.u32(u + 0x18) == 16, "opt[16] @8, len @0x18");
    const uint32_t r = np2::write_room_update(m.buf, 2, 0, opt);
    CHECK(r != 0 && m.u8(r) == 2 && m.u32(r + 4) == 0 && m.u8(r + 8 + 15) == 2 && m.u32(r + 0x18) == 16,
          "room update {cause, errorCode, opt}");
}

static void small_structs() {
    Mem m;
    const uint32_t s = np2::write_server_info(m.buf, 1, 1);
    CHECK(s != 0 && m.u16(s) == 1 && m.u8(s + 2) == 1, "server info {u16 id, u8 status}");
    const uint32_t w = np2::write_world_list(m.buf, 1, 3, 5);
    const uint32_t world = m.u32(w);
    CHECK(w != 0 && m.u32(w + 4) == 1 && m.inside(world), "world list {World*, 1}");
    CHECK(m.u32(world + 4) == 1 && m.u32(world + 0x14) == 3 && m.u32(world + 0x18) == 5, "world (recon layout)");
}

static void buffer_full_returns_zero() {
    std::vector<uint8_t> host(0x60, 0);
    np2::GuestBuf buf{host.data(), kBase, static_cast<uint32_t>(host.size()), 0};
    CHECK(buf.alloc(0x40) == kBase, "first alloc at the base");
    CHECK(buf.alloc(0x40) == 0, "no room left -> 0");
    np2::GuestBuf small{host.data(), kBase, static_cast<uint32_t>(host.size()), 0};
    CHECK(np2::write_room_ext(small, room_ext_json()) == 0, "a struct that does not fit -> 0");
    std::vector<uint8_t> big(0x200, 0xCD);
    np2::GuestBuf b2{big.data(), kBase, 0x200, 0};
    const uint32_t a = b2.alloc(3), c = b2.alloc(8);
    CHECK(a == kBase && c % 8 == 0 && big[0] == 0 && big[(c - kBase)] == 0, "zeroed and aligned");
}

int main() {
    room_ext_offsets();
    room_int_offsets();
    search_response_layout();
    member_update_layout();
    small_structs();
    buffer_full_returns_zero();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("np2_structs: all tests passed\n");
    return 0;
}
