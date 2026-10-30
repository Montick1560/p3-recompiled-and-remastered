#include "psp_discord.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr uint32_t kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4;
constexpr size_t kMaxBody = 64 * 1024;
constexpr int kReplyMs = 2000;

void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s += static_cast<char>(v >> (8 * i));
}
uint32_t get32(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[at + i])) << (8 * i);
    return v;
}

// Discord answers commands with "evt":null, which json::value() would throw on.
std::string evtOf(const nlohmann::json& j) {
    const auto it = j.find("evt");
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
}  // namespace

std::string discord_frame(uint32_t op, const nlohmann::json& body) {
    // replace: guest RAM text may hold invalid UTF-8, which the strict default would throw on.
    const std::string j = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::string f;
    put32(f, op);
    put32(f, static_cast<uint32_t>(j.size()));
    return f + j;
}

bool discord_unframe(const std::string& buf, uint32_t* op, nlohmann::json* body, size_t* used) {
    if (buf.size() < 8) return false;
    const uint32_t len = get32(buf, 4);
    if (len > kMaxBody || buf.size() - 8 < len) return false;
    nlohmann::json j = nlohmann::json::parse(buf.begin() + 8, buf.begin() + 8 + len, nullptr, false);
    if (j.is_discarded()) return false;
    *op = get32(buf, 0);
    *body = std::move(j);
    *used = 8 + len;
    return true;
}

#ifdef _WIN32

bool DiscordIpc::write_frame(uint32_t op, const nlohmann::json& body) {
    const std::string f = discord_frame(op, body);
    DWORD n = 0;
    return pipe_ && WriteFile(pipe_, f.data(), static_cast<DWORD>(f.size()), &n, nullptr) && n == f.size();
}

// Polls the pipe (PeekNamedPipe) with one overall deadline, so neither silence nor a flood of data
// can hold the caller past timeout_ms. Answers PING itself.
bool DiscordIpc::read_frame(uint32_t* op, nlohmann::json* body, int timeout_ms) {
    std::string buf;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        size_t used = 0;
        if (discord_unframe(buf, op, body, &used)) {
            buf.erase(0, used);
            if (*op == kPing) { write_frame(kPong, *body); continue; }
            return *op != kClose;
        }
        // Over the cap, or complete but not JSON: it will never become a frame.
        if (buf.size() >= 8 && (get32(buf, 4) > kMaxBody || buf.size() - 8 >= get32(buf, 4))) return false;
        if (GetTickCount64() >= deadline) return false;
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return false;  // broken pipe
        if (avail == 0) {
            Sleep(10);
            continue;
        }
        char tmp[4096];
        DWORD got = 0;
        if (!ReadFile(pipe_, tmp, avail < sizeof tmp ? avail : sizeof tmp, &got, nullptr) || !got) return false;
        buf.append(tmp, got);
    }
}

bool DiscordIpc::connect(const std::string& client_id) {
    close();
    for (int i = 0; i < 10; i++) {
        const std::wstring name = LR"(\\.\pipe\discord-ipc-)" + std::to_wstring(i);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        pipe_ = h;
        uint32_t op = 0;
        nlohmann::json ready;
        if (write_frame(kHandshake, {{"v", 1}, {"client_id", client_id}}) && read_frame(&op, &ready, kReplyMs) &&
            op == kFrame && evtOf(ready) == "READY")
            return true;
        close();
        return false;  // a pipe that answers wrongly is not worth trying its siblings
    }
    return false;
}

bool DiscordIpc::set_activity(const nlohmann::json& activity_or_null, int reply_timeout_ms) {
    if (!pipe_) return false;
    const nlohmann::json cmd = {{"cmd", "SET_ACTIVITY"},
                                {"args", {{"pid", static_cast<uint32_t>(GetCurrentProcessId())}, {"activity", activity_or_null}}},
                                {"nonce", std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++nonce_)}};
    uint32_t op = 0;
    nlohmann::json reply;
    if (write_frame(kFrame, cmd) && read_frame(&op, &reply, reply_timeout_ms) && op == kFrame &&
        evtOf(reply) != "ERROR")
        return true;
    close();
    return false;
}

bool DiscordIpc::alive() {
    DWORD avail = 0;
    if (pipe_ && !PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) close();
    return pipe_ != nullptr;
}

void DiscordIpc::close() {
    if (pipe_) CloseHandle(pipe_);
    pipe_ = nullptr;
}

#else  // no Discord pipe off Windows (Steam Deck / Proton bridging is out of scope)

bool DiscordIpc::write_frame(uint32_t, const nlohmann::json&) { return false; }
bool DiscordIpc::read_frame(uint32_t*, nlohmann::json*, int) { return false; }
bool DiscordIpc::connect(const std::string&) { return false; }
bool DiscordIpc::set_activity(const nlohmann::json&, int) { return false; }
bool DiscordIpc::alive() { return false; }
void DiscordIpc::close() { pipe_ = nullptr; }

#endif
