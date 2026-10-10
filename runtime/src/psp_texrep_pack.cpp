#include "psp_texrep_pack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_bool(const std::string& v) {
    const std::string l = lower(v);
    return l == "true" || l == "1" || l == "yes" || l == "on";
}

// sscanf("%16llx%8x_%d") semantics: >= 1 field parsed is a valid key.
bool parse_key(const std::string& k, GeTexrepKey* key, int* level) {
    unsigned long long ck = 0;
    unsigned int h = 0;
    int lv = 0;
    if (std::sscanf(k.c_str(), "%16llx%8x_%d", &ck, &h, &lv) < 1) return false;
    *key = {ck, h};
    *level = lv;
    return true;
}

bool parse_u32(const std::string& s, uint32_t* out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(t.c_str(), &end, (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) ? 16 : 10);
    if (end == t.c_str() || *end != '\0') return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, sep)) out.push_back(trim(part));
    return out;
}

bool has_parent_component(const std::string& p) {
    std::string norm = p;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    for (const std::string& c : split(norm, '/')) {
        if (c == "..") return true;
    }
    return false;
}

}  // namespace

bool TexrepPack::load(const std::string& ini_text, const std::vector<std::string>& root_files,
                      std::string* error) {
    *this = TexrepPack{};
    // filename map: key -> level -> file (PPSSPP builds this, then ComputeAliasMap).
    std::map<std::pair<uint64_t, uint32_t>, std::map<int, std::string>> files;
    for (const std::string& name : root_files) {
        const size_t dot = name.rfind('.');
        if (dot == std::string::npos || lower(name.substr(dot)) != ".png") continue;
        const std::string stem = name.substr(0, dot);
        if (!(stem.size() == 24 || (stem.size() >= 26 && stem.size() <= 27 && stem[24] == '_'))) continue;
        GeTexrepKey key{};
        int level = 0;
        if (parse_key(stem, &key, &level)) files[{key.cachekey, key.hash}][level] = name;
    }

    std::string section, hash_type;
    std::istringstream in(ini_text);
    std::string raw;
    while (std::getline(in, raw)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq));
        const std::string v = trim(line.substr(eq + 1));
        if (section == "options") {
            const std::string lk = lower(k);
            if (lk == "hash") hash_type = lower(v);
            else if (lk == "ignoreaddress") ignore_address_ = parse_bool(v);
            else if (lk == "reducehash") reduce_hash_ = parse_bool(v);
        } else if (section == "hashes") {
            GeTexrepKey key{};
            int level = 0;
            if (!parse_key(k, &key, &level)) continue;
            if (has_parent_component(v)) {
                std::fprintf(stderr, "[TEXREP] ignoring path with '..': %s\n", v.c_str());
                continue;
            }
            files[{key.cachekey, key.hash}][level] = v;
        } else if (section == "filtering") {
            GeTexrepKey key{};
            int level = 0;
            if (!parse_key(k, &key, &level)) continue;
            const std::string lv = lower(v);
            if (lv == "nearest") filtering_[key] = TexrepFilter::Nearest;
            else if (lv == "linear") filtering_[key] = TexrepFilter::Linear;
        } else if (section == "hashranges") {
            const auto kp = split(k, ',');
            const auto vp = split(v, ',');
            uint32_t addr = 0, fw = 0, fh = 0, tw = 0, th = 0;
            if (kp.size() != 3 || vp.size() != 2) continue;
            const std::string a = (kp[0].rfind("0x", 0) == 0 || kp[0].rfind("0X", 0) == 0) ? kp[0] : "0x" + kp[0];
            if (!parse_u32(a, &addr) || !parse_u32(kp[1], &fw) || !parse_u32(kp[2], &fh) ||
                !parse_u32(vp[0], &tw) || !parse_u32(vp[1], &th)) continue;
            if (tw > fw || th > fh || tw == 0 || th == 0) continue;
            hashranges_[(static_cast<uint64_t>(addr) << 32) | (static_cast<uint64_t>(fw) << 16) | fh] = {
                static_cast<int>(tw), static_cast<int>(th)};
        } else if (section == "reducehashranges") {
            const auto kp = split(k, ',');
            uint32_t fw = 0, fh = 0;
            if (kp.size() != 2 || !parse_u32(kp[0], &fw) || !parse_u32(kp[1], &fh)) continue;
            const float r = std::strtof(v.c_str(), nullptr);
            if (r == 0.0f) continue;
            reduceranges_[(static_cast<uint64_t>(fw) << 16) | fh] = r;
        }
    }

    if (hash_type.empty()) {
        *error = "textures.ini: hash type not specified";
        return false;
    }
    if (hash_type == "xxh32") {
        xxh32_ = true;
    } else if (hash_type != "xxh64") {
        *error = "textures.ini: unsupported hash type '" + hash_type + "' (xxh64/xxh32 only)";
        return false;
    }

    // ComputeAliasMap, level 0 only (packs use ignoreMipmap; mips are out of scope).
    // No level-0 file = PPSSPP's empty alias = "ignored" (keep the original).
    for (const auto& [k, levels] : files) {
        const auto it = levels.find(0);
        std::string path = it == levels.end() ? std::string() : it->second;
        std::replace(path.begin(), path.end(), '\\', '/');
        aliases_[{k.first, k.second}] = path;
    }
    return true;
}

