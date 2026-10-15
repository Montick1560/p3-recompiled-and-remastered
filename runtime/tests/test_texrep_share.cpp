// psp_texrep_share: one GL texture per pack file, shared by every cache slot
// whose texture resolves to that file (pure, reference-counted).
#include "psp_texrep_share.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            failures++;                                        \
        }                                                      \
    } while (0)

static void unknown_path_has_no_texture() {
    TexrepShare s;
    CHECK(s.acquire("ui/a.png") == 0, "nothing uploaded yet");
    CHECK(s.refs("ui/a.png") == 0, "a failed acquire takes no reference");
}

static void second_slot_shares_the_upload() {
    TexrepShare s;
    s.adopt("ui/a.png", 7);  // first slot uploaded the pack image
    CHECK(s.acquire("ui/a.png") == 7, "same file: reuse the live texture");
    CHECK(s.refs("ui/a.png") == 2, "uploader + sharer");
    CHECK(s.acquire("ui/b.png") == 0, "other files are not shared");
}

static void texture_lives_until_last_reference() {
    TexrepShare s;
    s.adopt("ui/a.png", 7);
    s.acquire("ui/a.png");
    s.acquire("ui/a.png");
    CHECK(!s.drop("ui/a.png"), "two references left: keep the texture");
    CHECK(!s.drop("ui/a.png"), "one reference left: keep the texture");
    CHECK(s.drop("ui/a.png"), "last reference: the caller deletes it");
    CHECK(s.acquire("ui/a.png") == 0, "deleted: the next slot uploads again");
}

static void drop_of_unknown_path_is_harmless() {
    TexrepShare s;
    CHECK(!s.drop("ui/a.png"), "nothing to delete");
}

static void clear_forgets_everything() {
    TexrepShare s;
    s.adopt("ui/a.png", 7);
    s.clear();
    CHECK(s.acquire("ui/a.png") == 0, "cleared with the GL cache");
}

int main() {
    unknown_path_has_no_texture();
    second_slot_shares_the_upload();
    texture_lives_until_last_reference();
    drop_of_unknown_path_is_harmless();
    clear_forgets_everything();
    if (failures == 0) std::printf("test_texrep_share: all passed\n");
    return failures == 0 ? 0 : 1;
}
