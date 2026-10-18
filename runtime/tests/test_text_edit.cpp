// psp_text_edit: the on-screen keyboard's line editor (pure, UTF-8 in/out,
// limit counted in characters like the PSP field limit).
#include "psp_text_edit.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static void starts_with_initial_text() {
    TextEdit e;
    e.reset("Hero", 8);
    CHECK(e.utf8() == "Hero", "initial text kept");
    CHECK(e.length() == 4, "length in characters");
}

static void typing_respects_the_limit() {
    TextEdit e;
    e.reset("", 5);
    e.insert("Pata");
    e.insert("pon!");  // only one more character fits
    CHECK(e.utf8() == "Patap", "insert stops at the field limit");
}

static void accents_count_as_one_character() {
    TextEdit e;
    e.reset("", 4);
    e.insert(u8"Ñandú");  // 5 characters, 7 bytes
    CHECK(e.utf8() == u8"Ñand", "limit counts characters, not bytes");
    e.backspace();
    CHECK(e.utf8() == u8"Ñan", "backspace removes a whole character");
}

static void backspace_on_empty_is_harmless() {
    TextEdit e;
    e.reset("", 3);
    e.backspace();
    CHECK(e.utf8().empty() && e.length() == 0, "empty stays empty");
}

static void control_characters_are_ignored() {
    TextEdit e;
    e.reset("", 10);
    e.insert("a\tb\nc\x7f");
    CHECK(e.utf8() == "abc", "tabs, newlines and DEL are not inserted");
}

static void initial_text_longer_than_limit_is_cut() {
    TextEdit e;
    e.reset("Patapon", 3);
    CHECK(e.utf8() == "Pat", "initial text truncated to the limit");
}

static void cursor_editing() {
    TextEdit e;
    e.reset("Pon", 5);
    CHECK(e.cursor() == 3, "cursor starts at the end");
    e.move_cursor(-2);
    e.insert("a");
    CHECK(e.utf8() == "Paon" && e.cursor() == 2, "insert at the cursor");
    e.backspace();
    CHECK(e.utf8() == "Pon" && e.cursor() == 1, "backspace before the cursor");
    e.move_cursor(-9);
    e.backspace();
    CHECK(e.utf8() == "Pon" && e.cursor() == 0, "backspace at the start does nothing");
    e.move_cursor(99);
    CHECK(e.cursor() == 3, "cursor clamps at the end");
    e.insert("ñañá");  // limit 5: only two more characters
    CHECK(e.utf8() == "Ponña" && e.cursor() == 5, "limit counted in characters at the cursor");
}

int main() {
    starts_with_initial_text();
    typing_respects_the_limit();
    accents_count_as_one_character();
    backspace_on_empty_is_harmless();
    control_characters_are_ignored();
    initial_text_longer_than_limit_is_cut();
    cursor_editing();
    if (failures == 0) std::printf("test_text_edit: all passed\n");
    return failures == 0 ? 0 : 1;
}
