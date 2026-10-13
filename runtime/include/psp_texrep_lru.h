#pragma once
// Pure byte-accounted LRU behind the texture-replacement caches: decoded PNG
// images (psp_texrep_queue) and GL textures (psp_ge_texture). Header-only and
// free of GL, threads and I/O, so it is unit-tested standalone.
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <unordered_map>
#include <vector>

template <typename Key, typename Hash = std::hash<Key>>
class TexrepLru {
public:
    explicit TexrepLru(size_t budget_bytes) : budget_(budget_bytes) {}

    size_t budget() const { return budget_; }
    void set_budget(size_t bytes) { budget_ = bytes; }
    size_t bytes() const { return bytes_; }
    size_t count() const { return entries_.size(); }
    bool contains(const Key& key) const { return entries_.find(key) != entries_.end(); }

    /// Insert or update `key`. Never evicts by itself; call trim() to enforce
    /// the budget (kept separate so eviction happens where it is safe).
    void put(const Key& key, size_t bytes) {
        Node& n = entries_[key];
        bytes_ -= n.bytes;
        n.bytes = bytes;
        n.clock = ++clock_;
        bytes_ += bytes;
    }

    /// Refresh the recency of an existing entry (newest keeps longest).
    void touch(const Key& key) {
        auto it = entries_.find(key);
        if (it != entries_.end()) it->second.clock = ++clock_;
    }

    /// Remove one entry; returns true when it was present.
    bool remove(const Key& key) {
        auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        bytes_ -= it->second.bytes;
        entries_.erase(it);
        return true;
    }

    void clear() {
        entries_.clear();
        bytes_ = 0;
    }

    /// Evict least-recently-used entries until bytes() <= budget(). Keys listed
    /// in `keep` are protected (they are in use right now), and so is the most
    /// recently used entry (a single oversized entry stays instead of thrashing).
    /// Returns the evicted keys, least recently used first.
    std::vector<Key> trim(std::initializer_list<Key> keep = {}) {
        return trim_if(keep, [](const Key&) { return true; });
    }

    /// trim() that only considers entries `eligible(key)` accepts. The caller
    /// uses it to spare entries still in use: when no eligible victim remains,
    /// the budget is exceeded on purpose (bytes() > budget() is allowed) —
    /// evicting a live entry would make the caller reload it immediately.
    template <typename Pred>
    std::vector<Key> trim_if(std::initializer_list<Key> keep, Pred eligible) {
        std::vector<Key> evicted;
        while (bytes_ > budget_) {
            uint64_t newest = 0;
            for (const auto& [key, node] : entries_) {
                (void)key;
                if (node.clock > newest) newest = node.clock;
            }
            auto victim = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (is_kept(it->first, keep)) continue;
                if (!eligible(it->first)) continue;
                if (it->second.clock == newest) continue;
                if (victim == entries_.end() || it->second.clock < victim->second.clock) {
                    victim = it;
                }
            }
            if (victim == entries_.end()) break;  // only protected entries remain
            bytes_ -= victim->second.bytes;
            evicted.push_back(victim->first);
            entries_.erase(victim);
        }
        return evicted;
    }

private:
    struct Node {
        size_t bytes = 0;
        uint64_t clock = 0;
    };

    bool is_kept(const Key& key, const std::initializer_list<Key>& keep) const {
        for (const Key& k : keep) {
            if (k == key) return true;
        }
        return false;
    }

    std::unordered_map<Key, Node, Hash> entries_;
    size_t bytes_ = 0;
    size_t budget_ = 0;
    uint64_t clock_ = 0;
};