float TexrepPack::reduce_for(int w, int h) const {
    const auto it = reduceranges_.find((static_cast<uint64_t>(w) << 16) | static_cast<uint64_t>(h));
    return it != reduceranges_.end() ? it->second : 0.5f;
}

bool TexrepPack::hash_range(uint32_t addr, int w, int h, int* nw, int* nh) const {
    const auto it = hashranges_.find((static_cast<uint64_t>(addr) << 32) |
                                     (static_cast<uint64_t>(w) << 16) | static_cast<uint64_t>(h));
    if (it == hashranges_.end()) {
        *nw = w;
        *nh = h;
        return false;
    }
    *nw = it->second.first;
    *nh = it->second.second;
    return true;
}

template <typename V>
const V* TexrepPack::wildcard(const std::unordered_map<GeTexrepKey, V, GeTexrepKeyHash>& m,
                              GeTexrepKey key) const {
    // PPSSPP FindReplacement zeroes the address first when ignoreAddress is set,
    // then LookupWildcard tries these in order.
    if (ignore_address_) key.cachekey &= 0xFFFFFFFFULL;
    const uint64_t ck = key.cachekey;
    const uint32_t h = key.hash;
    auto try_key = [&](uint64_t c, uint32_t d) -> const V* {
        const auto it = m.find({c, d});
        return it != m.end() ? &it->second : nullptr;
    };
    if (const V* v = try_key(ck, h)) return v;
    if (const V* v = try_key(ck & 0xFFFFFFFFULL, 0)) return v;
    if (!ignore_address_) {
        if (const V* v = try_key(ck, 0)) return v;
    }
    if (const V* v = try_key(ck & 0xFFFFFFFFULL, h)) return v;
    if (!ignore_address_) {
        if (const V* v = try_key(ck & ~0xFFFFFFFFULL, h)) return v;
        if (const V* v = try_key(ck & ~0xFFFFFFFFULL, 0)) return v;
    }
    return try_key(0, h);
}

TexrepFind TexrepPack::find(GeTexrepKey key) const {
    TexrepFind f;
    if (const std::string* p = wildcard(aliases_, key)) {
        f.found = true;
        f.ignored = p->empty();
        f.path = *p;
    }
    return f;
}

TexrepFilter TexrepPack::filter(GeTexrepKey key) const {
    if (const TexrepFilter* f = wildcard(filtering_, key)) return *f;
    const auto it = filtering_.find({0, 0});  // global wildcard
    return it != filtering_.end() ? it->second : TexrepFilter::None;
}
