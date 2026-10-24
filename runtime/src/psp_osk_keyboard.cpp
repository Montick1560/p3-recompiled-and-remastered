#include "psp_osk_keyboard.h"

#include <cmath>

namespace {

OskKey ch(const char* s) { return {OskKeyKind::Char, s, s, 1.0f}; }

// Upper case of the letters the layouts hold (ASCII, ñ, accented vowels).
std::string upper(const std::string& s) {
    static const char* const pairs[][2] = {{"ñ", "Ñ"}, {"á", "Á"}, {"é", "É"}, {"í", "Í"}, {"ó", "Ó"},
                                           {"ú", "Ú"}, {"ä", "Ä"}, {"ë", "Ë"}, {"ï", "Ï"}, {"ö", "Ö"},
                                           {"ü", "Ü"}};
    for (const auto& p : pairs) if (s == p[0]) return p[1];
    if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') return std::string(1, static_cast<char>(s[0] - 32));
    return s;
}

std::string accented(const std::string& accent, const std::string& vowel) {
    static const char* const acute[][2] = {{"a", "á"}, {"e", "é"}, {"i", "í"}, {"o", "ó"}, {"u", "ú"}};
    static const char* const dia[][2] = {{"a", "ä"}, {"e", "ë"}, {"i", "ï"}, {"o", "ö"}, {"u", "ü"}};
    for (const auto& p : accent == "´" ? acute : dia) if (vowel == p[0]) return p[1];
    return "";
}

}  // namespace

OskKeyboard::OskKeyboard(const std::string& lang) : en_(lang == "en") {
    const std::string space = en_ ? "space" : "espacio", ok = en_ ? "OK" : "Aceptar",
                      cancel = en_ ? "Cancel" : "Cancelar";
    letters_ = {
        {ch("1"), ch("2"), ch("3"), ch("4"), ch("5"), ch("6"), ch("7"), ch("8"), ch("9"), ch("0"),
         {OskKeyKind::Backspace, "⌫", "", 1.5f}},
        {ch("q"), ch("w"), ch("e"), ch("r"), ch("t"), ch("y"), ch("u"), ch("i"), ch("o"), ch("p"),
         en_ ? ch("-") : OskKey{OskKeyKind::Accent, "´", "´", 1.0f}},
        {ch("a"), ch("s"), ch("d"), ch("f"), ch("g"), ch("h"), ch("j"), ch("k"), ch("l"),
         en_ ? ch("'") : ch("ñ"), {OskKeyKind::Accept, ok, "", 1.5f}},
        {{OskKeyKind::Shift, "⇧", "", 1.5f}, ch("z"), ch("x"), ch("c"), ch("v"), ch("b"), ch("n"),
         ch("m"), ch(","), ch("."), {OskKeyKind::Shift, "⇧", "", 1.0f}},
        {{OskKeyKind::Layer, "?123", "", 1.5f}, {OskKeyKind::Left, "←", "", 1.0f},
         {OskKeyKind::Space, space, " ", 5.0f}, {OskKeyKind::Right, "→", "", 1.0f},
         {OskKeyKind::Cancel, cancel, "", 2.0f}},
    };
    symbols_rows_ = {
        {ch("1"), ch("2"), ch("3"), ch("4"), ch("5"), ch("6"), ch("7"), ch("8"), ch("9"), ch("0"),
         {OskKeyKind::Backspace, "⌫", "", 1.5f}},
        {ch("@"), ch("#"), ch("€"), ch("%"), ch("&"), ch("*"), ch("("), ch(")"), ch("-"), ch("_"),
         {OskKeyKind::Accent, "¨", "¨", 1.0f}},
        {ch("¿"), ch("?"), ch("¡"), ch("!"), ch("'"), ch("\""), ch(":"), ch(";"), ch("/"), ch("+"),
         {OskKeyKind::Accept, ok, "", 1.5f}},
        {{OskKeyKind::Shift, "⇧", "", 1.5f}, ch("~"), ch("="), ch("["), ch("]"), ch("<"), ch(">"),
         ch("{"), ch("}"), ch(","), {OskKeyKind::Shift, "⇧", "", 1.0f}},
        {{OskKeyKind::Layer, "ABC", "", 1.5f}, {OskKeyKind::Left, "←", "", 1.0f},
         {OskKeyKind::Space, space, " ", 5.0f}, {OskKeyKind::Right, "→", "", 1.0f},
         {OskKeyKind::Cancel, cancel, "", 2.0f}},
    };
    rebuild();
}

