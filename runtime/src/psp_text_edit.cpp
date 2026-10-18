#include "psp_text_edit.h"

namespace {

// UTF-8 -> code points (BMP only, like the PSP fields); invalid bytes skipped.
std::vector<char32_t> decode(const std::string& s) {
    std::vector<char32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        size_t len = b < 0x80 ? 1 : (b >= 0xC2 && b <= 0xDF) ? 2 : (b >= 0xE0 && b <= 0xEF) ? 3
                   : (b >= 0xF0 && b <= 0xF4) ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            i++;
            continue;
        }
        char32_t cp = len == 1 ? b : (b & (0xFF >> (len + 1)));
        bool ok = true;
        for (size_t j = 1; j < len; j++) {
            const unsigned char c = static_cast<unsigned char>(s[i + j]);
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        i += ok ? len : 1;
        if (ok && cp <= 0xFFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) out.push_back(cp);
    }
    return out;
}

bool printable(char32_t c) { return c >= 0x20 && c != 0x7F; }

}  // namespace

void TextEdit::reset(const std::string& initial, uint32_t max_len) {
    max_len_ = max_len;
    chars_.clear();
    cursor_ = 0;
    insert(initial);
}

void TextEdit::insert(const std::string& utf8) {
    for (char32_t c : decode(utf8)) {
        if (!printable(c)) continue;
        if (chars_.size() >= max_len_) break;
        chars_.insert(chars_.begin() + cursor_, c);
        cursor_++;
    }
}

void TextEdit::backspace() {
    if (cursor_ == 0) return;
    chars_.erase(chars_.begin() + (cursor_ - 1));
    cursor_--;
}

void TextEdit::move_cursor(int delta) {
    const long long target = static_cast<long long>(cursor_) + delta;
    if (target < 0) cursor_ = 0;
    else if (target > static_cast<long long>(chars_.size())) cursor_ = chars_.size();
    else cursor_ = static_cast<size_t>(target);
}

std::string TextEdit::utf8() const {
    std::string out;
    for (char32_t c : chars_) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}
