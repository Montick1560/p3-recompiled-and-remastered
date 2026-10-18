// unlocked.json store (psp_ach_store.h): append-only, atomic, nothing ever lost.
#include "psp_ach_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
static void put(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << t;
}
static const PspEarned kE{"2026-10-18T12:00:00Z", "DATA04", "Edson", "dxd-1.1.4"};

static fs::path fresh(const char* name) {
    const fs::path d = fs::temp_directory_path() / "psp_ach_store_test" / name;
    fs::remove_all(d);
    return d;
}

static void missing_then_created() {
    const fs::path f = fresh("a") / "achievements" / "unlocked.json";
    PspStoreState st = PspStoreState::Ok;
    CHECK(psp_ach_store_read(f.string(), &st).empty() && st == PspStoreState::Missing, "missing");
    std::string err;
    CHECK(psp_ach_store_add(f.string(), {{"story_prologue", kE}}, &err), "add creates folder and file");
    const auto m = psp_ach_store_read(f.string(), &st);
    CHECK(st == PspStoreState::Ok && m.size() == 1 && m.at("story_prologue").hero == "Edson" &&
          m.at("story_prologue").release == "dxd-1.1.4" && m.at("story_prologue").save == "DATA04", "read back");
    CHECK(!fs::exists(f.string() + ".tmp"), "no temp file left");
}

static void existing_entries_are_never_lost() {
    const fs::path f = fresh("b") / "unlocked.json";
    put(f, R"({"version":1,"extra":true,"earned":{"gone_id":{"date":"2020-01-01T00:00:00Z","note":"x"},
              "missions_1":{"date":"2026-01-01T00:00:00Z","save":"DATA08","hero":"edd","release":"vanilla-eu"}}})");
    std::string err;
    PspEarned later = kE;
    later.date = "2030-01-01T00:00:00Z";
    CHECK(psp_ach_store_add(f.string(), {{"missions_1", later}, {"vs_1", kE}}, &err), "add");
    PspStoreState st;
    const auto m = psp_ach_store_read(f.string(), &st);
    CHECK(m.size() == 3, "gone_id kept, vs_1 added");
    CHECK(m.at("missions_1").date == "2026-01-01T00:00:00Z" && m.at("missions_1").hero == "edd", "first earn kept");
    const std::string text = slurp(f);
    CHECK(text.find("\"note\"") != std::string::npos && text.find("\"extra\"") != std::string::npos, "unknown keys kept");
}

static void corrupt_is_kept_aside() {
    const fs::path d = fresh("c");
    const fs::path f = d / "unlocked.json";
    put(f, "{");
    PspStoreState st;
    CHECK(psp_ach_store_read(f.string(), &st).empty() && st == PspStoreState::Corrupt, "corrupt");
    std::string err;
    CHECK(psp_ach_store_add(f.string(), {{"vs_1", kE}}, &err), "add over a corrupt file");
    CHECK(slurp(d / "unlocked.corrupt.json") == "{", "the corrupt bytes are kept");
    CHECK(psp_ach_store_read(f.string(), &st).count("vs_1") == 1 && st == PspStoreState::Ok, "new file valid");
    put(f, "");
    CHECK(psp_ach_store_read(f.string(), &st).empty() && st == PspStoreState::Corrupt, "empty = corrupt");
}

static void newer_file_is_never_touched() {
    const fs::path f = fresh("d") / "unlocked.json";
    const std::string v2 = R"({"version":2,"earned":{"a":{}}})";
    put(f, v2);
    PspStoreState st;
    CHECK(psp_ach_store_read(f.string(), &st).empty() && st == PspStoreState::Newer, "newer");
    std::string err;
    CHECK(!psp_ach_store_add(f.string(), {{"vs_1", kE}}, &err) && !err.empty(), "refused");
    CHECK(slurp(f) == v2, "byte-identical");
}

static void text_helpers() {
    CHECK(psp_ach_iso_utc(0) == "1970-01-01T00:00:00Z", "epoch");
    CHECK(psp_ach_iso_utc(1792324800) == "2026-10-18T12:00:00Z", "2026-10-18 12:00 UTC");
    const char16_t edson[] = u"Edson";
    CHECK(psp_utf16_to_utf8(edson, 16) == "Edson", "ascii");
    const char16_t n[] = u"Ñandú";
    CHECK(psp_utf16_to_utf8(n, 16) == "\xC3\x91" "and\xC3\xBA", "latin");
    const char16_t pair[] = {0xD83C, 0xDFC6, 0};
    CHECK(psp_utf16_to_utf8(pair, 16) == "\xF0\x9F\x8F\x86", "surrogate pair (U+1F3C6)");
    const char16_t lone[] = {0xD83C, u'a', 0};
    CHECK(psp_utf16_to_utf8(lone, 16) == "\xEF\xBF\xBD" "a", "lone surrogate");
    const char16_t full[] = u"ABCDEFGHIJKLMNOPQRS";
    CHECK(psp_utf16_to_utf8(full, 16).size() == 16, "max units");
}

int main() {
    missing_then_created();
    existing_entries_are_never_lost();
    corrupt_is_kept_aside();
    newer_file_is_never_touched();
    text_helpers();
    if (failures == 0) std::printf("test_ach_store: all passed\n");
    return failures == 0 ? 0 : 1;
}
