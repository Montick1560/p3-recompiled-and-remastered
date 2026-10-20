// Patapon 3 field map + the official catalogue over the user's decrypted saves (skipped when absent;
// the saves are never committed): DATA04 earns exactly the documented list, DATA08 its own.
#include "p3_achievements.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static const std::vector<std::string> kWant04 = {
    "archfiend_impetuosity", "archfiend_inflexibility", "archfiend_justice", "archfiend_purity",
    "archfiend_restraint", "archfiend_tolerance", "archfiend_valor", "bonedeth_all", "boss_centura", "boss_cioking",
    "boss_dogaeen", "boss_ganodias", "boss_gigantus", "boss_goruru", "boss_kacchindonga", "boss_majidonga",
    "boss_manboth", "boss_mochicchichi", "boss_shooshookle", "boss_zaknel", "boss_zuttankarmen", "chin_15",
    "chin_25", "chin_32", "class_level_32", "class_level_40", "classes_15", "classes_5", "classes_all",
    "classes_bow", "classes_dark_all", "classes_every", "classes_shield", "classes_spear", "coop_1", "coop_100",
    "dark_hero_buzzcrave", "dark_hero_covet_hiss", "dark_hero_naughtyfins", "dark_hero_ragewolf",
    "dark_hero_ravenous", "dark_hero_slogturtle", "dark_hero_sonarchy", "dark_heroes_all", "dark_uber_justice",
    "dark_uber_shockwaves", "dark_uber_slogging", "dark_uber_treasure", "divine_1", "dragon_hunter", "dragon_lord",
    "duel_twice", "dungeon_twice", "dxd_gigante_king", "hero_level_10", "hero_level_30", "heroes_defeated_10",
    "heroes_defeated_100", "hoshipon_all", "kaching_1m", "kan_15", "kan_25", "kan_32", "missions_1", "missions_100",
    "missions_1000", "multi_100", "multi_1000", "multi_feisu", "multi_forudo", "multi_hyumitto", "multi_inosen",
    "multi_justi", "multi_libera", "multi_valo", "perfect_1000", "perfect_100000", "playtime_1000h", "playtime_100h",
    "popular_10", "popular_30", "recruit_10", "rematch_ragewolf", "rematch_ravenous", "rematch_slogturtle",
    "rematch_sonarchy", "replay_libera_100", "replay_multi_100", "secret_heroes_1000", "smith_2", "smith_3",
    "smith_4", "story_all_archfiends", "story_ending", "story_first_archfiend", "story_prologue", "sukopon_boss",
    "team_points_10000", "ton_15", "ton_25", "ton_32", "uberhero_15", "uberhero_25", "unit_level_30",
    "unlock_alosson", "unlock_bowmunk", "unlock_buzzcrave", "unlock_cannassault", "unlock_cannogabang",
    "unlock_charibasa", "unlock_covet_hiss", "unlock_destrobo", "unlock_grenburr", "unlock_guardira",
    "unlock_jamsch", "unlock_kibadda", "unlock_myamsar", "unlock_naughtyfins", "unlock_oohoroc", "unlock_piekron",
    "unlock_pingrek", "unlock_pyokorider", "unlock_ragewolf", "unlock_ravenous", "unlock_slogturtle",
    "unlock_sonarchy", "unlock_taterazay", "unlock_tondenga", "unlock_wondabarappa", "unlock_wooyari",
    "unlock_yumiyacha", "vs_1", "vs_50"};
static const std::vector<std::string> kWant08 = {"missions_1"};

// Puts the save at a heap address and the dxd-1.1.4 chain (from the doc) in front of it.
static std::vector<std::string> earned_by(const std::string& save, const PspAchievementSource& src) {
    std::vector<uint8_t> ram(0x01000000, 0);
    const uint32_t base = 0x08C00000u;
    const std::string bytes = slurp(save);
    std::copy(bytes.begin(), bytes.end(), ram.begin() + (base & 0x07FFFFFFu));
    auto w32 = [&ram](uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) ram[(a + i) & 0x07FFFFFFu] = (v >> (8 * i)) & 0xFF; };
    // Write the chain so that src.base(ram) == base (adapt to the doc's chain: global -> object, object + off -> base).
    const uint32_t object = 0x08B00000u;
    w32(0x08AABD94u, object);
    w32(object + 0x50u, base);
    CHECK(src.base(ram.data()) == base, "chain leads to the struct");
    CHECK(src.sane(ram.data(), base), "the save passes the sanity check");
    PspAchCatalogue c = psp_ach_compile(slurp(PSP_P3_CATALOGUE), src, {});
    CHECK(c.log.empty(), "every catalogue entry compiles");
    uint16_t g[256];
    std::vector<std::string> ids;
    for (int i = 0; i < 2; i++) {
        const size_t n = psp_ach_evaluate(c.checks, src, ram.data(), g, 256);
        for (size_t k = 0; k < n; k++) ids.push_back(c.entries[g[k]].id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

int main() {
    size_t n = 0;
    const PspAchievementSource* s = p3_achievement_sources(&n);
    CHECK(n >= 1 && std::string(s[0].release) == "dxd-1.1.4", "dxd-1.1.4 has a field map");
    std::vector<uint8_t> zero(0x01000000, 0);
    CHECK(s[0].base(zero.data()) == 0, "no struct in empty RAM");
    const std::string dir = PSP_P3_SAVES_DIR;
    if (std::ifstream(dir + "/plain_DATA04.bin").good() && std::ifstream(dir + "/plain_DATA08.bin").good()) {
        CHECK(earned_by(dir + "/plain_DATA04.bin", s[0]) == kWant04, "DATA04 earns the documented list");
        CHECK(earned_by(dir + "/plain_DATA08.bin", s[0]) == kWant08, "DATA08 earns the documented list");
    } else {
        std::printf("test_p3_achievements: user saves not on this PC, save checks skipped\n");
    }
    if (failures == 0) std::printf("test_p3_achievements: all passed\n");
    return failures == 0 ? 0 : 1;
}
