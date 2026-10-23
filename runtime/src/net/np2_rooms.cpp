#include "net/np2_rooms.h"

namespace np2 {

void Rooms::enter(const nlohmann::json& room, RoomCallback cb) {
    Room& r = rooms_[room.value("id", uint64_t{0})];
    r = Room{};
    r.me = room.value("me", uint16_t{0});
    r.cb = cb;
    for (const auto& m : room.value("members", nlohmann::json::array()))
        r.peers[m.value("member_id", uint16_t{0})] = Peer{m.value("vip", 0u), m.value("port", uint16_t{0})};
}

std::vector<uint64_t> Rooms::ids() const {
    std::vector<uint64_t> out;
    for (const auto& [id, r] : rooms_) out.push_back(id);
    return out;
}

void Rooms::member_joined(uint64_t room, uint16_t id, Peer p) {
    auto it = rooms_.find(room);
    if (it != rooms_.end()) it->second.peers[id] = p;
}

void Rooms::member_left(uint64_t room, uint16_t id) {
    auto it = rooms_.find(room);
    if (it != rooms_.end()) it->second.peers.erase(id);
}

void Rooms::signaled(uint64_t room, uint16_t id, Peer p) { member_joined(room, id, p); }

bool Rooms::peer(uint64_t room, uint16_t id, Peer* out) const {
    auto it = rooms_.find(room);
    if (it == rooms_.end()) return false;
    auto m = it->second.peers.find(id);
    if (m == it->second.peers.end() || m->second.vip == 0) return false;
    *out = m->second;
    return true;
}

RoomCallback Rooms::callback(uint64_t room) const {
    auto it = rooms_.find(room);
    return it == rooms_.end() ? RoomCallback{} : it->second.cb;
}

}  // namespace np2
