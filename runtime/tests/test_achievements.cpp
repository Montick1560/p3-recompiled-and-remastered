// Achievement engine (psp_achievements.h): reads, compile rules, the two-check rule, garbage
// rejection, shrinking pending list and the < 20 us budget of one check.
#include "psp_achievements.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// 16 MB of guest RAM is enough: addresses 0x08000000..0x08FFFFFF mask into it.
static std::vector<uint8_t> g_ram(0x01000000, 0);
constexpr uint32_t kPtr = 0x08AA0000u, kBase = 0x08C00000u;
static void w8(uint32_t a, uint8_t v) { g_ram[a & 0x07FFFFFFu] = v; }
static void w16(uint32_t a, uint16_t v) { w8(a, v & 0xFF); w8(a + 1, v >> 8); }
static void w32(uint32_t a, uint32_t v) { w16(a, v & 0xFFFF); w16(a + 2, v >> 16); }
static uint32_t r32(const uint8_t* ram, uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(ram[(a + i) & 0x07FFFFFFu]) << (8 * i);
    return v;
}

static const PspFact kFacts[] = {
    {"missions", 0x10, 4, false, -1, 0, 0, 100000},
    {"level", 0x20, 1, false, -1, 0, 1, 99},
    {"flags.ragewolf", 0x30, 4, false, 3, 0, 0, 1},
    {"classes", 0x40, 1, false, -1, 8, 0, 8},
    {"temp", 0x50, 2, true, -1, 0, -1000, 1000},
    {"unlocked", 0x60, 4, false, -1, 3, 0, 3, 0x10},
};
static uint32_t test_base(const uint8_t* ram) { return r32(ram, kPtr); }
static bool test_sane(const uint8_t* ram, uint32_t base) { return ram[base & 0x07FFFFFFu] == 'E'; }
static void test_hero(const uint8_t*, uint32_t, char16_t out[17]) { out[0] = u'E'; out[1] = 0; }
static const PspAchievementSource kSrc = {"test-1", test_base, test_sane, test_hero, 0x100, kFacts, 5};

static const char* kCat = R"({"version":1,"achievements":[
  {"id":"m1","icon":"🥁","name":{"es":"Uno","en":"One"},"desc":{"es":"d","en":"d"},"when":{"fact":"missions","op":">=","value":1}},
  {"id":"m100","name":{"es":"Cien","en":"Hundred"},"when":{"fact":"missions","op":">=","value":100}},
  {"id":"lvl10","name":{"es":"N","en":"L"},"when":{"fact":"level","op":"==","value":10}},
  {"id":"wolf","name":{"es":"W","en":"W"},"when":{"fact":"flags.ragewolf","op":"==","value":1}},
  {"id":"cls3","name":{"es":"C","en":"C"},"when":{"fact":"classes","op":">=","value":3}},
  {"id":"anyc","name":{"es":"A","en":"A"},"when":{"any":[{"fact":"level","op":">=","value":50},{"fact":"missions","op":">=","value":1000}]}},
  {"id":"allc","name":{"es":"B","en":"B"},"when":{"all":[{"fact":"missions","op":">=","value":1},{"fact":"level","op":">=","value":5}]}},
  {"id":"neg","name":{"es":"T","en":"T"},"when":{"fact":"temp","op":"!=","value":0}},
  {"id":"nowhen","name":{"es":"X","en":"X"}},
  {"id":"badfact","name":{"es":"X","en":"X"},"when":{"fact":"nope","op":">=","value":1}},
  {"id":"badop","name":{"es":"X","en":"X"},"when":{"fact":"level","op":"<","value":1}},
  {"id":"badval","name":{"es":"X","en":"X"},"when":{"fact":"level","op":">=","value":"1"}},
  {"id":"nested","name":{"es":"X","en":"X"},"when":{"all":[{"any":[{"fact":"level","op":">=","value":1}]}]}},
  {"id":"emptyall","name":{"es":"X","en":"X"},"when":{"all":[]}}]})";

