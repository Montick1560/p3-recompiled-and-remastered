#include "psp_ge_texmemo.h"

static uint64_t memo_key(uint32_t addr, uint32_t params) {
    return (static_cast<uint64_t>(addr) << 32) | params;
}

static uint32_t floor_log2(uint32_t v) {
    uint32_t s = 0;
    while (v >>= 1) s++;
    return s;
}

TexHashMemo::TexHashMemo(uint32_t max_window)
    : max_shift_(floor_log2(max_window < 1 ? 1 : max_window)) {}

bool TexHashMemo::find(uint32_t list, uint32_t addr, uint32_t params, uint32_t* hash) {
    const auto it = entries_.find(memo_key(addr, params));
    if (it == entries_.end()) {
        *hash = 0;
        return false;
    }
    const Entry& e = it->second;
    *hash = e.hash;
    return list < e.next_check;  // inside the backoff window: reuse
}

void TexHashMemo::put(uint32_t list, uint32_t addr, uint32_t params, uint32_t hash) {
    auto it = entries_.find(memo_key(addr, params));
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxEntries) entries_.clear();  // one rehash round
        Entry e;
        e.hash = hash;
        e.next_check = list + 1;
        entries_.emplace(memo_key(addr, params), e);
        return;
    }
    Entry& e = it->second;
    if (e.hash != hash) {
        e.hash = hash;
        e.stable = 0;
    } else if (e.stable < max_shift_) {
        e.stable++;
    }
    e.next_check = list + (1u << e.stable);
}

void TexHashMemo::invalidate() { entries_.clear(); }
