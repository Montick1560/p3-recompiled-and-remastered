#pragma once
// Patapon 3 progress struct for achievements (docs/p3_achievement_facts.md): the pointer chain per
// release, the plausibility check and the named facts. Pure: no runtime state.
#include "psp_achievements.h"

#include <cstddef>

const PspAchievementSource* p3_achievement_sources(size_t* n);
