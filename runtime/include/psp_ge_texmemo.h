#pragma once
// Texture content-hash memo (pure): ge_texture_bind hashes a texture's bytes
// at most once per display list. A scene binds the same few atlases hundreds
// of times per list; re-hashing each bind cost ~40 MB of FNV per frame.
//
// The hash is only trusted for a while: the CPU may rewrite textures between
// lists (fonts, movie frames, palette fades). find() says when the stored
// hash can be reused and when guest memory must be re-read. A re-read that
// matches the stored hash pushes the next check back, doubling the window
// (1, 2, 4, ... up to max_window lists); a mismatch, or a GE block transfer
// through invalidate(), restarts the window at one list, so an animation
// that changes every list keeps verifying every list. `list` counts display
// lists (one per frame in the common case).
#include <cstddef>
#include <cstdint>
#include <unordered_map>

class TexHashMemo {
public:
    // max_window: cap of the reuse window in lists. 1 = verify every list
    // (the backoff off); a value that is not a power of two rounds down.
    explicit TexHashMemo(uint32_t max_window = 8);
    // `params` hashes everything besides the address that changes the
    // decoded result (format, size, stride, swizzle, CLUT address/format).
    bool find(uint32_t list, uint32_t addr, uint32_t params, uint32_t* hash);
    void put(uint32_t list, uint32_t addr, uint32_t params, uint32_t hash);
    void invalidate();

private:
    struct Entry {
        uint32_t hash = 0;
        uint32_t next_check = 0;  // list index that must re-read guest memory
        uint8_t stable = 0;       // consecutive checks with no change (capped)
    };
    // Entries are tiny; on overflow drop everything (one rehash round).
    static constexpr size_t kMaxEntries = 4096;
    uint32_t max_shift_;  // floor(log2(max_window)), the cap of `stable`
    std::unordered_map<uint64_t, Entry> entries_;
};
