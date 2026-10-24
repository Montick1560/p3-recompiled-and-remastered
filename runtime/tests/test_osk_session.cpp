// psp_osk_session: hand-off between the sceUtilityOsk HLE (guest thread)
// and the keyboard UI (main thread). Pure; no SDL.
#include "psp_osk_session.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static void confirm_returns_typed_text() {
    OskSession s;
    CHECK(!s.active(), "idle at start");
    s.open("Nombre", "Hero", 8);
    CHECK(s.active(), "active after open");
    std::string out;
    CHECK(!s.poll(&out), "no result while editing");
    s.backspace();
    s.backspace();
    s.backspace();
    s.backspace();
    s.type(u8"Tatepón");
    s.confirm();
    CHECK(!s.active(), "closed after confirm");
    CHECK(s.poll(&out) && out == u8"Tatepón", "poll returns the typed text");
    CHECK(!s.poll(&out), "result is delivered once");
}

static void cancel_returns_initial_text() {
    OskSession s;
    s.open("Nombre", "Hero", 8);
    s.type("xyz");
    s.cancel();
    std::string out;
    CHECK(s.poll(&out) && out == "Hero", "cancel keeps the initial text (PPSSPP)");
}

static void view_shows_title_and_text() {
    OskSession s;
    s.open("Equipo", "Pata", 6);
    s.type("pon!!");
    const OskSession::View v = s.view();
    CHECK(v.active && v.title == "Equipo", "title");
    CHECK(v.text == "Patapo" && v.length == 6 && v.max_len == 6, "text, length and limit");
}

static void input_while_idle_is_ignored() {
    OskSession s;
    s.type("abc");
    s.confirm();
    std::string out;
    CHECK(!s.poll(&out) && !s.active(), "nothing happens without an open request");
}

static void cursor_editing() {
    OskSession s;
    s.open("Equipo", "Pata", 8);
    s.move_cursor(-1);
    s.type("x");
    const OskSession::View v = s.view();
    CHECK(v.text == "Patxa" && v.cursor == 4, "typing inserts before the last character, view reports the cursor");
}

// Pad B (beta report #2: a gamepad could not leave "Conectar con ID online").
static void b_on_empty_text_cancels() {
    OskSession s;
    s.open("ID online", "", 16);
    s.back();
    std::string out;
    CHECK(!s.active(), "B on an empty text closes the keyboard");
    CHECK(s.poll(&out) && out.empty(), "cancelled: the initial text comes back");
    s.open("Nombre", "ab", 8);
    s.back();
    s.back();
    CHECK(s.active(), "still open after deleting the last character");
    s.back();
    CHECK(s.poll(&out) && out == "ab", "B on the emptied text cancels with the initial text");
}

static void b_on_text_deletes_one() {
    OskSession s;
    s.open("Nombre", "abc", 8);
    s.back();
    CHECK(s.active(), "still open");
    CHECK(s.view().text == "ab", "one character deleted");
}

int main() {
    b_on_empty_text_cancels();
    b_on_text_deletes_one();
    confirm_returns_typed_text();
    cancel_returns_initial_text();
    view_shows_title_and_text();
    input_while_idle_is_ignored();
    cursor_editing();
    if (failures == 0) std::printf("test_osk_session: all passed\n");
    return failures == 0 ? 0 : 1;
}
