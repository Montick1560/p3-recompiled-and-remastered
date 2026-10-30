// Discord IPC framing (pure).
#include "psp_discord.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void frame_roundtrip() {
    const nlohmann::json body = {{"cmd", "SET_ACTIVITY"}};
    const std::string f = discord_frame(1, body);
    uint32_t op = 0;
    nlohmann::json out;
    size_t used = 0;
    CHECK(discord_unframe(f, &op, &out, &used), "unframe");
    CHECK(op == 1 && out == body, "op and body");
    CHECK(used == f.size() && used == 8 + body.dump().size(), "used == 8 + len");
    CHECK(f[0] == 1 && f[1] == 0 && f[4] == char(body.dump().size()) && f[5] == 0, "little endian header");
    // A second frame behind the first is left alone.
    CHECK(discord_unframe(f + f, &op, &out, &used) && used == f.size(), "only the first frame is consumed");
}

static void unframe_needs_whole_frame() {
    const std::string f = discord_frame(1, {{"cmd", "X"}});
    uint32_t op;
    nlohmann::json out;
    size_t used;
    CHECK(!discord_unframe(f.substr(0, f.size() - 1), &op, &out, &used), "truncated body");
    CHECK(!discord_unframe(f.substr(0, 7), &op, &out, &used), "truncated header");
    CHECK(!discord_unframe("", &op, &out, &used), "empty");
}

static void unframe_rejects_huge_length() {
    uint32_t op;
    nlohmann::json out;
    size_t used;
    // Valid JSON in both, so only the 64 KiB cap can reject the first one (body = string + 2 quotes).
    const std::string over = discord_frame(1, std::string(65535, 'a'));   // body 65537 bytes
    const std::string exact = discord_frame(1, std::string(65534, 'a'));  // body 65536 bytes
    CHECK(!discord_unframe(over, &op, &out, &used), "length over 64 KiB");
    CHECK(discord_unframe(exact, &op, &out, &used) && used == 8 + 65536, "length exactly 64 KiB is accepted");
}

int main() {
    frame_roundtrip();
    unframe_needs_whole_frame();
    unframe_rejects_huge_length();
    if (failures) return 1;
    std::puts("discord_frame: OK");
    return 0;
}
