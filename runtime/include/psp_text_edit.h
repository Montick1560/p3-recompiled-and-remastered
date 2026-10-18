#pragma once
// One-line text editor for the on-screen keyboard (pure). Text is held as
// Unicode code points (BMP, like the PSP's UCS-2 fields) so the limit counts
// characters, not bytes; input and output are UTF-8 (SDL_TEXTINPUT). Insert
// and backspace work at the cursor.
#include <cstdint>
#include <string>
#include <vector>

class TextEdit {
public:
    // Start from `initial` (cut to `max_len` characters).
    void reset(const std::string& initial, uint32_t max_len);
    // Insert typed text at the cursor; control characters are skipped, and
    // input past the limit is dropped.
    void insert(const std::string& utf8);
    void backspace();
    void move_cursor(int delta);
    std::string utf8() const;
    uint32_t length() const { return static_cast<uint32_t>(chars_.size()); }
    uint32_t max_len() const { return max_len_; }
    uint32_t cursor() const { return static_cast<uint32_t>(cursor_); }

private:
    std::vector<char32_t> chars_;
    uint32_t max_len_ = 0;
    size_t cursor_ = 0;
};
