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
    if (!p.texture) return P3Prim::None;
    if (p.prim == 4 && p.tex_fmt == 4 && p.vtype == 0x193 && p.tex_addr == 0x104080 && p.clut_addr == 0x1044C0)
        return p.count == 7 ? P3Prim::BadgeFrame : p.count == 4 ? P3Prim::BadgeStrip : P3Prim::None;
    // The HUD atlas draws the masks, their health frames and fills too: only the outline is 26x11 at y 27.
    const auto near = [](float a, float b) { return std::fabs(a - b) < 0.6f; };
    if (p.prim == 4 && p.tex_fmt == 5 && p.count == 4 && p.vtype == 0x800183 && p.tex_addr == 0x11DA00 &&
        p.tex_size == 0x0808 && p.clut_addr == 0x12DA40 && near(box.x1 - box.x0, 26) && near(box.y1 - box.y0, 11) && near(box.y0, 27))
        return P3Prim::Mask;
    // VS minimap fortresses (bases 32x20, towers 26x15) end at y 50 in the top band (STRIP.md).
    if (p.prim == 6 && p.count == 2 && p.vtype == 0x800102 && p.tex_addr == 0x1446C0 && p.tex_fmt == 4 &&
        p.tex_size == 0x0505 && p.clut_addr == 0x144900 && near(box.y1, 50))
        return P3Prim::VsHud;
    return P3Prim::None;
}

std::vector<std::string> p3_ping_read_slots(const uint8_t* rdram, uint32_t id_base, int slots) {
    std::vector<std::string> out(slots);
    for (int i = 0; i < slots; i++)
        if (named(rdram, id_base, i)) out[i] = id_at(rdram, id_base + i * kIdStride);
    return out;
}

std::vector<std::string> p3_ping_read_names(const uint8_t* rdram, uint32_t id_base, int slots) {
    std::vector<std::string> out(slots);
    for (int i = 0; i < slots; i++) {
        const uint8_t* n = at(rdram, id_base - kNameBelow + i * kNameStride);
        char16_t u[17] = {};  // hero names are at most 16 characters
        for (int k = 0; k < 16; k++) u[k] = static_cast<char16_t>(n[2 * k] | n[2 * k + 1] << 8);
        out[i] = p3_ping_short_name(u);
    }
    return out;
}

uint32_t p3_ping_pick_base(const uint8_t* rdram, const std::string& own, P3Prim kind, size_t anchors, int slots) {
    if (own.empty() || own.size() > kIdMax) return 0;
    // Without anchors (missions, VS) any table holding our id in the screen's slots fits; on the scan the fullest
    // wins over a stale copy (a red-army player kept a hideout copy in a 4v4 VS, 7 Nov). Only the screen's slots
    // count, so a stale 8-player VS copy cannot win a mission.
    auto score = [&](uint32_t base) -> int {
        const std::vector<std::string> ids = p3_ping_read_slots(rdram, base, anchors > 0 ? 4 : slots);
        if (anchors > 0) return p3_ping_assign(kind, anchors, ids, own).empty() ? 0 : 1;
        if (std::find(ids.begin(), ids.end(), own) == ids.end()) return 0;
        return static_cast<int>(std::count_if(ids.begin(), ids.end(), [](const std::string& id) { return !id.empty(); }));
    };
    for (uint32_t b : kKnownBases)
        if (score(b) > 0) return b;
    uint32_t best = 0;
    int best_score = 0;
    const uint8_t* lo = at(rdram, kSearchFrom);
    const uint8_t* hi = at(rdram, kSearchTo);
    const char* pat = own.c_str();
    const size_t len = own.size() + 1;  // with its NUL
    for (const uint8_t* p = lo; (p = std::search(p, hi, pat, pat + len)) != hi; p++) {
        const uint32_t hit = kSearchFrom + static_cast<uint32_t>(p - lo);
        for (int i = 0; i < (anchors > 0 ? 4 : slots); i++) {
            const int sc = score(hit - i * kIdStride);
            if (sc > best_score) {
                best_score = sc;
                best = hit - i * kIdStride;
            }
            if (anchors > 0 && sc > 0) return best;
        }
    }
    return best;
}

