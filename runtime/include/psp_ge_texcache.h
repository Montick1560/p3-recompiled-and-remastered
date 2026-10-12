#pragma once
// Slot-selection policy of the fixed-capacity GL texture cache
// (runtime/src/psp_ge_texture.cpp). Header-only and free of GL, so it is
// unit-tested standalone. An LRU eviction leaves a vacant hole (!valid) in
// the slot array: the search must still honour an exact (addr, hash) match
// above that hole, otherwise the same texture is decoded, uploaded and
// byte-accounted again while its GL object stays resident in the higher slot
// (duplicate upload, leaked object, double-counted budget).
#include <array>
#include <cstddef>
#include <cstdint>

enum class GeTexCachePick {
    Match,  // exact (addr, hash) hit: `slot` is valid and resident
    Empty,  // `slot` is the first vacant slot: nothing to evict
    Evict,  // cache full: `slot` holds the least-recently-used entry
};

// Find (psp_addr, content_hash) in `slots`; a match wins over a vacancy, and
// a vacancy wins over an eviction. On Evict the caller must release the
// slot's GL object. Entry only needs the fields below (TexCacheEntry has
// more); this stays a template so tests can use a minimal struct.
template <typename Entry, std::size_t N>
GeTexCachePick ge_texcache_pick(
    const std::array<Entry, N>& slots,
    uint32_t psp_addr,
    uint32_t content_hash,
    int* slot
) {
    int empty_slot = -1;
    int best_slot = 0;
    uint32_t oldest_frame = UINT32_MAX;
    for (std::size_t i = 0; i < N; i++) {
        const Entry& e = slots[i];
        if (e.valid
            && e.psp_addr == psp_addr
            && e.content_hash == content_hash) {
            *slot = static_cast<int>(i);
            return GeTexCachePick::Match;
        }
        if (!e.valid) {
            if (empty_slot < 0) empty_slot = static_cast<int>(i);
            continue;
        }
        if (e.last_frame < oldest_frame) {
            oldest_frame = e.last_frame;
            best_slot = static_cast<int>(i);
        }
    }
    if (empty_slot >= 0) {
        *slot = empty_slot;
        return GeTexCachePick::Empty;
    }
    *slot = best_slot;
    return GeTexCachePick::Evict;
}
