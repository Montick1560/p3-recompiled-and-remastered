#pragma once
// Steam Deck style on-screen keyboard (pure): a key grid with two layers (letters; numbers and
// symbols), shift (one letter) / caps (double shift), an accent key (´ á é í ó ú, ¨ ä ë ï ö ü) and
// the special keys Backspace, Space, cursor ← →, Accept and Cancel. Gamepad actions in; edits out
// through an OskTarget (the OskSession in the runtime, a fake in the tests). Layouts: "es" (QWERTY +
// ñ) and "en". Main thread only.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct OskTarget {
    virtual ~OskTarget() = default;
    virtual void type(const std::string& utf8) = 0;
    virtual void backspace() = 0;
    virtual void move_cursor(int delta) = 0;
    virtual void accept() = 0;
    virtual void cancel() = 0;
};

enum class OskKeyKind { Char, Shift, Backspace, Space, Layer, Accent, Left, Right, Accept, Cancel };

struct OskKey {
    OskKeyKind kind;
    std::string label;  // what the key shows (follows shift / caps for letters)
    std::string text;   // Char: what it types (lower case); Accent: "´" or "¨"
    float width;        // in key units (a letter is 1)
};

enum class OskAction { Up, Down, Left, Right, Press, Backspace, Shift, Space, CursorLeft, CursorRight,
                       Layer, Accept, Cancel };

class OskKeyboard {
public:
    explicit OskKeyboard(const std::string& lang = "es");
    void act(OskAction a, OskTarget& t);
    const std::vector<std::vector<OskKey>>& rows() const { return view_; }
    int focus_row() const { return row_; }
    int focus_col() const { return col_; }
    bool shift() const { return shift_; }
    bool caps() const { return caps_; }
    bool symbols() const { return symbols_; }
    const std::string& pending_accent() const { return accent_; }

private:
    void rebuild();                       // view_ from the layer + case
    void press(const OskKey& k, OskTarget& t);
    void type_char(const std::string& c, OskTarget& t);
    void move_vertical(int dir);
    std::vector<std::vector<OskKey>> letters_, symbols_rows_, view_;
    bool en_ = false, shift_ = false, caps_ = false, symbols_ = false;
    std::string accent_;
    int row_ = 1, col_ = 0;
};

// Left-stick navigation with key repeat (SDL axes: -y is up), like the launcher's StickRepeat.
class OskStickRepeat {
public:
    std::optional<OskAction> update(float x, float y, uint64_t now_ms);

private:
    std::optional<OskAction> dir_;
    uint64_t next_ms_ = 0;
};
