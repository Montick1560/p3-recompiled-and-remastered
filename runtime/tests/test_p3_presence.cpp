// Patapon 3 presence facts from guest RAM (synthetic 128 MB buffer, little endian).
#include "p3_presence.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

namespace {
constexpr uint32_t kGlobal = 0x08AABD94u, kOverlay = 0x08ABB1A0u;
constexpr uint32_t kGlobalObj = 0x08B00000u;  // rd32(kGlobal): holds +0x50 -> save and +0x78 -> mission holder
constexpr uint32_t kSave = 0x09000000u, kHolder = 0x08C00000u;

struct Ram {
    std::vector<uint8_t> m = std::vector<uint8_t>(0x08000000);
    void w8(uint32_t a, uint8_t v) { m[a & 0x07FFFFFFu] = v; }
    void w32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) w8(a + i, uint8_t(v >> (8 * i))); }
    void str(uint32_t a, const char* s) { do w8(a++, uint8_t(*s)); while (*s++); }
    void u16(uint32_t a, const char16_t* s) {
        do { w8(a, uint8_t(*s)); w8(a + 1, uint8_t(*s >> 8)); a += 2; } while (*s++);
    }
    void overlay(const char* n) { str(kOverlay, n); }
    void hero(uint32_t cls, uint32_t level) {
        w32(kGlobal, kGlobalObj);
        w32(kGlobalObj + 0x50, kSave);
        w32(kSave + 0x9520, cls);
        w32(kSave + 0xF294, level);
        u16(kSave + 0xF1C8, u"Hero");
    }
    void mission(const char16_t* name) {
        w32(kGlobal, kGlobalObj);
        w32(kGlobalObj + 0x78, kHolder);
        u16(kHolder + 0x9FC + 0x100, name);
    }
};
}  // namespace

static void title_screen_without_save() {
    Ram r;
    r.overlay("OL_Title.bin");
    CHECK(p3_presence_read(r.m.data()) == (P3Presence{P3Place::Title, 0, 0, false, u""}), "title");
}

static void hideout_with_hero() {
    Ram r;
    r.overlay("OL_Azito.bin");
    r.hero(13, 22);
    CHECK(p3_presence_read(r.m.data()) == (P3Presence{P3Place::Hideout, 13, 22, false, u""}), "hideout");
    r.hero(2, 0);  // blank default struct before the save loads
    CHECK(p3_presence_read(r.m.data()) == (P3Presence{P3Place::Hideout, 0, 0, false, u""}), "blank save");
}

static void mission_name() {
    Ram r;
    r.overlay("OL_Mission.bin");
    r.mission(u"Borra la sonrisa de la g\u00e1rgola");
    const P3Presence p = p3_presence_read(r.m.data());
    CHECK(p.place == P3Place::Mission && p.mission == u"Borra la sonrisa de la g\u00e1rgola" && !p.versus, "mission");
}

static void versus_hides_nothing_but_flags() {
    Ram r;
    r.overlay("OL_Azito.bin");
    r.hero(13, 22);
    r.mission(u"[VS] Enf. m\u00faltiple");
    const P3Presence p = p3_presence_read(r.m.data());
    CHECK(p.versus && p.place == P3Place::Hideout && p.class_id == 13 && p.level == 22, "versus flags, keeps class");
    r.overlay("OL_Mission.bin");  // the same name in a real mission is not a lobby
    CHECK(!p3_presence_read(r.m.data()).versus, "versus only in the hideout");
}

static void bad_pointers_are_empty() {
    const P3Presence empty{P3Place::Mission, 0, 0, false, u""};
    Ram r;
    r.overlay("OL_Mission.bin");
    r.hero(99, 22);
    r.mission(u"x");
    r.w32(kGlobalObj + 0x78, 0x12345678u);  // masks to 0x02345678: a name placed there must not be read
    r.u16((0x12345678u & 0x07FFFFFFu) + 0x9FC + 0x100, u"stray");
    CHECK(p3_presence_read(r.m.data()) == empty, "holder pointer outside user RAM");

    Ram o;  // the game object pointer itself outside user RAM, with a complete chain behind it
    o.overlay("OL_Mission.bin");
    o.w32(kGlobal, 0x12345678u);
    o.w32(0x12345678u + 0x78, kHolder);
    o.u16(kHolder + 0x9FC + 0x100, u"stray");
    CHECK(p3_presence_read(o.m.data()) == empty, "object pointer outside user RAM");
}

static void unterminated_name_is_empty() {
    Ram r;
    r.overlay("OL_Mission.bin");
    r.mission(std::u16string(70, u'a').c_str());
    CHECK(p3_presence_read(r.m.data()).mission.empty(), "70 units without terminator");
    r.mission(std::u16string(63, u'a').c_str());
    CHECK(p3_presence_read(r.m.data()).mission.size() == 63, "63 units fit");
}

static void ordinary_battle_is_not_versus() {
    Ram r;
    r.overlay("OL_Azito.bin");
    r.mission(u"Batalla contra el dragón");
    CHECK(!p3_presence_read(r.m.data()).versus, "Batalla contra ... is a normal mission");
}

int main() {
    title_screen_without_save();
    hideout_with_hero();
    mission_name();
    versus_hides_nothing_but_flags();
    bad_pointers_are_empty();
    unterminated_name_is_empty();
    ordinary_battle_is_not_versus();
    if (failures) return 1;
    std::puts("p3_presence: OK");
    return 0;
}
