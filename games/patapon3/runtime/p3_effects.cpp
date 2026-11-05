#include "p3_effects.h"

#include <cstring>
#include <string_view>

bool p3_effect_paced_name(const char* field) {
    const std::string_view name(field, strnlen(field, 16));
    // Ghost riders (HERO_ICERING_ED2 moves the dash; the HERO_PHANTOM_0..3 are born with it) step a fixed amount
    // per call: 1197 units in 1.68 s unpaced against 584 in 1.71 s at 30 fps. The wheel (HERO_RING_ ST/LP/ED, its
    // ring stones, and DIRECT_1_002_1 born in the same 30-vblank cycle) is not drawn for one vblank at each restart
    // unpaced. Everything else (arrows, javelins, spears, hits) moves by the frame time and stays smooth unpaced.
    for (std::string_view part : {"ICERING_ED", "HERO_PHANTOM_", "HERO_RING_", "GSTONE_"})
        if (name.find(part) != std::string_view::npos) return true;
    return name == "DIRECT_1_002_1x";
}
