#include "psp_ge_texmemo.h"

static uint64_t memo_key(uint32_t addr, uint32_t params) {
    return (static_cast<uint64_t>(addr) << 32) | params;
}

void TexHashMemo::enter_list(uint32_t list) {
    if (list != list_) {
        hashes_.clear();
        list_ = list;
    }
}

bool TexHashMemo::find(uint32_t list, uint32_t addr, uint32_t params, uint32_t* hash) {
    enter_list(list);
    const auto it = hashes_.find(memo_key(addr, params));
    if (it == hashes_.end()) return false;
    *hash = it->second;
    return true;
}

void TexHashMemo::put(uint32_t list, uint32_t addr, uint32_t params, uint32_t hash) {
    enter_list(list);
    hashes_[memo_key(addr, params)] = hash;
}

void TexHashMemo::invalidate() { hashes_.clear(); }
