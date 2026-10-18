#include "psp_achievements_run.h"

#include "psp_ach_store.h"
#include "psp_vblank_hook.h"

#include "json.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int kCheckEvery = 60;  // vblanks between two checks (~1 s)

const PspAchievementSource* g_sources = nullptr;
size_t g_source_count = 0;

struct Run {
    const PspAchievementSource* src = nullptr;
    PspAchCatalogue cat;
    std::string file, save, lang;
    bool toasts = true;
    void (*post)(const ToastText&) = nullptr;
    PspAchRing<16> ring;   // batches; one per check that granted something
    std::atomic<bool> stop{false};
    std::thread worker;
    int countdown = kCheckEvery;
};
// Published before the hook is installed. Never deleted: at shutdown a game thread may still be inside
// on_vblank after the hook is removed (a few KB left to the process exit).
std::atomic<Run*> g_run{nullptr};

bool slurp(const std::string& path, std::string* out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream s;
    s << f.rdbuf();
    *out = s.str();
    return true;
}

// Game thread, every vblank: one decrement and one compare on 59 of 60.
void on_vblank(uint8_t* rdram) {
    Run* r = g_run.load(std::memory_order_acquire);
    if (!r || --r->countdown > 0) return;
    r->countdown = kCheckEvery;
    PspAchBatch b{};
    b.n = static_cast<uint16_t>(psp_ach_evaluate(r->cat.checks, *r->src, rdram, b.index, kAchBatchMax));
    if (b.n == 0) return;
    r->src->hero(rdram, r->src->base(rdram), b.hero);
    b.hero[16] = 0;
    if (!r->ring.push(b)) std::fprintf(stderr, "[ACH] queue full: %u achievements wait for the next session\n", b.n);
}

// Worker: everything earned since the last pass, in one file write and one batch of toasts.
void drain(Run& r) {
    std::vector<std::pair<std::string, PspEarned>> add;
    std::vector<std::pair<std::string, std::string>> icon_name;
    PspAchBatch b;
    const std::string date = psp_ach_iso_utc(std::time(nullptr));
    while (r.ring.pop(&b)) {
        const std::string hero = psp_utf16_to_utf8(b.hero, 16);
        for (uint16_t i = 0; i < b.n; i++) {
            const PspAchEntry& e = r.cat.entries[b.index[i]];
            add.push_back({e.id, {date, r.save, hero, r.src->release}});
            icon_name.push_back({e.icon, r.lang == "en" ? e.name_en : e.name_es});
        }
    }
    if (add.empty()) return;
    std::string err;
    if (!psp_ach_store_add(r.file, add, &err)) std::fprintf(stderr, "[ACH] %s\n", err.c_str());
    for (const auto& a : add) std::fprintf(stderr, "[ACH] earned %s\n", a.first.c_str());
    if (r.toasts && r.post)
        for (const ToastText& t : psp_toasts_for(icon_name, r.lang.c_str())) r.post(t);
}

void worker_main(Run* r) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    while (!r->stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        drain(*r);
    }
    drain(*r);
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

}  // namespace

void psp_achievements_register(const PspAchievementSource* sources, size_t n) {
    g_sources = sources;
    g_source_count = n;
}

bool psp_achievements_start(const PspAchRunOptions& opt) {
    if (env("PSPRECOMP_ACHIEVEMENTS") == "0") { std::fprintf(stderr, "[ACH] off (PSPRECOMP_ACHIEVEMENTS=0)\n"); return false; }
    const std::string file = env("PSPRECOMP_ACHIEVEMENTS_FILE");
    if (file.empty()) { std::fprintf(stderr, "[ACH] off: no settings.ini (PSPRECOMP_ACHIEVEMENTS_FILE unset)\n"); return false; }
    std::string text;
    if (!slurp(opt.exe_dir + "/release.json", &text)) { std::fprintf(stderr, "[ACH] off: no release.json\n"); return false; }
    const nlohmann::json rel = nlohmann::json::parse(text, nullptr, false);
    const std::string release = rel.is_object() ? rel.value("id", "") : "";
    const PspAchievementSource* src = nullptr;
    for (size_t i = 0; i < g_source_count; i++)
        if (release == g_sources[i].release) src = &g_sources[i];
    if (!src) { std::fprintf(stderr, "[ACH] off: no field map for release \"%s\"\n", release.c_str()); return false; }
    if (!slurp(opt.exe_dir + "/achievements.json", &text)) { std::fprintf(stderr, "[ACH] off: no achievements.json\n"); return false; }
    PspStoreState st;
    std::vector<std::string> earned;
    for (const auto& kv : psp_ach_store_read(file, &st)) earned.push_back(kv.first);
    if (st == PspStoreState::Corrupt) std::fprintf(stderr, "[ACH] unlocked.json is corrupt: it is kept aside on the next write\n");
    if (st == PspStoreState::Newer) { std::fprintf(stderr, "[ACH] off: unlocked.json is from a newer build\n"); return false; }
    Run* r = new Run;
    r->src = src;
    r->cat = psp_ach_compile(text, *src, earned);
    for (const std::string& l : r->cat.log) std::fprintf(stderr, "%s\n", l.c_str());
    r->file = file;
    const std::string slot = env("PSPRECOMP_SAVE_SLOT");
    r->save = slot.size() > 1 && slot[0] == '_' ? slot.substr(1) : slot;
    r->lang = env("PSPRECOMP_UI_LANG") == "en" ? "en" : "es";
    r->toasts = env("PSPRECOMP_ACHIEVEMENT_TOASTS") != "0";
    r->post = opt.post_toast;
    std::fprintf(stderr, "[ACH] %s: %zu achievements, %zu earned, %zu checks pending\n", release.c_str(),
                 r->cat.entries.size(), earned.size(), r->cat.checks.pending.size());
    r->worker = std::thread(worker_main, r);
    g_run.store(r, std::memory_order_release);
    psp_vblank_hook_set(on_vblank);
    return true;
}

void psp_achievements_stop() {
    psp_vblank_hook_set(nullptr);
    Run* r = g_run.exchange(nullptr, std::memory_order_acq_rel);
    if (!r) return;
    r->stop.store(true, std::memory_order_release);
    r->worker.join();  // its last drain() writes what is queued
}
