// Patapon 3 Discord activity text (pure).
#include "p3_presence_text.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void activity_hideout_es() {
    const auto a = p3_presence_activity({P3Place::Hideout, 13, 22, false, u""}, "es", 1700000000);
    CHECK(a["details"] == "Wooyari \xC2\xB7 Nivel 22", "details");
    CHECK(a["state"] == "En el escondite", "state");
    CHECK(a["assets"]["small_image"] == "class_13" && a["assets"]["large_image"] == "logo", "assets");
    CHECK(a["timestamps"]["start"] == 1700000000, "start");
}

static void activity_mission_en() {
    const auto a = p3_presence_activity({P3Place::Mission, 3, 10, false, u"Erase the gargoyle's smile"}, "en", 5);
    CHECK(a["state"] == "Mission: Erase the gargoyle's smile", "state");
    CHECK(a["details"] == "Taterazay \xC2\xB7 Level 10", "details");
    const auto e = p3_presence_activity({P3Place::Mission, 3, 10, false, u"Borra la sonrisa de la g\u00e1rgola"}, "es", 5);
    CHECK(e["state"] == "Misi\xC3\xB3n: Borra la sonrisa de la g\xC3\xA1rgola", "UTF-8 mission name");
}

static void activity_versus_hides_class() {
    const auto a = p3_presence_activity({P3Place::Hideout, 13, 22, true, u"[VS] x"}, "es", 5);
    CHECK(a["details"] == "En enfrentamiento" && a["assets"]["small_image"] == "vs", "versus");
    const std::string s = a.dump();
    CHECK(s.find("Wooyari") == std::string::npos && s.find("class_") == std::string::npos &&
          s.find("22") == std::string::npos && !a.contains("state"), "no class or level");
}

static void activity_title() {
    const auto a = p3_presence_activity({P3Place::Title, 0, 0, false, u""}, "es", 5);
    CHECK(a["details"] == "En la pantalla de t\xC3\xADtulo" && !a["assets"].contains("small_image"), "title");
    CHECK(p3_presence_activity({P3Place::Unknown, 0, 0, false, u""}, "es", 5).is_null(), "unknown place sends nothing");
}

int main() {
    activity_hideout_es();
    activity_mission_en();
    activity_versus_hides_class();
    activity_title();
    if (failures) return 1;
    std::puts("p3_presence_text: OK");
    return 0;
}
