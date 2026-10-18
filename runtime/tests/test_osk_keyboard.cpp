// Unit tests for psp_osk_keyboard: the Steam Deck style key grid, navigation, layers, shift / caps,
// accents and the edits it sends to its target.
#include "psp_osk_keyboard.h"
#include "psp_osk_skin.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

struct Fake : OskTarget {
    std::string text;
    int cursor_moves = 0, accepted = 0, cancelled = 0;
    void type(const std::string& s) override { text += s; }
    void backspace() override {
        if (text.empty()) return;
        size_t i = text.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i--;
        text.erase(i);
    }
    void move_cursor(int d) override { cursor_moves += d; }
    void accept() override { accepted++; }
    void cancel() override { cancelled++; }
};

static const OskKey& focused(const OskKeyboard& k) { return k.rows()[k.focus_row()][k.focus_col()]; }

// Moves the focus to the key labelled `label` on the current layer (test helper, uses only actions).
static bool go_to(OskKeyboard& k, Fake& f, const std::string& label) {
    for (int guard = 0; guard < 200; guard++) {
        if (focused(k).label == label) return true;
        k.act(OskAction::Right, f);
        if (k.focus_col() == 0) k.act(OskAction::Down, f);
    }
    return false;
}

static void starts_on_q_and_types() {
    OskKeyboard k("es");
    Fake f;
    CHECK(focused(k).label == "q", "focus starts on q");
    k.act(OskAction::Press, f);
    CHECK(f.text == "q", "Press types the focused key");
    CHECK(go_to(k, f, "ñ"), "es layout has ñ");
    k.act(OskAction::Press, f);
    CHECK(f.text == "qñ", "ñ typed as UTF-8");
}

static void navigation_wraps_and_snaps() {
    OskKeyboard k("es");
    Fake f;
    k.act(OskAction::Left, f);
    CHECK(k.focus_row() == 1 && k.focus_col() == static_cast<int>(k.rows()[1].size()) - 1, "left wraps in the row");
    k.act(OskAction::Up, f);
    CHECK(k.focus_row() == 0, "up to the number row");
    k.act(OskAction::Up, f);
    CHECK(k.focus_row() == static_cast<int>(k.rows().size()) - 1, "up wraps to the bottom row");
    CHECK(focused(k).kind == OskKeyKind::Cancel || focused(k).kind == OskKeyKind::Right ||
              focused(k).kind == OskKeyKind::Space, "snapped to the bottom row key under it");
}

static void shift_then_caps() {
    OskKeyboard k("en");
    Fake f;
    k.act(OskAction::Shift, f);
    CHECK(k.shift() && !k.caps() && focused(k).label == "Q", "shift shows upper case");
    k.act(OskAction::Press, f);
    k.act(OskAction::Press, f);
    CHECK(f.text == "Qq" && !k.shift(), "shift is one letter");
    k.act(OskAction::Shift, f);
    k.act(OskAction::Shift, f);
    CHECK(k.caps(), "double shift = caps");
    k.act(OskAction::Press, f);
    k.act(OskAction::Press, f);
    CHECK(f.text == "QqQQ" && k.caps(), "caps stays");
    k.act(OskAction::Shift, f);
    CHECK(!k.caps() && !k.shift(), "third shift turns it off");
}

static void accents() {
    OskKeyboard k("es");
    Fake f;
    CHECK(go_to(k, f, "´"), "accent key on the letters layer");
    k.act(OskAction::Press, f);
    CHECK(k.pending_accent() == "´" && f.text.empty(), "accent waits for a vowel");
    CHECK(go_to(k, f, "e"), "find e");
    k.act(OskAction::Press, f);
    CHECK(f.text == "é" && k.pending_accent().empty(), "´ + e = é");
    go_to(k, f, "´");
    k.act(OskAction::Press, f);
    k.act(OskAction::Shift, f);
    go_to(k, f, "O");
    k.act(OskAction::Press, f);
    CHECK(f.text == "éÓ", "´ + shift o = Ó");
    go_to(k, f, "´");
    k.act(OskAction::Press, f);
    go_to(k, f, "t");
    k.act(OskAction::Press, f);
    CHECK(f.text == "éÓ´t", "´ + consonant types both");
    k.act(OskAction::Layer, f);
    CHECK(k.symbols() && go_to(k, f, "¨"), "diaeresis on the symbols layer");
    k.act(OskAction::Press, f);
    k.act(OskAction::Layer, f);
    go_to(k, f, "u");
    k.act(OskAction::Press, f);
    CHECK(f.text == "éÓ´tü", "¨ + u = ü");
}

