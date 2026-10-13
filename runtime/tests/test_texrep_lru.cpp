// TexrepLru: pure byte-accounted LRU behind the texture-replacement caches
// (decoded PNG images, GL textures). Header-only, no GL / threads / I/O.
#include "psp_texrep_lru.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static void put_under_budget() {
    TexrepLru<std::string> lru(100);
    CHECK(lru.trim().empty(), "empty lru trims nothing");
    lru.put("a", 40);
    lru.put("b", 30);
    CHECK(lru.bytes() == 70, "bytes account both puts");
    CHECK(lru.count() == 2, "count accounts both puts");
    CHECK(lru.contains("a") && lru.contains("b"), "keys present");
    CHECK(lru.trim().empty(), "under budget: no eviction");
    CHECK(lru.bytes() == 70, "under budget: bytes unchanged");
}

static void update_same_key_reaccounts() {
    TexrepLru<std::string> lru(100);
    lru.put("a", 10);
    lru.put("a", 30);
    CHECK(lru.count() == 1, "re-put keeps one entry");
    CHECK(lru.bytes() == 30, "re-put re-accounts the bytes");
}

static void trim_evicts_lru_first() {
    TexrepLru<std::string> lru(100);
    lru.put("a", 40);
    lru.put("b", 40);
    lru.touch("a");           // b is now the least recently used
    lru.put("c", 40);         // bytes 120 > 100: one eviction needed
    const std::vector<std::string> evicted = lru.trim();
    CHECK(evicted.size() == 1 && evicted[0] == "b", "evicts the LRU entry first");
    CHECK(lru.bytes() == 80, "bytes drop to the budget");
    CHECK(lru.contains("a") && lru.contains("c") && !lru.contains("b"), "a and c survive");
}

static void trim_keeps_entries_in_use() {
    TexrepLru<std::string> lru(50);
    lru.put("a", 40);
    lru.put("b", 40);
    const std::vector<std::string> evicted = lru.trim({"b"});
    CHECK(evicted.size() == 1 && evicted[0] == "a", "keep list protects b, evicts a");
    CHECK(lru.contains("b"), "kept key survives");
}

static void remove_accounts() {
    TexrepLru<std::string> lru(100);
    lru.put("a", 40);
    CHECK(lru.remove("a"), "remove reports the key was present");
    CHECK(!lru.remove("a"), "second remove reports absence");
    CHECK(lru.bytes() == 0 && lru.count() == 0, "bytes and count cleared");
}

static void oversized_entry_is_kept() {
    TexrepLru<std::string> lru(10);
    lru.put("a", 50);
    CHECK(lru.trim().empty(), "a single oversized entry is not evicted");
    CHECK(lru.contains("a") && lru.bytes() == 50, "oversized entry stays accounted");
}

static void set_budget_enforced_on_next_trim() {
    TexrepLru<std::string> lru(100);
    lru.put("a", 40);
    lru.put("b", 40);
    lru.set_budget(40);
    const std::vector<std::string> evicted = lru.trim();
    CHECK(evicted.size() == 1, "shrinking the budget evicts on the next trim");
    CHECK(lru.bytes() <= 40, "bytes fit the new budget");
}

static void trim_if_skips_ineligible_lru() {
    TexrepLru<std::string> lru(100);
    lru.put("a", 40);
    lru.put("b", 40);
    lru.touch("a");          // b is the least recently used
    lru.put("c", 40);        // bytes 120 > 100
    // The usual victim (b) is ineligible: the next LRU entry (a) goes instead.
    const std::vector<std::string> evicted =
        lru.trim_if({}, [](const std::string& k) { return k != "b"; });
    CHECK(evicted.size() == 1 && evicted[0] == "a", "skips the ineligible LRU entry");
    CHECK(lru.contains("b") && lru.contains("c") && !lru.contains("a"), "ineligible entry survives");
}

static void trim_if_all_ineligible_evicts_nothing() {
    TexrepLru<std::string> lru(100);
    lru.put("a", 60);
    lru.put("b", 60);
    const std::vector<std::string> evicted =
        lru.trim_if({}, [](const std::string&) { return false; });
    CHECK(evicted.empty(), "all ineligible: nothing is evicted");
    CHECK(lru.bytes() == 120 && lru.count() == 2, "over budget with live entries: bytes and count stay");
}

static void trim_if_evicts_eligible_in_lru_order() {
    TexrepLru<std::string> lru(60);
    lru.put("a", 30);
    lru.put("b", 30);
    lru.put("c", 30);
    lru.put("d", 30);        // bytes 120
    // a is ineligible; among eligible b < c < d, the budget needs b and c.
    const std::vector<std::string> evicted =
        lru.trim_if({}, [](const std::string& k) { return k != "a"; });
    CHECK(evicted.size() == 2 && evicted[0] == "b" && evicted[1] == "c",
          "eligible entries are evicted least-recently-used first");
    CHECK(lru.contains("a") && lru.contains("d"), "ineligible and newest entries stay");
    CHECK(lru.bytes() == 60, "bytes reach the budget");
}

static void trim_if_keep_still_wins() {
    TexrepLru<std::string> lru(40);
    lru.put("a", 30);        // eligible but kept
    lru.put("b", 30);        // the only real candidate (oldest unkept, eligible)
    lru.put("c", 30);        // ineligible and newest: never a victim
    const std::vector<std::string> evicted =
        lru.trim_if({"a"}, [](const std::string& k) { return k != "c"; });
    CHECK(evicted.size() == 1 && evicted[0] == "b", "keep list protects a though it is eligible");
    CHECK(lru.contains("a") && lru.contains("c"), "kept and ineligible entries survive");
    CHECK(lru.bytes() == 60, "budget stays exceeded when only protected entries remain");
}

static void clear_resets() {
    TexrepLru<std::string> lru(100);
    lru.put("a", 40);
    lru.clear();
    CHECK(lru.bytes() == 0 && lru.count() == 0, "clear resets accounting");
    CHECK(!lru.contains("a"), "clear drops keys");
}

int main() {
    put_under_budget();
    update_same_key_reaccounts();
    trim_evicts_lru_first();
    trim_keeps_entries_in_use();
    remove_accounts();
    oversized_entry_is_kept();
    set_budget_enforced_on_next_trim();
    trim_if_skips_ineligible_lru();
    trim_if_all_ineligible_evicts_nothing();
    trim_if_evicts_eligible_in_lru_order();
    trim_if_keep_still_wins();
    clear_resets();
    if (failures == 0) std::printf("test_texrep_lru: all passed\n");
    return failures == 0 ? 0 : 1;
}
