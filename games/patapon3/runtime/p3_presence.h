#pragma once
// Patapon 3 Discord presence facts read from guest RAM: where the player is, hero class and level,
// mission name. Pure: no runtime state, never throws, every read bounded.
#include <cstdint>
#include <string>

enum class P3Place : uint8_t { Unknown, Title, Hideout, Mission };

struct P3Presence {
    P3Place place;
    uint8_t class_id;  // 1..34, 0 = unknown (no sane save yet)
    uint8_t level;     // hero level, 0 = unknown
    bool versus;       // hideout whose mission name says it is a VS lobby
    std::u16string mission;
    // Own multiplayer status (multi_setTweet) while online: 1 ready, 2 event, 3 tips, 4 chests, 5 results,
    // 6 loading; 0 = none (offline, playing, unknown).
    uint8_t activity = 0;
};

bool operator==(const P3Presence& a, const P3Presence& b);
P3Presence p3_presence_read(const uint8_t* rdram);