static void reset_ram() {
    std::fill(g_ram.begin(), g_ram.end(), 0);
    w32(kPtr, kBase);
    w8(kBase, 'E');
    w8(kBase + 0x20, 1);  // level 1 (the plausible minimum)
}

static std::vector<std::string> pending_ids(const PspAchCatalogue& c) {
    std::vector<std::string> v;
    for (const auto& p : c.checks.pending) v.push_back(c.entries[p.index].id);
    std::sort(v.begin(), v.end());
    return v;
}

static std::vector<std::string> run(PspAchCatalogue& c) {
    uint16_t g[64];
    const size_t n = psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 64);
    std::vector<std::string> v;
    for (size_t i = 0; i < n; i++) v.push_back(c.entries[g[i]].id);
    std::sort(v.begin(), v.end());
    return v;
}

static void reads() {
    reset_ram();
    w32(kBase + 0x10, 123456);
    CHECK(psp_ach_read(kFacts[0], g_ram.data(), kBase) == 123456, "u32");
    w32(kBase + 0x30, 0x8);
    CHECK(psp_ach_read(kFacts[2], g_ram.data(), kBase) == 1, "bit 3 set");
    w32(kBase + 0x30, 0x7);
    CHECK(psp_ach_read(kFacts[2], g_ram.data(), kBase) == 0, "bit 3 clear");
    w8(kBase + 0x40, 1); w8(kBase + 0x42, 9); w8(kBase + 0x47, 1);
    CHECK(psp_ach_read(kFacts[3], g_ram.data(), kBase) == 3, "count of non-zero entries");
    w16(kBase + 0x50, 0xFFFE);
    CHECK(psp_ach_read(kFacts[4], g_ram.data(), kBase) == -2, "signed 16");
    w32(kBase + 0x60, 1); w32(kBase + 0x64, 1); w32(kBase + 0x80, 1);
    CHECK(psp_ach_read(kFacts[5], g_ram.data(), kBase) == 2, "count of non-zero entries `stride` apart");
}

static void compile_rules() {
    PspAchCatalogue c = psp_ach_compile(kCat, kSrc, {"m100"});
    CHECK(c.entries.size() == 14, "every entry listed");
    CHECK(c.entries[0].icon == "🥁" && c.entries[0].name_es == "Uno" && c.entries[0].name_en == "One", "texts");
    CHECK(!c.entries[8].has_when && c.entries[0].has_when, "has_when");
    const std::vector<std::string> want = {"allc", "anyc", "cls3", "lvl10", "m1", "neg", "wolf"};
    CHECK(pending_ids(c) == want, "pending = with when, valid, not earned");
    int left_out = 0;
    for (const auto& l : c.log) left_out += l.find("left out") != std::string::npos;
    CHECK(left_out == 5, "badfact, badop, badval, nested, emptyall each logged");
    CHECK(psp_ach_compile("{", kSrc, {}).checks.pending.empty(), "bad json = nothing");
    CHECK(psp_ach_compile(R"({"version":2,"achievements":[]})", kSrc, {}).log.size() == 1, "other version logged");
    CHECK(c.checks.values.size() == 5, "scratch per fact");
}

static void two_check_rule() {
    reset_ram();
    PspAchCatalogue c = psp_ach_compile(kCat, kSrc, {});
    w32(kBase + 0x10, 1);
    w8(kBase + 0x20, 5);
    CHECK(run(c).empty(), "first check: nothing yet");
    const std::vector<std::string> want = {"allc", "m1"};
    CHECK(run(c) == want, "second consecutive check grants");
    CHECK(run(c).empty(), "granted once");
    w32(kBase + 0x10, 100);
    CHECK(run(c).empty(), "m100 first hold");
    w32(kBase + 0x10, 99);
    CHECK(run(c).empty(), "dropped back: streak reset");
    w32(kBase + 0x10, 100);
    CHECK(run(c).empty(), "hold again: first");
    CHECK(run(c) == std::vector<std::string>{"m100"}, "then granted");
}

