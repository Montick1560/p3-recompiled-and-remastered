// np2_rooms: the Matching2 state of every room the game is in at once (its
// hideout room and a VS room), keyed by room id.
#include "net/np2_rooms.h"

#include <algorithm>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static constexpr uint64_t kH = 0x1000000773ULL, kV = 0x1000000774ULL;

static nlohmann::json room(uint64_t id, uint16_t me, std::vector<std::pair<uint16_t, uint32_t>> members) {
    nlohmann::json r = {{"id", id}, {"me", me}, {"members", nlohmann::json::array()}};
    for (auto [mid, vip] : members) r["members"].push_back({{"member_id", mid}, {"vip", vip}, {"port", 3658}});
    return r;
}

static void enter_two_rooms(np2::Rooms& rs) {
    rs.enter(room(kH, 0x11, {{0x10, 0x0A500001}, {0x11, 0x0A500002}}), np2::RoomCallback{0x08800100, 1});
    rs.enter(room(kV, 0x10, {{0x10, 0x0A500002}}), np2::RoomCallback{0x08800200, 2});
}

static void test_enter_two_rooms() {
    np2::Rooms rs;
    enter_two_rooms(rs);
    std::vector<uint64_t> ids = rs.ids();
    std::sort(ids.begin(), ids.end());
    CHECK(ids == (std::vector<uint64_t>{kH, kV}), "both rooms");
    CHECK(rs.has(kH) && rs.has(kV), "has both");
    CHECK(rs.callback(kV).cb == 0x08800200 && rs.callback(kV).arg == 2, "V's own callback");
    CHECK(rs.callback(kH).cb == 0x08800100 && rs.callback(kH).arg == 1, "H's own callback");
    np2::Peer p;
    CHECK(rs.peer(kH, 0x10, &p) && p.vip == 0x0A500001 && p.port == 3658, "peer 0x10 of H has H's vip");
    CHECK(rs.peer(kV, 0x10, &p) && p.vip == 0x0A500002, "peer 0x10 of V has V's vip");
}

static void leave_keeps_other_rooms() {
    np2::Rooms rs;
    enter_two_rooms(rs);
    rs.leave(kH);
    CHECK(!rs.has(kH) && rs.has(kV), "only H left");
    np2::Peer p;
    CHECK(!rs.peer(kH, 0x10, &p), "H's peers gone");
    CHECK(rs.peer(kV, 0x10, &p) && p.vip == 0x0A500002, "V's peers kept");
    CHECK(rs.callback(kV).cb == 0x08800200, "V's callback kept");
    rs.clear();
    CHECK(rs.ids().empty(), "clear drops every room");
}

static void member_events_per_room() {
    np2::Rooms rs;
    enter_two_rooms(rs);
    rs.member_joined(kV, 0x11, np2::Peer{0x0A500005, 3659});
    np2::Peer p;
    CHECK(rs.peer(kV, 0x11, &p) && p.vip == 0x0A500005, "joined V");
    CHECK(rs.peer(kH, 0x11, &p) && p.vip == 0x0A500002, "H's 0x11 untouched");
    rs.member_left(kV, 0x11);
    CHECK(!rs.peer(kV, 0x11, &p), "left V");
    CHECK(rs.peer(kH, 0x11, &p), "still in H");
    rs.signaled(kV, 0x12, np2::Peer{0x0A500006, 3660});
    CHECK(rs.peer(kV, 0x12, &p) && p.port == 3660, "ESTABLISHED records the peer");
    rs.member_joined(kH, 0x13, np2::Peer{0, 0});
    CHECK(!rs.peer(kH, 0x13, &p), "a member without a vip is not a peer yet");
}

static void unknown_room_is_empty() {
    np2::Rooms rs;
    enter_two_rooms(rs);
    np2::Peer p;
    CHECK(!rs.peer(999, 0x10, &p), "unknown room: no peer");
    CHECK(rs.callback(999).cb == 0 && rs.callback(999).arg == 0, "unknown room: no callback");
    rs.member_joined(999, 0x10, np2::Peer{1, 1});
    CHECK(!rs.has(999), "an event for an unknown room does not create it");
    rs.leave(999);
    CHECK(rs.ids().size() == 2, "leaving an unknown room changes nothing");
}

int main() {
    test_enter_two_rooms();
    leave_keeps_other_rooms();
    member_events_per_room();
    unknown_room_is_empty();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("np2_rooms: all tests passed\n");
    return 0;
}
