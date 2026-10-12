// GeTexCachePick: slot-selection policy of the GL texture cache
// (runtime/src/psp_ge_texture.cpp). Header-only, no GL / threads / I/O.
#include "psp_ge_texcache.h"

#include <array>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

struct Entry {
    uint32_t psp_addr = 0;
    uint32_t content_hash = 0;
    uint32_t last_frame = 0;
    bool valid = false;
};

using Cache = std::array<Entry, 4>;

static Entry valid_entry(uint32_t addr, uint32_t hash, uint32_t frame) {
    Entry e;
    e.psp_addr = addr;
    e.content_hash = hash;
    e.last_frame = frame;
    e.valid = true;
    return e;
}

static void empty_cache_picks_first_slot() {
    Cache c{};
    int slot = -1;
    CHECK(ge_texcache_pick(c, 0x1000, 0xAA, &slot) == GeTexCachePick::Empty,
          "empty cache: picks a vacancy");
    CHECK(slot == 0, "empty cache: first slot");
}

static void exact_match_wins() {
    Cache c{};
    c[2] = valid_entry(0x1000, 0xAA, 5);
    int slot = -1;
    CHECK(ge_texcache_pick(c, 0x1000, 0xAA, &slot) == GeTexCachePick::Match,
          "same addr+hash: match");
    CHECK(slot == 2, "match reports its own slot");
}

static void hash_or_addr_miss_is_not_a_match() {
    Cache c{};
    c[0] = valid_entry(0x1000, 0xAA, 5);
    int slot = -1;
    CHECK(ge_texcache_pick(c, 0x1000, 0xBB, &slot) == GeTexCachePick::Empty,
          "same addr, new hash: not a match");
    CHECK(slot == 1, "same addr, new hash: next vacancy");
}

// Regression for the 5df03817 review finding: an external eviction (budget
// trim) leaves a hole below a texture that is still resident; requesting the
// resident texture must find it instead of re-uploading a duplicate into the
// hole, and only a genuinely unseen texture may reuse the hole.
static void match_above_a_hole_is_found() {
    Cache c{};
    c[0] = valid_entry(0x1000, 0xAA, 1);  // A
    c[2] = valid_entry(0x3000, 0xCC, 3);  // C, resident above the hole
    int slot = -1;
    CHECK(ge_texcache_pick(c, 0x3000, 0xCC, &slot) == GeTexCachePick::Match,
          "resident C found above the hole");
    CHECK(slot == 2, "C keeps its slot: no duplicate upload");
    CHECK(ge_texcache_pick(c, 0x4000, 0xDD, &slot) == GeTexCachePick::Empty,
          "a new texture reuses a vacancy");
    CHECK(slot == 1, "new texture takes the first vacated slot");
}

static void full_cache_evicts_lru() {
    Cache c{};
    c[0] = valid_entry(0x1000, 0xAA, 9);
    c[1] = valid_entry(0x2000, 0xBB, 3);  // oldest
    c[2] = valid_entry(0x3000, 0xCC, 7);
    c[3] = valid_entry(0x4000, 0xDD, 5);
    int slot = -1;
    CHECK(ge_texcache_pick(c, 0x9000, 0xEE, &slot) == GeTexCachePick::Evict,
          "full cache: evicts");
    CHECK(slot == 1, "evicts the least-recently-used frame");
}

static void full_cache_still_matches() {
    Cache c{};
    c[0] = valid_entry(0x1000, 0xAA, 9);
    c[1] = valid_entry(0x2000, 0xBB, 3);
    c[2] = valid_entry(0x3000, 0xCC, 7);
    c[3] = valid_entry(0x4000, 0xDD, 5);
    int slot = -1;
    CHECK(ge_texcache_pick(c, 0x2000, 0xBB, &slot) == GeTexCachePick::Match,
          "full cache: match skips eviction");
    CHECK(slot == 1, "full cache: match reports its own slot");
}

int main() {
    empty_cache_picks_first_slot();
    exact_match_wins();
    hash_or_addr_miss_is_not_a_match();
    match_above_a_hole_is_found();
    full_cache_evicts_lru();
    full_cache_still_matches();
    if (failures == 0) std::printf("test_ge_texcache: all passed\n");
    return failures == 0 ? 0 : 1;
}
