// The pure half of the ping under each hero (p3_ping.h): no GL, no network, no globals.
#include "p3_ping.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t kIdStride = 0x118, kNameStride = 0x23C, kNameBelow = 0x1128;  // build/ping_recon/SLOTS.md
constexpr uint32_t kKnownBases[] = {0x0936E114u /* DxD 1.1.4 */, 0x0936DFD4u /* DxD 1.1.5b */};
constexpr uint32_t kSearchFrom = 0x09300000u, kSearchTo = 0x09400000u;
constexpr int kSlots = 4;
constexpr size_t kIdMax = 16;  // PSN online ids are 3-16 characters

const uint8_t* at(const uint8_t* rdram, uint32_t addr) { return rdram + (addr & 0x07FFFFFFu); }

// An online id (3-16 of A-Za-z0-9_-, the nickname rule), or "" for anything else.
std::string id_at(const uint8_t* rdram, uint32_t addr) {
    const char* p = reinterpret_cast<const char*>(at(rdram, addr));
    const size_t n = strnlen(p, kIdMax + 1);
    if (n < 3 || n > kIdMax) return std::string();
    for (size_t i = 0; i < n; i++)
        if (!std::isalnum(static_cast<unsigned char>(p[i])) && p[i] != '_' && p[i] != '-') return std::string();
    return std::string(p, n);
}

bool named(const uint8_t* rdram, uint32_t id_base, int slot) {
    const uint8_t* n = at(rdram, id_base - kNameBelow + slot * kNameStride);
    return n[0] != 0 || n[1] != 0;  // first UTF-16 unit
}

}  // namespace

P3Prim p3_ping_classify(const GePrimInfo& p, const GePrimBox& box) {
    if (!p.texture || p.prim != 4) return P3Prim::None;
    if (p.tex_fmt == 4 && p.vtype == 0x193 && p.tex_addr == 0x104080 && p.clut_addr == 0x1044C0)
        return p.count == 7 ? P3Prim::BadgeFrame : p.count == 4 ? P3Prim::BadgeStrip : P3Prim::None;
    // The HUD atlas draws the masks, their health frames and fills too: only the outline is 26x11 at y 27.
    const auto near = [](float a, float b) { return std::fabs(a - b) < 0.6f; };
    if (p.tex_fmt == 5 && p.count == 4 && p.vtype == 0x800183 && p.tex_addr == 0x11DA00 && p.tex_size == 0x0808 &&
        p.clut_addr == 0x12DA40 && near(box.x1 - box.x0, 26) && near(box.y1 - box.y0, 11) && near(box.y0, 27))
        return P3Prim::Mask;
    return P3Prim::None;
}

std::vector<std::string> p3_ping_read_slots(const uint8_t* rdram, uint32_t id_base) {
    std::vector<std::string> out(kSlots);
    for (int i = 0; i < kSlots; i++)
        if (named(rdram, id_base, i)) out[i] = id_at(rdram, id_base + i * kIdStride);
    return out;
}

uint32_t p3_ping_pick_base(const uint8_t* rdram, const std::string& own, P3Prim kind, size_t anchors) {
    if (own.empty() || own.size() > kIdMax) return 0;
    auto fits = [&](uint32_t base) { return !p3_ping_assign(kind, anchors, p3_ping_read_slots(rdram, base), own).empty(); };
    for (uint32_t b : kKnownBases)
        if (fits(b)) return b;
    const uint8_t* lo = at(rdram, kSearchFrom);
    const uint8_t* hi = at(rdram, kSearchTo);
    const char* pat = own.c_str();
    const size_t len = own.size() + 1;  // with its NUL
    for (const uint8_t* p = lo; (p = std::search(p, hi, pat, pat + len)) != hi; p++) {
        const uint32_t hit = kSearchFrom + static_cast<uint32_t>(p - lo);
        for (int i = 0; i < kSlots; i++)
            if (fits(hit - i * kIdStride)) return hit - i * kIdStride;
    }
    return 0;
}

std::vector<int> p3_ping_assign(P3Prim kind, size_t anchors, const std::vector<std::string>& slots,
                                const std::string& own, int own_anchor) {
    static const int kBadgeOrder[] = {1, 0, 2, 3}, kMaskOrder[] = {3, 2, 1, 0};  // MASKS.md, 2-4 players
    std::vector<int> order;
    for (int i : kind == P3Prim::Mask ? kMaskOrder : kBadgeOrder)
        if (i < static_cast<int>(slots.size()) && !slots[i].empty()) order.push_back(i);
    const auto own_at = std::find_if(order.begin(), order.end(), [&](int i) { return slots[i] == own; });
    if (anchors == 0 || anchors != order.size() || own_at == order.end()) return {};
    if (own_anchor >= 0 && own_anchor != own_at - order.begin()) return {};
    return order;
}

int p3_ping_bars(int ms) { return ms < 0 ? 0 : ms < 80 ? 3 : ms < 150 ? 2 : 1; }

WorldTag p3_ping_tag(P3Prim kind, const GePrimBox& box, int ms, bool via_server) {
    WorldTag t;
    const bool mask = kind == P3Prim::Mask;
    if (ms < 0 && !mask) return t;
    const int bars = p3_ping_bars(ms);
    if (bars == 0) { t.r = 150; t.g = 150; t.b = 150; }
    else if (bars == 3) { t.r = 80; t.g = 220; t.b = 90; }
    else if (bars == 2) { t.r = 255; t.g = 210; t.b = 40; }
    else { t.r = 240; t.g = 70; t.b = 60; }
    if (ms >= 0) t.text = mask ? std::to_string(ms) : std::to_string(ms) + " ms";
    if (mask) {
        t.size_px = 7.5f;
        t.icon = bars + (via_server ? 4 : 0);
    }
    t.x = (box.x0 + box.x1) * 0.5f;
    t.y = box.y1 + (mask ? 1.0f : 2.0f);
    return t;
}
