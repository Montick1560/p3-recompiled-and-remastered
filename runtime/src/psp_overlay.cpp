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

// Function-local statics: banks register during static initialization of
// other translation units, before main() and in unspecified order.
std::vector<BankState>& banks() {
    static std::vector<BankState> v;
    return v;
}

std::mutex& mutex() {
    static std::mutex m;
    return m;
}

uint8_t* g_rdram = nullptr;

// Active-bank cache per window base: the bank whose code was verified the
// last time this window was inspected. Re-verified whenever the id changes.
struct Active {
    uint32_t window_lo;
    uint32_t id;
    BankState* state;  // nullptr when the loaded overlay failed verification
};
std::vector<Active> g_active;

uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
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
    g_active.clear();  // BankState pointers may have moved
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
    for (Active& a : g_active) {
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
        const uint64_t h = fnv1a64(guest(rdram, lo), match->bank->hash_len);
        BankState* verified = (h == match->bank->hash) ? match : nullptr;
        if (active) {
            *active = {lo, id, verified};
        } else {
            g_active.push_back({lo, id, verified});
        }
        if (!verified) return PspOverlayStatus::HashMismatch;
        active = &g_active.back();
        for (Active& a : g_active) {
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
    g_active.clear();
}
