// psp_ge_texmemo: hash each texture at most once per display list (pure).
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

int main() {
    same_list_reuses();
    rehash_next_frame();
    different_params_rehash();
    transfer_invalidates();
    if (failures == 0) std::printf("test_tex_memo: all passed\n");
    return failures == 0 ? 0 : 1;
}
