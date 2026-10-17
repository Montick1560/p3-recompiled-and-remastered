#include "psp_ini.h"

#include <algorithm>
#include <cctype>

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
}  // namespace

const std::string* IniDoc::get(const std::string& section, const std::string& key) const {
    auto s = values.find(section);
    if (s == values.end()) return nullptr;
    auto k = s->second.find(key);
    return k == s->second.end() ? nullptr : &k->second;
}

IniDoc psp_ini_parse(const std::string& text) {
    IniDoc d;
    std::string body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
        body.erase(0, 3);
    std::string section;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        const std::string line = trim(body.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            d.values[section];
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            d.warnings.push_back("ignored line \"" + line + "\"");
            continue;
        }
        d.values[section][lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }
    return d;
}
