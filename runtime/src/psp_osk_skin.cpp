#include "psp_osk_skin.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

bool osk_parse_color(const std::string& s, OskColor* out) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    for (size_t i = 1; i < s.size(); i++) if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    auto byte = [&](size_t i) { return std::stoi(s.substr(i, 2), nullptr, 16) / 255.0f; };
    *out = {byte(1), byte(3), byte(5), s.size() == 9 ? byte(7) : 1.0f};
    return true;
}

OskSkin osk_skin_parse(const std::string& json, const std::string& skin_dir, std::vector<std::string>* log) {
    OskSkin s;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        log->push_back("[OSK] skin is not valid JSON: using the built-in Deck skin");
        return s;
    }
    if (j.contains("name") && j["name"].is_string()) s.name = j["name"].get<std::string>();
    const std::pair<const char*, OskColor*> colours[] = {
        {"background", &s.background}, {"panel", &s.panel}, {"key", &s.key}, {"key_text", &s.key_text},
        {"focus", &s.focus}, {"focus_text", &s.focus_text}, {"accent", &s.accent}};
    for (const auto& [k, dst] : colours) {
        if (!j.contains(k)) continue;
        OskColor c{};
        if (j[k].is_string() && osk_parse_color(j[k].get<std::string>(), &c)) *dst = c;
        else log->push_back(std::string("[OSK] skin ") + k + ": not a #RRGGBB(AA) colour, default kept");
    }
    const std::tuple<const char*, float*, float, float> nums[] = {
        {"radius", &s.radius, 0.0f, 40.0f}, {"gap", &s.gap, 0.0f, 40.0f}, {"opacity", &s.opacity, 0.1f, 1.0f}};
    for (const auto& [k, dst, lo, hi] : nums) {
        if (!j.contains(k)) continue;
        if (j[k].is_number() && j[k].get<float>() >= lo && j[k].get<float>() <= hi) *dst = j[k].get<float>();
        else log->push_back(std::string("[OSK] skin ") + k + ": out of range, default kept");
    }
    if (j.contains("font") && j["font"].is_string() && !j["font"].get<std::string>().empty()) {
        const std::string f = j["font"].get<std::string>();
        const bool abs = f[0] == '/' || f[0] == '\\' || (f.size() > 1 && f[1] == ':');
        s.font = abs || skin_dir.empty() ? f : skin_dir + "/" + f;
    }
    return s;
}

std::string psp_ui_font_path(const std::string& exe_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> ttf;
    for (fs::directory_iterator it(fs::u8path(exe_dir) / "assets" / "fonts", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".ttf") ttf.push_back(it->path());
    if (ttf.empty()) return "";
    return std::min_element(ttf.begin(), ttf.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename() < b.filename();
    })->u8string();
}

bool psp_glyph_parts(int cp, GlyphParts* out) {
    if (cp == 0xA1 || cp == 0xBF) {
        *out = {cp == 0xA1 ? '!' : '?', 0, true};
        return true;
    }
    // Latin-1 letters by row: the accent of each column, then the base letters in code point order.
    static const struct { int first; const char* bases; int mark; } kRows[] = {
        {0xC0, "AAAAA", 0}, {0xC8, "EEEE", 0}, {0xCC, "IIII", 0}, {0xD2, "OOOOO", 0}, {0xD9, "UUUU", 0},
        {0xE0, "aaaaa", 0}, {0xE8, "eeee", 0}, {0xEC, "iiii", 0}, {0xF2, "ooooo", 0}, {0xF9, "uuuu", 0}};
    static const int kAccents[] = {'`', 0xB4, '^', '~', 0xA8};  // grave, acute, circumflex, tilde, diaeresis
    for (const auto& r : kRows) {
        const int n = static_cast<int>(std::strlen(r.bases));
        if (cp < r.first || cp >= r.first + n) continue;
        const int col = cp - r.first;
        // E/I/U rows have no tilde: their 4th column is the diaeresis.
        const int mark = n == 4 && col == 3 ? 0xA8 : kAccents[col];
        *out = {r.bases[col], mark, false};
        return true;
    }
    if (cp == 0xD1 || cp == 0xF1) { *out = {cp == 0xD1 ? 'N' : 'n', '~', false}; return true; }
    if (cp == 0xFF) { *out = {'y', 0xA8, false}; return true; }
    return false;
}
