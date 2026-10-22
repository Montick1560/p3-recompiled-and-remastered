#pragma once
// sceNpMatching2 response structs written into guest memory. The PSP
// library takes the PS3 SceNpMatching2* structs (RPCS3 sceNp2.h), little
// endian, 32-bit pointers, lists linked through a leading next pointer; the
// PSP differences are in docs/superpowers/specs/2026-10-22-p3-infrastructure-mode-design.md
// (member count of RoomDataExternal is a u32 at +0x24). Input is the JSON of
// tools/infra_server.py (room_ext, room_int, member). Every writer returns the
// guest address of its top struct, or 0 when the buffer is full. Pure.
#include <cstdint>
#include <vector>

#include "json.hpp"

namespace np2 {

constexpr uint16_t kServerId = 1;
constexpr uint32_t kWorldId = 1;

// A window of guest memory: base_host is the host pointer of base_guest.
struct GuestBuf {
    uint8_t* base_host;
    uint32_t base_guest;
    uint32_t size;
    uint32_t used;
    // n zeroed bytes, 8-aligned; 0 when they do not fit.
    uint32_t alloc(uint32_t n);
};

// GetServerInfo (event 0x0001): {u16 serverId; u8 status}
uint32_t write_server_info(GuestBuf& b, uint16_t server_id, uint8_t status);
// GetWorldInfoList (0x0002): {World* list; u32 n = 1}; World (64 bytes, the
// layout the game accepted in the recon): worldId @4, rooms @0x14, members @0x18
uint32_t write_world_list(GuestBuf& b, uint32_t world_id, uint32_t rooms, uint32_t members);
// One SceNpMatching2RoomDataExternal (0x58).
uint32_t write_room_ext(GuestBuf& b, const nlohmann::json& room_ext);
// SearchRoom (0x0106): {u32 start, total, n; RoomDataExternal* list}
uint32_t write_search_response(GuestBuf& b, uint32_t start, uint32_t total, const nlohmann::json& rooms);
// GetRoomDataExternalList (0x0007 on the PSP): {RoomDataExternal* list; u32 n}
uint32_t write_ext_list_response(GuestBuf& b, const nlohmann::json& rooms);
// One SceNpMatching2RoomMemberDataInternal (0x50).
uint32_t write_member(GuestBuf& b, const nlohmann::json& member);
// CreateJoinRoom / JoinRoom (0x0101 / 0x0102): {RoomDataInternal*}
uint32_t write_room_int_response(GuestBuf& b, const nlohmann::json& room_int);
// Room events 0x1101 / 0x1102: SceNpMatching2RoomMemberUpdateInfo
// {member* @0; u8 cause @4; optData {u8 data[16] @8; u32 len @0x18}}
uint32_t write_member_update(GuestBuf& b, const nlohmann::json& member, uint8_t cause, const std::vector<uint8_t>& opt);
// Room events 0x1103 / 0x1104: SceNpMatching2RoomUpdateInfo
// {u8 cause @0; s32 errorCode @4; optData @8}
uint32_t write_room_update(GuestBuf& b, uint8_t cause, int32_t error, const std::vector<uint8_t>& opt);

}  // namespace np2
