// psp_ge_texmemo: hash each texture at most once per display list, with a
// frame-delayed backoff for textures whose content verified unchanged (pure).
#include "psp_ge_texmemo.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static void same_list_reuses() {
    TexHashMemo m;
    uint32_t h = 0;
    CHECK(!m.find(1, 0x04000000, 0xAA, &h), "first bind hashes");
    m.put(1, 0x04000000, 0xAA, 0x1234);
    CHECK(m.find(1, 0x04000000, 0xAA, &h) && h == 0x1234, "same list, same texture: reuse");
}

static void rehash_next_frame() {
    TexHashMemo m;
    uint32_t h = 0;
    m.put(1, 0x04000000, 0xAA, 0x1234);
    CHECK(!m.find(2, 0x04000000, 0xAA, &h), "next list: rehash (CPU may have rewritten it)");
    m.put(2, 0x04000000, 0xAA, 0x5678);
    CHECK(m.find(2, 0x04000000, 0xAA, &h) && h == 0x5678, "new hash reused in the new list");
}

static void different_params_rehash() {
    TexHashMemo m;
    uint32_t h = 0;
    m.put(1, 0x04000000, 0xAA, 0x1234);
    CHECK(!m.find(1, 0x04000000, 0xBB, &h), "same address, other params (CLUT/format): rehash");
    CHECK(!m.find(1, 0x04001000, 0xAA, &h), "other address: rehash");
}

static void transfer_invalidates() {
    TexHashMemo m;
    uint32_t h = 0;
    m.put(1, 0x04000000, 0xAA, 0x1234);
    m.invalidate();  // a GE block transfer wrote guest memory mid-list
    CHECK(!m.find(1, 0x04000000, 0xAA, &h), "after a block transfer: rehash");
}

static void stable_texture_backs_off() {
    TexHashMemo m;
    uint32_t h = 0;
    m.put(1, 0x04000000, 0xAA, 0x1234);  // new: verify the next list once more
    CHECK(!m.find(2, 0x04000000, 0xAA, &h), "new texture: verify again next list");
    m.put(2, 0x04000000, 0xAA, 0x1234);  // unchanged -> window 2
    CHECK(m.find(3, 0x04000000, 0xAA, &h) && h == 0x1234, "stable: window 2 covers list 3");
    CHECK(!m.find(4, 0x04000000, 0xAA, &h), "window 2 expires at list 4");
    m.put(4, 0x04000000, 0xAA, 0x1234);  // unchanged -> window 4
    CHECK(m.find(7, 0x04000000, 0xAA, &h) && h == 0x1234, "window 4 covers lists 5..7");
    CHECK(!m.find(8, 0x04000000, 0xAA, &h), "window 4 expires at list 8");
    m.put(8, 0x04000000, 0xAA, 0x1234);  // unchanged -> window 8 (cap)
    CHECK(m.find(15, 0x04000000, 0xAA, &h) && h == 0x1234, "window 8 covers lists 9..15");
    CHECK(!m.find(16, 0x04000000, 0xAA, &h), "window 8 expires at list 16");
    m.put(16, 0x04000000, 0xAA, 0x1234);  // unchanged -> still window 8
    CHECK(m.find(23, 0x04000000, 0xAA, &h) && h == 0x1234, "window capped at 8");
    CHECK(!m.find(24, 0x04000000, 0xAA, &h), "cap: verifies every 8 lists");
}

static void changed_hash_verifies_every_list() {
    TexHashMemo m;
    uint32_t h = 0;
    m.put(1, 0x04000000, 0xAA, 0x1111);
    for (uint32_t list = 2; list < 20; list++) {
        CHECK(!m.find(list, 0x04000000, 0xAA, &h),
              "a hash that keeps changing: verify every list");
        m.put(list, 0x04000000, 0xAA, 0x1000 + list);
    }
}

static void change_resets_window() {
    TexHashMemo m;
    uint32_t h = 0;
    m.put(1, 0x04000000, 0xAA, 0x1111);
    m.put(2, 0x04000000, 0xAA, 0x1111);  // stable -> window 2, next check list 4
    // The content changed at list 3; the scheduled check catches it at list 4.
    CHECK(!m.find(4, 0x04000000, 0xAA, &h), "scheduled recheck");
    m.put(4, 0x04000000, 0xAA, 0x2222);
    CHECK(!m.find(5, 0x04000000, 0xAA, &h), "a change resets to per-list verification");
}

static void zero_hash_is_a_valid_entry() {
    TexHashMemo m;
    uint32_t h = 0xFFFF;
    m.put(1, 0x04000000, 0xAA, 0);
    CHECK(m.find(1, 0x04000000, 0xAA, &h) && h == 0, "hash 0 still counts as a hit");
}

static void window_configurable() {
    TexHashMemo once(1);  // 1 = verify every list (the backoff off)
    uint32_t h = 0;
    once.put(1, 0x04000000, 0xAA, 0x1234);
    CHECK(!once.find(2, 0x04000000, 0xAA, &h), "window 1: verify every list");
    once.put(2, 0x04000000, 0xAA, 0x1234);
    CHECK(!once.find(3, 0x04000000, 0xAA, &h), "window 1 stays at one list");

    TexHashMemo two(2);  // windows 1 then 2
    two.put(1, 0x04000000, 0xAA, 0x1234);
    two.put(2, 0x04000000, 0xAA, 0x1234);
    CHECK(two.find(3, 0x04000000, 0xAA, &h), "window 2");
    CHECK(!two.find(4, 0x04000000, 0xAA, &h), "window 2 expires");
}

static void entry_cap_drops_old() {
    TexHashMemo m;
    uint32_t h = 0;
    for (uint32_t i = 0; i < 5000; i++) {
        m.put(1, 0x04000000 + i * 0x1000, 0xAA, 0x1234);
    }
    CHECK(m.find(1, 0x04000000 + 4999 * 0x1000, 0xAA, &h) && h == 0x1234,
          "newest entry survives the cap");
    CHECK(!m.find(1, 0x04000000, 0xAA, &h), "old entries were dropped");
}

int main() {
    same_list_reuses();
    rehash_next_frame();
    different_params_rehash();
    transfer_invalidates();
    stable_texture_backs_off();
    changed_hash_verifies_every_list();
    change_resets_window();
    zero_hash_is_a_valid_entry();
    window_configurable();
    entry_cap_drops_old();
    if (failures == 0) std::printf("test_tex_memo: all passed\n");
    return failures == 0 ? 0 : 1;
}