std::vector<int> p3_ping_assign(P3Prim kind, size_t anchors, const std::vector<std::string>& slots,
                                const std::string& own) {
    static const int kBadgeOrder[] = {1, 0, 2, 3};  // SLOTS.md + MASKS.md, 2-4 players
    if (kind != P3Prim::BadgeFrame) return {};
    std::vector<int> order;
    for (int i : kBadgeOrder)
        if (i < static_cast<int>(slots.size()) && !slots[i].empty()) order.push_back(i);
    const bool own_in = std::any_of(order.begin(), order.end(), [&](int i) { return slots[i] == own; });
    if (anchors == 0 || anchors != order.size() || !own_in) return {};
    return order;
}

int p3_ping_bars(int ms) { return ms < 0 ? 0 : ms < 80 ? 3 : ms < 150 ? 2 : 1; }

namespace {

void paint(WorldTag& t, int bars) {
    if (bars == 0) { t.r = 150; t.g = 150; t.b = 150; }
    else if (bars == 3) { t.r = 80; t.g = 220; t.b = 90; }
    else if (bars == 2) { t.r = 255; t.g = 210; t.b = 40; }
    else { t.r = 240; t.g = 70; t.b = 60; }
}

// Small digits after the signal icon (frame = bars, +4 through the server); unknown: the grey icon alone.
WorldTag small_ping(int ms, bool via_server) {
    WorldTag t;
    const int bars = p3_ping_bars(ms);
    paint(t, bars);
    if (ms >= 0) t.text = std::to_string(ms);
    t.size_px = 7.5f;
    t.icon = bars + (via_server ? 4 : 0);
    return t;
}

}  // namespace

WorldTag p3_ping_tag(const GePrimBox& box, int ms) {
    WorldTag t;
    if (ms < 0) return t;
    paint(t, p3_ping_bars(ms));
    t.text = std::to_string(ms) + " ms";
    t.x = (box.x0 + box.x1) * 0.5f;
    t.y = box.y1 + 2.0f;
    return t;
}

// Zones from the user's screenshots of 7 Nov (spec), checked in the lab (build/ping_recon/STRIP.md).
// Glyphs are ~4.4 PSP px wide at 7.5 px ("197" = 13 px in the user's 4-player video): "4.Krish2" fits before the
// mission split, a 1-digit number before the VS one.
const P3Zone kP3MissionZone{122, 305, 13, 0.61f};
const P3Zone kP3VsZone{143, 366, 54, 0.3f};

std::vector<WorldTag> p3_ping_strip(std::vector<P3Partner> partners, const P3Zone& zone, bool names) {
    std::sort(partners.begin(), partners.end(), [](const P3Partner& a, const P3Partner& b) { return a.number < b.number; });
    std::vector<WorldTag> out;
    const float cell = partners.empty() ? 0.0f : (zone.x1 - zone.x0) / partners.size();
    for (size_t i = 0; i < partners.size(); i++) {
        const P3Partner& p = partners[i];
        const float split = zone.x0 + cell * i + cell * zone.name_share;
        WorldTag name;
        name.text = std::to_string(p.number) + (names ? "." + p.name : "");
        name.size_px = 7.5f;
        name.align = 1.0f;
        name.x = split;
        name.y = zone.y;
        WorldTag ping = small_ping(p.ms, p.via_server);
        ping.align = 0.0f;
        ping.x = split + 2.0f;
        ping.y = zone.y;
        out.push_back(name);
        out.push_back(ping);
    }
    return out;
}

std::string p3_ping_short_name(const char16_t* utf16, size_t max_chars) {
    std::string out;
    for (size_t i = 0; out.size() < max_chars && utf16[i]; i++)
        if (utf16[i] != 0x32A4) out += utf16[i] >= 0x20 && utf16[i] < 0x7F ? static_cast<char>(utf16[i]) : '?';
    return out;
}
