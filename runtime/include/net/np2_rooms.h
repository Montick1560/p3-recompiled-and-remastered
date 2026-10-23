#pragma once
// sceNpMatching2 state of every room the game is in, by room id. On VS the
// game stays in its hideout room while it hosts or joins a VS room, so each
// room keeps its own member id, peers and room event callback. Input is the
// room JSON of tools/infra_server.py (`internal()`: id, me, members[{member_id,
// vip, port}]). Pure; not thread-safe (the HLE holds its mutex).
#include <cstdint>
#include <map>
#include <vector>

#include "json.hpp"

namespace np2 {

struct Peer {
    uint32_t vip = 0;
    uint16_t port = 0;
};

struct RoomCallback {
    uint32_t cb = 0, arg = 0;
};

class Rooms {
public:
    // A create/join answer: replaces any earlier state of that room.
    void enter(const nlohmann::json& room, RoomCallback cb);
    bool has(uint64_t room) const { return rooms_.count(room) != 0; }
    void leave(uint64_t room) { rooms_.erase(room); }
    void clear() { rooms_.clear(); }
    std::vector<uint64_t> ids() const;

    // Events for a room not entered are ignored.
    void member_joined(uint64_t room, uint16_t id, Peer p);
    void member_left(uint64_t room, uint16_t id);
    void signaled(uint64_t room, uint16_t id, Peer p);  // SIGNALING ESTABLISHED: the peer's address

    // false for an unknown room or member, or a member without a vip yet.
    bool peer(uint64_t room, uint16_t id, Peer* out) const;
    RoomCallback callback(uint64_t room) const;  // {0, 0} for an unknown room

private:
    struct Room {
        uint16_t me = 0;
        RoomCallback cb;
        std::map<uint16_t, Peer> peers;  // by member id, me included
    };
    std::map<uint64_t, Room> rooms_;
};

}  // namespace np2