void OskKeyboard::rebuild() {
    view_ = symbols_ ? symbols_rows_ : letters_;
    if (shift_ || caps_) {
        for (auto& r : view_) for (auto& k : r) if (k.kind == OskKeyKind::Char) k.label = upper(k.text);
    }
    if (row_ >= static_cast<int>(view_.size())) row_ = 0;
    if (col_ >= static_cast<int>(view_[row_].size())) col_ = static_cast<int>(view_[row_].size()) - 1;
}

void OskKeyboard::type_char(const std::string& c, OskTarget& t) {
    std::string out = (shift_ || caps_) ? upper(c) : c;
    if (!accent_.empty()) {
        const std::string a = accented(accent_, c);
        if (a.empty()) out = accent_ + out;
        else out = (shift_ || caps_) ? upper(a) : a;
        accent_.clear();
    }
    t.type(out);
    if (shift_ && !caps_) {
        shift_ = false;
        rebuild();
    }
}

void OskKeyboard::press(const OskKey& k, OskTarget& t) {
    switch (k.kind) {
    case OskKeyKind::Char: type_char(k.text, t); break;
    case OskKeyKind::Accent:
        if (accent_ == k.text) { t.type(accent_); accent_.clear(); }
        else accent_ = k.text;
        break;
    case OskKeyKind::Shift: act(OskAction::Shift, t); break;
    case OskKeyKind::Backspace: act(OskAction::Backspace, t); break;
    case OskKeyKind::Space: act(OskAction::Space, t); break;
    case OskKeyKind::Layer: act(OskAction::Layer, t); break;
    case OskKeyKind::Left: t.move_cursor(-1); break;
    case OskKeyKind::Right: t.move_cursor(1); break;
    case OskKeyKind::Accept: t.accept(); break;
    case OskKeyKind::Cancel: t.cancel(); break;
    }
}

// Key centre in key units from the row start.
static float centre(const std::vector<OskKey>& r, int col) {
    float x = 0;
    for (int i = 0; i < col; i++) x += r[i].width;
    return x + r[col].width * 0.5f;
}

void OskKeyboard::move_vertical(int dir) {
    const float c = centre(view_[row_], col_);
    const int n = static_cast<int>(view_.size());
    row_ = (row_ + dir + n) % n;
    int best = 0;
    float best_d = 1e9f;
    for (int i = 0; i < static_cast<int>(view_[row_].size()); i++) {
        const float d = std::fabs(centre(view_[row_], i) - c);
        if (d < best_d) { best_d = d; best = i; }
    }
    col_ = best;
}

void OskKeyboard::act(OskAction a, OskTarget& t) {
    const int n = static_cast<int>(view_[row_].size());
    switch (a) {
    case OskAction::Up: move_vertical(-1); break;
    case OskAction::Down: move_vertical(1); break;
    case OskAction::Left: col_ = (col_ - 1 + n) % n; break;
    case OskAction::Right: col_ = (col_ + 1) % n; break;
    case OskAction::Press: press(view_[row_][col_], t); break;
    case OskAction::Backspace:
        if (!accent_.empty()) accent_.clear();
        else t.backspace();
        break;
    case OskAction::Back:
        if (!accent_.empty()) accent_.clear();
        else t.back();
        break;
    case OskAction::Shift:
        if (caps_) { caps_ = false; shift_ = false; }
        else if (shift_) { caps_ = true; shift_ = false; }
        else shift_ = true;
        rebuild();
        break;
    case OskAction::Space:
        if (!accent_.empty()) { t.type(accent_); accent_.clear(); }
        t.type(" ");
        break;
    case OskAction::CursorLeft: t.move_cursor(-1); break;
    case OskAction::CursorRight: t.move_cursor(1); break;
    case OskAction::Layer: symbols_ = !symbols_; rebuild(); break;
    case OskAction::Accept: t.accept(); break;
    case OskAction::Cancel: t.cancel(); break;
    }
}

std::optional<OskAction> OskStickRepeat::update(float x, float y, uint64_t now_ms) {
    std::optional<OskAction> d;
    if (y < -0.6f) d = OskAction::Up;
    else if (y > 0.6f) d = OskAction::Down;
    else if (x < -0.6f) d = OskAction::Left;
    else if (x > 0.6f) d = OskAction::Right;
    if (!d) { dir_.reset(); return std::nullopt; }
    if (dir_ != d) { dir_ = d; next_ms_ = now_ms + 400; return d; }
    if (now_ms >= next_ms_) { next_ms_ = now_ms + 120; return d; }
    return std::nullopt;
}
