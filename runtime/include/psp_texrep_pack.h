#pragma once
// A PPSSPP texture pack's textures.ini + root listing, and PPSSPP's lookup
// (TextureReplacer.cpp LoadIniValues / ScanForHashNamedFiles / ComputeAliasMap /
// LookupWildcard). Pure: no file I/O, no GL.
#include "psp_texrep_hash.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

enum class TexrepFilter { None = 0, Nearest = 1, Linear = 2 };  // TexCacheEntry::forced_filter

struct TexrepFind {
    bool found = false;    // the pack lists this texture
    bool ignored = false;  // listed with an empty file name: keep the original
    std::string path;      // relative to the pack folder, '/' separators
};

struct GeTexrepKeyHash {
    size_t operator()(const GeTexrepKey& k) const {
        return std::hash<uint64_t>()(k.cachekey ^ (static_cast<uint64_t>(k.hash) * 0x9E3779B97F4A7C15ULL));
    }
};

class TexrepPack {
public:
    bool load(const std::string& ini_text, const std::vector<std::string>& root_files, std::string* error);
    bool ignore_address() const { return ignore_address_; }
    bool reduce_hash() const { return reduce_hash_; }
    bool xxh32() const { return xxh32_; }
    float reduce_for(int w, int h) const;
    bool hash_range(uint32_t addr, int w, int h, int* nw, int* nh) const;
    TexrepFind find(GeTexrepKey key) const;
    TexrepFilter filter(GeTexrepKey key) const;
    size_t alias_count() const { return aliases_.size(); }

private:
    template <typename V>
    const V* wildcard(const std::unordered_map<GeTexrepKey, V, GeTexrepKeyHash>& m, GeTexrepKey key) const;

    bool ignore_address_ = false;
    bool reduce_hash_ = false;
    bool xxh32_ = false;
    std::unordered_map<GeTexrepKey, std::string, GeTexrepKeyHash> aliases_;   // "" = ignored
    std::unordered_map<GeTexrepKey, TexrepFilter, GeTexrepKeyHash> filtering_;
    std::map<uint64_t, std::pair<int, int>> hashranges_;  // addr<<32 | w<<16 | h
    std::map<uint64_t, float> reduceranges_;              // w<<16 | h
};
