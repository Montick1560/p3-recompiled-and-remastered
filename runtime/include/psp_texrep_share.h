#pragma once
// One GL texture per texture-pack file, shared by every GL cache slot whose
// texture resolves to that file. The cache is keyed by a hash of the whole
// guest texture span, while the pack key hashes less (PPSSPP clips to the
// rows drawn), so a texture the game keeps rewriting (loading tips: ~25
// cache misses per screen) maps many slots to one PNG. Without sharing each
// miss re-decoded the PNG (the CPU copy is dropped on upload) and uploaded
// another copy, drawing the original until the worker finished: HD flicker.
// Pure and GL-free (names are plain integers) so it is unit-tested standalone
// (tests/test_texrep_share.cpp). Render thread only, like the GL cache.
#include <cstdint>
#include <string>
#include <unordered_map>

class TexrepShare {
public:
    /// Live texture uploaded from `path`, with one more reference taken for
    /// the caller; 0 (no reference) when none is alive.
    uint32_t acquire(const std::string& path) {
        auto it = live_.find(path);
        if (it == live_.end()) return 0;
        it->second.refs++;
        return it->second.tex;
    }

    /// The caller just uploaded `path` into `tex` and holds its first
    /// reference. Only call after acquire() returned 0.
    void adopt(const std::string& path, uint32_t tex) { live_[path] = Live{tex, 1}; }

    /// Release one reference. True when it was the last one: the caller
    /// deletes the GL texture.
    bool drop(const std::string& path) {
        auto it = live_.find(path);
        if (it == live_.end()) return false;
        if (--it->second.refs > 0) return false;
        live_.erase(it);
        return true;
    }

    int refs(const std::string& path) const {
        auto it = live_.find(path);
        return it == live_.end() ? 0 : it->second.refs;
    }

    void clear() { live_.clear(); }

private:
    struct Live {
        uint32_t tex = 0;
        int refs = 0;
    };
    std::unordered_map<std::string, Live> live_;
};
