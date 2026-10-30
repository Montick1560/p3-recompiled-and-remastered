#pragma once
// Patapon 3 progress struct for achievements (docs/p3_achievement_facts.md): the pointer chain per
// release, the plausibility check and the named facts. Pure: no runtime state.
#include "psp_achievements.h"

#include <cstddef>
#include <cstdint>

const PspAchievementSource* p3_achievement_sources(size_t* n);

// The hero save struct in guest RAM (0 when the chain leaves user RAM) and its plausibility check,
// shared with the Discord presence reader.
uint32_t p3_save_base(const uint8_t* rdram);
bool p3_save_sane(const uint8_t* rdram, uint32_t base);