static void garbage_never_grants() {
    reset_ram();
    PspAchCatalogue c = psp_ach_compile(kCat, kSrc, {});
    w32(kBase + 0x10, 5);
    CHECK(run(c).empty(), "first hold");
    w32(kPtr, 0);
    CHECK(run(c).empty(), "no struct");
    w32(kPtr, kBase);
    CHECK(run(c).empty(), "streak was reset by the invalid check");
    CHECK(run(c) == std::vector<std::string>{"m1"}, "two valid checks again");
    w32(kPtr, 0x04000000u);
    CHECK(run(c).empty() && run(c).empty(), "outside user RAM");
    w32(kPtr, 0x09FFFFC0u);
    CHECK(run(c).empty() && run(c).empty(), "struct would run past user RAM");
    w32(kPtr, kBase);
    w8(kBase, 'x');
    CHECK(run(c).empty() && run(c).empty(), "sane() false");
    w8(kBase, 'E');
    w8(kBase + 0x20, 200);
    w32(kBase + 0x30, 0x8);
    CHECK(run(c).empty() && run(c).empty(), "level 200 is implausible: the whole check is skipped");
}

static void shrinking_and_max() {
    reset_ram();
    PspAchCatalogue c = psp_ach_compile(kCat, kSrc, {});
    w32(kBase + 0x10, 1000); w8(kBase + 0x20, 50); w32(kBase + 0x30, 0x8);
    w8(kBase + 0x40, 1); w8(kBase + 0x41, 1); w8(kBase + 0x42, 1); w16(kBase + 0x50, 7);
    uint16_t g[64];
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 0, "first hold");
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 2, "at most max per call");
    size_t total = 2;
    for (int i = 0; i < 8; i++) total += psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2);
    CHECK(total == 7 && c.checks.pending.size() == 1, "the rest follow; only lvl10 (level 50 != 10) is left");
    w8(kBase + 0x20, 10);
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 0, "lvl10 first hold");
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 1 && c.checks.pending.empty(), "pending empties");
    CHECK(psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 2) == 0, "nothing left to do");
}

static void ring() {
    static PspAchRing<2> r;
    PspAchBatch a{}, b{}, c{}, out{};
    a.n = 1; b.n = 2; c.n = 3;
    CHECK(r.push(a) && r.push(b) && !r.push(c), "holds N, then full");
    CHECK(r.pop(&out) && out.n == 1 && r.push(c), "FIFO, room again");
    CHECK(r.pop(&out) && out.n == 2 && r.pop(&out) && out.n == 3 && !r.pop(&out), "order kept, then empty");
}

static void budget() {
    reset_ram();
    std::string cat = R"({"version":1,"achievements":[)";
    for (int i = 0; i < 40; i++) {
        cat += (i ? "," : "") + std::string(R"({"id":"a)") + std::to_string(i) +
               R"(","name":{"es":"a","en":"a"},"when":{"all":[{"fact":"missions","op":">=","value":)" +
               std::to_string(1000 + i) + R"(},{"fact":"classes","op":">=","value":8}]}})";
    }
    cat += "]}";
    PspAchCatalogue c = psp_ach_compile(cat, kSrc, {});
    CHECK(c.checks.pending.size() == 40, "40 pending");
    uint16_t g[64];
    const int iters = 2000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; i++) psp_ach_evaluate(c.checks, kSrc, g_ram.data(), g, 64);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / iters;
    std::printf("test_achievements: one check with 40 pending = %.2f us\n", us);
    CHECK(us < 20.0, "a check stays under 20 us");
}

int main() {
    reads();
    compile_rules();
    two_check_rule();
    garbage_never_grants();
    shrinking_and_max();
    ring();
    budget();
    if (failures == 0) std::printf("test_achievements: all passed\n");
    return failures == 0 ? 0 : 1;
}