static void symbols_and_special_keys() {
    OskKeyboard k("es");
    Fake f;
    k.act(OskAction::Layer, f);
    CHECK(go_to(k, f, "¿"), "¿ on the symbols layer");
    k.act(OskAction::Press, f);
    k.act(OskAction::Space, f);
    k.act(OskAction::Backspace, f);
    CHECK(f.text == "¿", "space then backspace");
    k.act(OskAction::CursorLeft, f);
    k.act(OskAction::CursorRight, f);
    k.act(OskAction::CursorRight, f);
    CHECK(f.cursor_moves == 1, "cursor actions reach the target");
    k.act(OskAction::Accept, f);
    CHECK(f.accepted == 1, "Start accepts");
    k.act(OskAction::Layer, f);
    CHECK(!k.symbols(), "layer toggles back");
    CHECK(go_to(k, f, "Cancelar"), "cancel key (es label)");
    k.act(OskAction::Press, f);
    CHECK(f.cancelled == 1, "cancel key cancels");
    OskKeyboard en("en");
    Fake g;
    CHECK(go_to(en, g, "Cancel"), "cancel key (en label)");
}

static void stick_repeat() {
    OskStickRepeat s;
    CHECK(s.update(0.0f, -0.9f, 0) == OskAction::Up, "first push moves (SDL: -y is up)");
    CHECK(!s.update(0.0f, -0.9f, 300).has_value(), "no repeat before 400 ms");
    CHECK(s.update(0.0f, -0.9f, 400) == OskAction::Up, "repeat");
    CHECK(s.update(0.0f, -0.9f, 520) == OskAction::Up, "then every 120 ms");
    CHECK(!s.update(0.0f, 0.1f, 530).has_value(), "centre");
    CHECK(s.update(0.8f, 0.0f, 600) == OskAction::Right, "right");
}

static void skins() {
    OskColor c{};
    CHECK(osk_parse_color("#1A9FFF", &c) && std::fabs(c.b - 1.0f) < 1e-6f && std::fabs(c.a - 1.0f) < 1e-6f, "#RRGGBB");
    CHECK(osk_parse_color("#00000080", &c) && std::fabs(c.a - 128 / 255.0f) < 1e-6f, "#RRGGBBAA");
    CHECK(!osk_parse_color("1A9FFF", &c) && !osk_parse_color("#12345", &c) && !osk_parse_color("#GGGGGG", &c), "bad colours");
    std::vector<std::string> log;
    OskSkin s = osk_skin_parse(R"({"name":"Patapon","key":"#F5F0E6","radius":4,"gap":6,"font":"f.ttf"})", "C:/s", &log);
    CHECK(s.name == "Patapon" && std::fabs(s.key.r - 0xF5 / 255.0f) < 1e-6f && s.radius == 4 && s.gap == 6, "fields read");
    CHECK(s.font == "C:/s/f.ttf", "font relative to the skin folder");
    CHECK(log.empty(), "no log for a good skin");
    const OskSkin d;
    s = osk_skin_parse(R"({"key":"red","radius":-3,"opacity":7})", "C:/s", &log);
    CHECK(std::fabs(s.key.r - d.key.r) < 1e-6f && s.radius == d.radius && s.opacity == d.opacity, "bad fields keep defaults");
    CHECK(log.size() == 3, "one log line per bad field");
    log.clear();
    s = osk_skin_parse("{not json", "C:/s", &log);
    CHECK(s.name == d.name && log.size() == 1, "invalid JSON -> built-in Deck skin");
}

int main() {
    starts_on_q_and_types();
    navigation_wraps_and_snaps();
    shift_then_caps();
    accents();
    symbols_and_special_keys();
    stick_repeat();
    skins();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
