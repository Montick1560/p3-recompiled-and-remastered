// p3_effects: which mission effect actors are paced to 30 Hz at 60 fps, by the tail of their effect name.
#include "p3_effects.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// The game keeps the last 15 characters of the effect name and an 'x' in a 16-byte field (no NUL when full).
static bool paced(const char* tail) {
    char field[16] = {};
    std::memcpy(field, tail, std::strlen(tail) < 16 ? std::strlen(tail) : 16);
    return p3_effect_paced_name(field);
}

int main() {
    // Pyokojinete's hero ghost riders: a fixed step per call (2x speed unpaced)
    CHECK(paced("RO_ICERING_ED2x"), "ghost riders (moves the dash part)");
    CHECK(paced("HERO_PHANTOM_0x"), "phantoms born with the ghost riders");
    CHECK(paced("HERO_PHANTOM_3x"), "phantom trail");
    // Wooyari's hero wheel and what is born in its 30-vblank cycle (1-frame gap at its restart unpaced)
    CHECK(paced("_HERO_RING_ST2x"), "wheel start");
    CHECK(paced("_HERO_RING_LP2x"), "wheel loop");
    CHECK(paced("F_HERO_RING_EDx"), "wheel end");
    CHECK(paced("GSTONE_LONG_STx"), "ring stone");
    CHECK(paced("NGSTONE_ICE_EDx"), "ring stone end");
    CHECK(paced("DIRECT_1_002_1x"), "wheel-cycle direct effect");
    // The cyclops' thrown rock (Caza al ciclope II): its flight brakes per call, so unpaced it fell 184 units short
    // of the 270 it flies at 30 fps (same 2.5 s in the air)
    CHECK(paced("EFF_EMDL_060x"), "cyclops rock");
    CHECK(!paced("EFF_EMDL_061x"), "another effect model");
    CHECK(!paced("EFF_EMDL_006x"), "another effect model (prefix)");
    // Projectiles and the rest move by the frame time: smooth and right unpaced
    CHECK(!paced("APON_LONGARROWx"), "long arrow");
    CHECK(!paced("_WEAPON_BOWGUNx"), "bowgun");
    CHECK(!paced("HIT_ARROW_WOODx"), "arrow hit");
    CHECK(!paced("_JAVELIN_BLASTx"), "javelin");
    CHECK(!paced("_HERO_SPEAR_01x"), "hero spear");
    CHECK(!paced("DIRECT_1_002_0x"), "other direct effect");
    CHECK(!paced("TPOWERUP_STARTx"), "power-up burst");
    CHECK(!paced("YA_KAGE_ATTACKx"), "shadow attack");
    CHECK(!paced(""), "no name");
    char full[16];
    std::memset(full, 'A', 16);  // no NUL inside the field: never read past it
    CHECK(!p3_effect_paced_name(full), "full field without NUL");
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("p3_effects: all passed\n");
    return 0;
}
