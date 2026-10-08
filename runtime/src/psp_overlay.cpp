#include "psp_overlay.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct BankState {
    const PspOverlayBank* bank;
    std::unordered_map<uint32_t, FuncPtr> map;
};

// Leaked function-local singletons: banks register during static
// initialization of other translation units (before main(), unspecified
// order), and game threads may still resolve while exit-time destructors run.
std::vector<BankState>& banks() {
    static auto* v = new std::vector<BankState>();
    return *v;
}

std::mutex& mutex() {
    static auto* m = new std::mutex();
    return *m;
}

uint8_t* g_rdram = nullptr;

// Active-bank cache per window base: the bank whose code was verified the
// last time this window was inspected. Re-verified whenever the id changes.
struct Active {
    uint32_t window_lo;
    uint32_t id;
    BankState* state;  // nullptr when the loaded overlay failed verification
};
std::vector<Active>& active_list() {
    static auto* v = new std::vector<Active>();
    return *v;
}

uint64_t fnv1a64_continue(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

const uint8_t* guest(uint8_t* rdram, uint32_t addr) {
    return rdram + (addr & 0x07FFFFFFU);
}

uint32_t read_u32(uint8_t* rdram, uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, guest(rdram, addr), 4);
    return v;
}

}  // namespace

void psp_overlay_register_bank(const PspOverlayBank* bank) {
    std::lock_guard<std::mutex> lock(mutex());
    BankState st{bank, {}};
    st.map.reserve(bank->count);
    for (uint32_t i = 0; i < bank->count; i++) {
        st.map[bank->addrs[i]] = bank->fns[i];
    }
    banks().push_back(std::move(st));
    active_list().clear();  // BankState pointers may have moved
}

PspOverlayStatus psp_overlay_resolve(uint8_t* rdram, uint32_t vaddr, FuncPtr* out) {
    std::lock_guard<std::mutex> lock(mutex());
    const PspOverlayBank* window = nullptr;
    for (const BankState& st : banks()) {
        if (vaddr >= st.bank->window_lo && vaddr < st.bank->window_hi) {
            window = st.bank;
            break;
        }
    }
    if (!window || !rdram) return PspOverlayStatus::NotInWindow;

    const uint32_t lo = window->window_lo;
    const bool has_overlay = std::memcmp(guest(rdram, lo), "MWo3", 4) == 0;
    const uint32_t id = has_overlay ? read_u32(rdram, lo + 4) : 0;

    Active* active = nullptr;
    for (Active& a : active_list()) {
        if (a.window_lo == lo) active = &a;
    }
    // Whatever now occupies the window is not what was verified: forget the
    // verification so the next load of any overlay (even the same id) is
    // hashed again.
    if (active && (!has_overlay || active->id != id)) {
        active->id = 0;
        active->state = nullptr;
    }
    if (!has_overlay) return PspOverlayStatus::NoBank;
    if (!active || active->id != id || !active->state) {
        BankState* match = nullptr;
        for (BankState& st : banks()) {
            if (st.bank->window_lo == lo && st.bank->id == id) match = &st;
        }
        if (!match) return PspOverlayStatus::NoBank;
        uint64_t h = 0xcbf29ce484222325ULL;
        for (uint32_t r = 0; r < match->bank->hash_range_count; r++) {
            const uint32_t start = match->bank->hash_ranges[2 * r];
            const uint32_t end = match->bank->hash_ranges[2 * r + 1];
            h = fnv1a64_continue(h, guest(rdram, start), end - start);
        }
        BankState* verified = (h == match->bank->hash) ? match : nullptr;
        if (active) {
            *active = {lo, id, verified};
        } else {
            active_list().push_back({lo, id, verified});
        }
        if (!verified) return PspOverlayStatus::HashMismatch;
        active = &active_list().back();
        for (Active& a : active_list()) {
            if (a.window_lo == lo) active = &a;
        }
    }

    auto it = active->state->map.find(vaddr);
    if (it == active->state->map.end()) return PspOverlayStatus::NotFound;
    *out = it->second;
    return PspOverlayStatus::Found;
}

void psp_overlay_set_rdram(uint8_t* rdram) { g_rdram = rdram; }
uint8_t* psp_overlay_rdram() { return g_rdram; }

void psp_overlay_reset_for_tests() {
    std::lock_guard<std::mutex> lock(mutex());
    banks().clear();
    active_list().clear();
}
