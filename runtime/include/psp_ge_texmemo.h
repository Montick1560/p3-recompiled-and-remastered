#pragma once
// Texture content-hash memo (pure): ge_texture_bind hashes a texture's bytes
// at most once per display list. A scene binds the same few atlases hundreds
// of times per list; re-hashing each bind cost ~40 MB of FNV per frame.
// A new list always re-hashes (the CPU may rewrite textures between lists:
// fonts, movie frames, palette fades), and a GE block transfer inside a list
// invalidates the memo (it writes guest memory mid-list).
#include <cstdint>
#include <unordered_map>

class TexHashMemo {
public:
    // `params` hashes everything besides the address that changes the
    // decoded result (format, size, stride, swizzle, CLUT address/format).
    bool find(uint32_t list, uint32_t addr, uint32_t params, uint32_t* hash);
    void put(uint32_t list, uint32_t addr, uint32_t params, uint32_t hash);
    void invalidate();

private:
    void enter_list(uint32_t list);
    uint32_t list_ = 0xFFFFFFFFu;
    std::unordered_map<uint64_t, uint32_t> hashes_;
};
