#include "psp_ach_store.h"

#include "json.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

bool read_text(const std::string& path, std::string* out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream s;
    s << f.rdbuf();
    *out = s.str();
    return true;
}

// Parses the file; state as psp_ach_store_read documents.
nlohmann::json load(const std::string& path, PspStoreState* state) {
    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) { *state = PspStoreState::Missing; return nlohmann::json(); }
    std::string text;
    if (!read_text(path, &text)) { *state = PspStoreState::Corrupt; return nlohmann::json(); }
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { *state = PspStoreState::Corrupt; return nlohmann::json(); }
    if (!j.contains("version") || !j["version"].is_number_integer()) { *state = PspStoreState::Corrupt; return nlohmann::json(); }
    if (j["version"].get<int64_t>() != 1) { *state = PspStoreState::Newer; return nlohmann::json(); }
    if (!j.contains("earned") || !j["earned"].is_object()) { *state = PspStoreState::Corrupt; return nlohmann::json(); }
    *state = PspStoreState::Ok;
    return j;
}

}  // namespace

std::map<std::string, PspEarned> psp_ach_store_read(const std::string& path, PspStoreState* state) {
    std::map<std::string, PspEarned> m;
    const nlohmann::json j = load(path, state);
    if (*state != PspStoreState::Ok) return m;
    for (auto it = j["earned"].begin(); it != j["earned"].end(); ++it) {
        if (!it.value().is_object()) continue;
        const nlohmann::json& e = it.value();
        m[it.key()] = {e.value("date", ""), e.value("save", ""), e.value("hero", ""), e.value("release", "")};
    }
    return m;
}

bool psp_ach_store_add(const std::string& path, const std::vector<std::pair<std::string, PspEarned>>& add,
                       std::string* err) {
    PspStoreState state;
    nlohmann::json j = load(path, &state);
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    if (state == PspStoreState::Newer) { *err = "unlocked.json has a newer version: left as it is"; return false; }
    if (state == PspStoreState::Corrupt) {
        fs::rename(p, p.parent_path() / "unlocked.corrupt.json", ec);
        if (ec) { *err = "cannot keep the corrupt unlocked.json aside: " + ec.message(); return false; }
    }
    if (state != PspStoreState::Ok) j = {{"version", 1}, {"earned", nlohmann::json::object()}};
    for (const auto& [id, e] : add) {
        if (j["earned"].contains(id)) continue;
        j["earned"][id] = {{"date", e.date}, {"save", e.save}, {"hero", e.hero}, {"release", e.release}};
    }
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = fs::u8path(path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { *err = "cannot write " + tmp.u8string(); return false; }
        f << j.dump(2) << "\n";
        if (!f.flush()) { *err = "write failed: " + tmp.u8string(); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { *err = "rename failed: " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

std::string psp_ach_iso_utc(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string psp_utf16_to_utf8(const char16_t* s, size_t max) {
    std::string out;
    auto put = [&out](uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (size_t i = 0; i < max && s[i]; i++) {
        const uint32_t u = s[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < max && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            put(0x10000 + ((u - 0xD800) << 10) + (s[i + 1] - 0xDC00));
            i++;
        } else if (u >= 0xD800 && u < 0xE000) {
            put(0xFFFD);
        } else {
            put(u);
        }
    }
    return out;
}
