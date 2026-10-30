#pragma once
// Discord Rich Presence over the local IPC pipe (\\.\pipe\discord-ipc-N-N), no SDK. Framing is pure;
// DiscordIpc does the pipe I/O (Windows only, elsewhere connect() is false). Every read is bounded so
// the owner thread can always stop. The client id is a parameter: no game data here.
#include <cstddef>
#include <cstdint>
#include <string>

#include "json.hpp"

// u32 opcode LE | u32 length LE | UTF-8 JSON.
std::string discord_frame(uint32_t op, const nlohmann::json& body);
// Parses one whole frame from the start of buf; false when incomplete, over 64 KiB or not JSON.
// On success *used = bytes consumed.
bool discord_unframe(const std::string& buf, uint32_t* op, nlohmann::json* body, size_t* used);

class DiscordIpc {
public:
    DiscordIpc() = default;
    DiscordIpc(const DiscordIpc&) = delete;
    DiscordIpc& operator=(const DiscordIpc&) = delete;
    ~DiscordIpc() { close(); }

    // Tries discord-ipc-0..9, handshakes and consumes the READY answer. False leaves it closed.
    bool connect(const std::string& client_id);
    // SET_ACTIVITY with this process id; null clears. False (and closed) on ERROR or a broken pipe.
    bool set_activity(const nlohmann::json& activity_or_null);
    // Cheap liveness probe (no I/O wait): closes and returns false when Discord went away.
    bool alive();
    void close();
    bool connected() const { return pipe_ != nullptr; }

private:
    bool read_frame(uint32_t* op, nlohmann::json* body, int timeout_ms);
    bool write_frame(uint32_t op, const nlohmann::json& body);
    void* pipe_ = nullptr;
    uint32_t nonce_ = 0;
};
