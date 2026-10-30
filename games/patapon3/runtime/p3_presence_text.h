#pragma once
// Discord activity text for Patapon 3 (Spanish / English), built from the presence facts.
#include "p3_presence.h"

#include "json.hpp"

// Activity object for Discord, or null while the place is unknown. Never contains the hero name.
nlohmann::json p3_presence_activity(const P3Presence&, const char* lang, int64_t start_unix);
// The same from guest RAM (what the runner registers).
nlohmann::json p3_presence_activity_from_ram(const uint8_t* rdram, const char* lang, int64_t start_unix);
