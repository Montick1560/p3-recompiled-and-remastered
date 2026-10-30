#pragma once
// Discord Rich Presence in a running game. The game module registers where to read the facts and its
// Discord application id; main() starts the runner after the module's hooks and stops it at shutdown.
// The runner thread wakes every second, builds the activity from guest RAM, and sends it when it
// changed and at least 15 s passed since the last send. A missing or closed Discord is retried every
// 30 s, silently. Off with PSPRECOMP_DISCORD=0 or without a registered client id.
#include <cstdint>
#include <string>

#include "json.hpp"

struct PspPresenceSource {
    const char* client_id;  // Discord application id; null or empty keeps the module off
    // Activity object for the current guest state, or null when there is nothing to show yet.
    nlohmann::json (*activity)(const uint8_t* rdram, const char* lang, int64_t start_unix);
};

void psp_presence_register(const PspPresenceSource&);
// lang: "en" or "es". Returns false (one [DISCORD] line) when presence stays off.
bool psp_presence_start(const uint8_t* rdram, const std::string& lang);
// Clears the activity, closes the pipe and joins the thread (bounded by the IPC timeouts). Safe when not started.
void psp_presence_stop();
// Discord allows one update per 15 s: send only a changed activity, and only after kPresenceMinSendGapMs.
constexpr int64_t kPresenceMinSendGapMs = 15000;
bool psp_presence_should_send(const nlohmann::json& last, const nlohmann::json& now, int64_t ms_since_last);
