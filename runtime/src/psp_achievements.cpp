#include "psp_achievements.h"

#include "json.hpp"

#include <algorithm>

namespace {

constexpr uint32_t kUserLo = 0x08800000u, kUserHi = 0x0A000000u;
constexpr size_t kMaxEntries = 255;

uint32_t rd(const uint8_t* rdram, uint32_t addr, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; i++) v |= static_cast<uint32_t>(rdram[(addr + i) & 0x07FFFFFFu]) << (8 * i);
    return v;
}

int fact_index(const PspAchievementSource& src, const std::string& name) {
    for (size_t i = 0; i < src.fact_count; i++)
        if (name == src.facts[i].name) return static_cast<int>(i);
    return -1;
}

bool parse_leaf(const nlohmann::ordered_json& j, const PspAchievementSource& src, PspAchLeaf* out, std::string* why) {
    if (!j.is_object() || !j.contains("fact") || !j["fact"].is_string()) { *why = "a leaf without fact"; return false; }
    const std::string name = j["fact"].get<std::string>();
    const int f = fact_index(src, name);
    if (f < 0) { *why = "unknown fact " + name; return false; }
    const std::string op = j.value("op", "");
    if (op == ">=") out->op = PspAchOp::Ge;
    else if (op == "==") out->op = PspAchOp::Eq;
    else if (op == "!=") out->op = PspAchOp::Ne;
    else { *why = "bad op \"" + op + "\""; return false; }
    if (!j.contains("value") || !j["value"].is_number_integer()) { *why = "value is not an integer"; return false; }
    out->fact = static_cast<uint16_t>(f);
    out->value = j["value"].get<int64_t>();
    return true;
}

// "#RRGGBB" -> 0xRRGGBB; false for anything else.
bool parse_rgb(const nlohmann::ordered_json& j, uint32_t* out) {
    if (!j.is_string()) return false;
    const std::string s = j.get<std::string>();
    if (s.size() != 7 || s[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; i++) {
        const char ch = s[i];
        const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                    : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | static_cast<uint32_t>(d);
    }
    *out = v;
    return true;
}

void parse_rarities(const nlohmann::ordered_json& j, PspAchCatalogue& c) {
    if (j.contains("rarities") && j["rarities"].is_object())
        for (const auto& [key, t] : j["rarities"].items()) {
            PspAchRarity r{key, "", "", 0, 0};
            if (!t.is_object() || !parse_rgb(t.value("color", nlohmann::ordered_json()), &r.rgb)) {
                c.log.push_back("[ACH] rarity " + key + ": color is not #RRGGBB; left out");
                continue;
            }
            const nlohmann::ordered_json ms = t.value("ms", nlohmann::ordered_json(4000));
            r.ms = std::clamp(ms.is_number_integer() ? ms.get<int>() : 4000, 2000, 10000);
            if (t.contains("name") && t["name"].is_object()) {
                r.name_es = t["name"].value("es", key);
                r.name_en = t["name"].value("en", key);
            }
            c.rarities.push_back(r);
        }
    if (c.rarities.empty()) c.rarities.push_back({"comun", "Común", "Common", 0x231815u, 4000});
}

}  // namespace

const PspAchRarity& psp_ach_rarity(const PspAchCatalogue& c, const std::string& key) {
    for (const PspAchRarity& r : c.rarities)
        if (r.key == key) return r;
    return c.rarities.front();
}

int64_t psp_ach_read(const PspFact& f, const uint8_t* rdram, uint32_t base) {
    if (f.count > 0) {
        const uint32_t step = f.stride ? f.stride : f.size;
        int64_t n = 0;
        for (uint32_t i = 0; i < f.count; i++) n += rd(rdram, base + f.offset + i * step, f.size) != 0;
        return n;
    }
    const uint32_t raw = rd(rdram, base + f.offset, f.size);
    if (f.bit >= 0) return (raw >> f.bit) & 1u;
    if (!f.is_signed) return raw;
    const int shift = 32 - 8 * f.size;
    return static_cast<int32_t>(raw << shift) >> shift;
}

