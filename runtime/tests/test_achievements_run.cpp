// The running system (psp_achievements_run.h) with a fake source over a 16 MB guest RAM: grants reach
// unlocked.json and the toast poster; off paths install nothing.
#include "psp_achievements_run.h"
#include "psp_ach_store.h"
#include "psp_vblank_hook.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::vector<uint8_t> g_ram(0x01000000, 0);
constexpr uint32_t kPtr = 0x08AA0000u, kBase = 0x08C00000u;
static void w32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) g_ram[(a + i) & 0x07FFFFFFu] = (v >> (8 * i)) & 0xFF; }
static uint32_t r32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(ram[(a + i) & 0x07FFFFFFu]) << (8 * i);
    return v;
}
static const PspFact kFacts[] = {{"missions", 0x10, 4, false, -1, 0, 0, 100000}};
static uint32_t fbase(const uint8_t* ram) { return r32(ram, kPtr); }
static bool fsane(const uint8_t*, uint32_t) { return true; }
static void fhero(const uint8_t*, uint32_t, char16_t out[17]) {
    const char16_t* n = u"Edson";
    for (int i = 0; i < 6; i++) out[i] = n[i];
}
static const PspAchievementSource kSrc[] = {{"test-1", fbase, fsane, fhero, 0x100, kFacts, 1}};

static std::vector<ToastText> g_toasts;
static void record(const ToastText& t) { g_toasts.push_back(t); }

static void setenv_(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static fs::path g_dir;
static void write(const fs::path& p, const std::string& t) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << t; }

static void prepare(int entries, const char* release = "test-1") {
    g_dir = fs::temp_directory_path() / "psp_ach_run_test";
    fs::remove_all(g_dir);
    std::string cat = R"({"version":1,"achievements":[)";
    for (int i = 0; i < entries; i++)
        cat += (i ? "," : "") + std::string(R"({"id":"m)") + std::to_string(i) + R"(","icon":"🥁","name":{"es":"Misión )" +
               std::to_string(i) + R"(","en":"Mission )" + std::to_string(i) +
               R"("},"when":{"fact":"missions","op":">=","value":)" + std::to_string(i + 1) + "}}";
    cat += "]}";
    write(g_dir / "exe" / "achievements.json", cat);
    write(g_dir / "exe" / "release.json", std::string(R"({"id":")") + release + R"("})");
    setenv_("PSPRECOMP_ACHIEVEMENTS_FILE", (g_dir / "root" / "achievements" / "unlocked.json").string().c_str());
    setenv_("PSPRECOMP_ACHIEVEMENTS", "");
    setenv_("PSPRECOMP_ACHIEVEMENT_TOASTS", "1");
    setenv_("PSPRECOMP_UI_LANG", "es");
    setenv_("PSPRECOMP_SAVE_SLOT", "_DATA04");
    std::fill(g_ram.begin(), g_ram.end(), 0);
    w32(kPtr, kBase);
    g_toasts.clear();
}
static PspAchRunOptions opts() { return {(g_dir / "exe").string(), record}; }
static void vblanks(int n) { for (int i = 0; i < n; i++) psp_vblank_hook_run(g_ram.data()); }
static std::map<std::string, PspEarned> earned() {
    PspStoreState st;
    return psp_ach_store_read((g_dir / "root" / "achievements" / "unlocked.json").string(), &st);
}

static void grant_reaches_file_and_toast() {
    prepare(2);
    psp_achievements_register(kSrc, 1);
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 1);
    vblanks(119);
    psp_achievements_stop();
    CHECK(earned().empty(), "one check only after 119 vblanks: nothing yet");
    CHECK(psp_achievements_start(opts()), "starts again");
    vblanks(120);
    psp_achievements_stop();
    const auto e = earned();
    CHECK(e.size() == 1 && e.count("m0"), "m0 earned after two checks");
    CHECK(e.at("m0").release == "test-1" && e.at("m0").save == "DATA04" && e.at("m0").hero == "Edson", "fields");
    CHECK(e.at("m0").date.size() == 20 && e.at("m0").date.back() == 'Z', "ISO date");
    CHECK(g_toasts.size() == 1 && g_toasts[0].title == "Logro desbloqueado" && g_toasts[0].name == "🥁 Misión 0", "toast");
}

static void earned_at_start_is_not_pending() {
    // continues from the previous test's file: m0 is earned
    g_toasts.clear();
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 1);
    vblanks(240);
    psp_achievements_stop();
    CHECK(g_toasts.empty(), "no second toast for m0");
}

static void a_big_batch_is_one_write_and_one_toast() {
    prepare(12);
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 100);
    vblanks(120);
    psp_achievements_stop();
    CHECK(earned().size() == 12, "all 12 recorded");
    CHECK(g_toasts.size() == 1 && g_toasts[0].name == "🏆 12 logros", "one summary toast");
}

static void toasts_off_still_records() {
    prepare(1);
    setenv_("PSPRECOMP_ACHIEVEMENT_TOASTS", "0");
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 5);
    vblanks(120);
    psp_achievements_stop();
    CHECK(earned().size() == 1 && g_toasts.empty(), "recorded, no toast");
}

static void stop_drains_the_ring() {
    prepare(1);
    CHECK(psp_achievements_start(opts()), "starts");
    w32(kBase + 0x10, 5);
    vblanks(120);
    psp_achievements_stop();  // immediately: the worker has not woken yet
    CHECK(earned().size() == 1, "written by stop()");
}

static void off_without_file_or_with_kill_switch() {
    prepare(1);
    setenv_("PSPRECOMP_ACHIEVEMENTS", "0");
    CHECK(!psp_achievements_start(opts()), "kill switch");
    w32(kBase + 0x10, 5);
    vblanks(240);
    CHECK(earned().empty(), "no hook ran");
    prepare(1);
    setenv_("PSPRECOMP_ACHIEVEMENTS_FILE", "");
    CHECK(!psp_achievements_start(opts()), "no settings.ini: off");
    prepare(1, "other-release");
    CHECK(!psp_achievements_start(opts()), "release without a field map: off");
    prepare(1);
    fs::remove(g_dir / "exe" / "achievements.json");
    CHECK(!psp_achievements_start(opts()), "no catalogue: off");
    psp_achievements_stop();  // safe when not started
}

int main() {
    grant_reaches_file_and_toast();
    earned_at_start_is_not_pending();
    a_big_batch_is_one_write_and_one_toast();
    toasts_off_still_records();
    stop_drains_the_ring();
    off_without_file_or_with_kill_switch();
    if (failures == 0) std::printf("test_achievements_run: all passed\n");
    return failures == 0 ? 0 : 1;
}