PspAchCatalogue psp_ach_compile(const std::string& json, const PspAchievementSource& src,
                                const std::vector<std::string>& earned) {
    PspAchCatalogue c;
    const nlohmann::ordered_json j = nlohmann::ordered_json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("version", 0) != 1 || !j.contains("achievements") ||
        !j["achievements"].is_array()) {
        c.log.push_back("[ACH] achievements.json is not a version 1 catalogue: achievements off");
        parse_rarities(nlohmann::ordered_json::object(), c);
        return c;
    }
    parse_rarities(j, c);
    for (const auto& a : j["achievements"]) {
        if (c.entries.size() >= kMaxEntries) { c.log.push_back("[ACH] more than 255 achievements: the rest ignored"); break; }
        if (!a.is_object() || !a.contains("id") || !a["id"].is_string()) continue;
        PspAchEntry e;
        e.id = a["id"].get<std::string>();
        e.icon = a.value("icon", "");
        e.hidden = a.value("hidden", false);
        if (a.contains("name") && a["name"].is_object()) {
            e.name_es = a["name"].value("es", "");
            e.name_en = a["name"].value("en", "");
        }
        if (a.contains("desc") && a["desc"].is_object()) {
            e.desc_es = a["desc"].value("es", "");
            e.desc_en = a["desc"].value("en", "");
        }
        const nlohmann::ordered_json rarity = a.value("rarity", nlohmann::ordered_json());
        e.rarity = psp_ach_rarity(c, rarity.is_string() ? rarity.get<std::string>() : "").key;
        e.has_when = a.contains("when");
        const uint16_t index = static_cast<uint16_t>(c.entries.size());
        c.entries.push_back(e);
        if (!e.has_when || std::find(earned.begin(), earned.end(), e.id) != earned.end()) continue;
        const nlohmann::ordered_json& w = a["when"];
        std::vector<PspAchLeaf> leaves;
        std::string why;
        bool ok = true, any = false;
        if (w.is_object() && (w.contains("all") || w.contains("any"))) {
            any = w.contains("any");
            const nlohmann::ordered_json& list = any ? w["any"] : w["all"];
            if (!list.is_array() || list.empty()) { ok = false; why = "empty all/any"; }
            for (size_t i = 0; ok && i < list.size(); i++) {
                const nlohmann::ordered_json& l = list[i];
                if (l.is_object() && (l.contains("all") || l.contains("any"))) { ok = false; why = "nested all/any"; break; }
                PspAchLeaf leaf{};
                ok = parse_leaf(l, src, &leaf, &why);
                if (ok) leaves.push_back(leaf);
            }
        } else {
            PspAchLeaf leaf{};
            ok = parse_leaf(w, src, &leaf, &why);
            if (ok) leaves.push_back(leaf);
        }
        if (!ok) { c.log.push_back("[ACH] " + e.id + ": " + why + "; left out"); continue; }
        c.checks.pending.push_back({index, any, 0, static_cast<uint16_t>(c.checks.leaves.size()),
                                    static_cast<uint16_t>(leaves.size())});
        c.checks.leaves.insert(c.checks.leaves.end(), leaves.begin(), leaves.end());
    }
    for (const PspAchLeaf& l : c.checks.leaves)
        if (std::find(c.checks.used.begin(), c.checks.used.end(), l.fact) == c.checks.used.end())
            c.checks.used.push_back(l.fact);
    c.checks.values.assign(src.fact_count, 0);
    return c;
}

size_t psp_ach_evaluate(PspAchChecks& k, const PspAchievementSource& src, const uint8_t* rdram,
                        uint16_t* granted, size_t max) {
    if (k.pending.empty()) return 0;
    const uint32_t base = src.base(rdram);
    bool valid = base >= kUserLo && base < kUserHi && src.size <= kUserHi - base && src.sane(rdram, base);
    for (size_t i = 0; valid && i < k.used.size(); i++) {
        const PspFact& f = src.facts[k.used[i]];
        const int64_t v = psp_ach_read(f, rdram, base);
        if (v < f.min || v > f.max) valid = false;
        k.values[k.used[i]] = v;
    }
    if (!valid) {
        for (PspAchPending& p : k.pending) p.streak = 0;
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < k.pending.size();) {
        PspAchPending& p = k.pending[i];
        bool hold = !p.any;
        for (uint16_t l = p.first; l < p.first + p.n; l++) {
            const PspAchLeaf& leaf = k.leaves[l];
            const int64_t v = k.values[leaf.fact];
            const bool t = leaf.op == PspAchOp::Ge ? v >= leaf.value : leaf.op == PspAchOp::Eq ? v == leaf.value
                                                                                                 : v != leaf.value;
            if (p.any && t) { hold = true; break; }
            if (!p.any && !t) { hold = false; break; }
        }
        if (!hold) { p.streak = 0; i++; continue; }
        if (p.streak < 2) p.streak++;
        if (p.streak < 2 || n == max) { i++; continue; }
        granted[n++] = p.index;
        k.pending[i] = k.pending.back();  // the moved entry has not been looked at in this check
        k.pending.pop_back();
    }
    return n;
}
