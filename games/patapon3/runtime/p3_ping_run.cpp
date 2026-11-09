// The runtime half of the partner pings (p3_ping.h): GE observers on the GL thread, slots from guest RAM, pings
// from the infrastructure client. Hideout: each partner's tag is drawn right after its badge. Missions and VS: the
// strip is drawn at the first mask outline / minimap fortress, so later game windows still cover it.
#include "p3_ping.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "net/infra_client.h"

namespace {

struct Look {
    int bars = 0;  // 0 = the game's own badge
    uint8_t r = 0, g = 0, b = 0;
};

struct Shown {             // one badge of the last presented frame
    bool partner = false;  // false: our own hero (a ping is between two players: no tag, badge untouched)
    infra::Client::PeerPing ping;
};

// GL thread only.
std::vector<GePrimBox> g_badges;            // hideout badges of the frame being drawn, in draw order
P3Prim g_screen = P3Prim::None;             // Mask (mission) or VsHud seen in the frame being drawn
bool g_strip_drawn = false;                 // the strip went into the frame being drawn
int g_prims = 0;                            // PRIMs seen since the last present
int g_strip = -1;                           // strip index after the last badge frame, -1 outside a badge
std::vector<Look> g_looks;                  // per badge in draw order, from the last presented frame
std::vector<Shown> g_shown;                 // what each badge shows, in draw order
std::vector<WorldTag> g_strip_tags;         // the strip of the last presented frame
P3Prim g_strip_kind = P3Prim::None;         // its screen
uint32_t g_base = 0;                        // online-id table, 0 until found
int64_t g_next_scan = 0, g_next_ping = 0;
std::map<std::string, infra::Client::PeerPing> g_pings;  // by online id, refreshed once a second
bool g_logged_base = false, g_logged_strip = false;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

infra::Client::PeerPing ping_of(infra::Client& c, const std::string& id) {
    auto it = g_pings.find(id);
    if (it == g_pings.end()) it = g_pings.emplace(id, c.peer_ping(id)).first;
    return it->second;
}

void log_base() {
    if (g_base && !g_logged_base) {
        g_logged_base = true;
        std::fprintf(stderr, "[P3] ping: party table at 0x%08X\n", g_base);
    }
}

// The tag of badge `index` of this frame, with what its slot showed in the last presented frame. Drawn right away:
// game windows drawn later (hideout menus) cover it like the rest of the HUD.
void badge_tag(size_t index, const GePrimBox& box) {
    if (index >= g_shown.size() || !g_shown[index].partner) return;
    const WorldTag t = p3_ping_tag(box, g_shown[index].ping.ms);
    if (!t.text.empty()) psp_ge_draw_tag(t, false);
}

bool on_prim(const GePrimInfo& p, const GePrimBox& box, uint8_t* rgba, size_t n, size_t stride) {
    g_prims++;
    switch (const P3Prim kind = p3_ping_classify(p, box)) {
    case P3Prim::BadgeFrame:
        badge_tag(g_badges.size(), box);
        g_badges.push_back(box);
        g_strip = 0;
        return true;
    case P3Prim::BadgeStrip: {
        if (g_strip < 0) return true;
        const int strip = g_strip++;  // 0-2: the bars; 3-4: the icon
        const size_t badge = g_badges.size() - 1;
        if (strip >= 3 || badge >= g_looks.size() || g_looks[badge].bars == 0) return true;
        if (strip >= g_looks[badge].bars) return false;
        for (size_t i = 0; i < n; i++) {
            uint8_t* c = rgba + i * stride;
            c[0] = g_looks[badge].r;
            c[1] = g_looks[badge].g;
            c[2] = g_looks[badge].b;
        }
        return true;
    }
    case P3Prim::Mask:
    case P3Prim::VsHud:
        if (g_screen == P3Prim::None) g_screen = kind;
        if (kind == g_strip_kind && !g_strip_drawn) {
            g_strip_drawn = true;
            for (const WorldTag& t : g_strip_tags) psp_ge_draw_tag(t, false);
        }
        g_strip = -1;
        return true;
    case P3Prim::None:
        g_strip = -1;
        return true;
    }
    return true;
}

// Hideout: what each badge shows (partner ping, bar colours), matched by p3_ping_assign.
void hideout(const uint8_t* rdram, infra::Client& c, const std::vector<GePrimBox>& badges, int64_t now) {
    const std::string own = c.online_id();
    std::vector<std::string> slots = g_base ? p3_ping_read_slots(rdram, g_base) : std::vector<std::string>();
    std::vector<int> slot_of = p3_ping_assign(P3Prim::BadgeFrame, badges.size(), slots, own);
    if (slot_of.empty() && now >= g_next_scan) {  // first time, or the table moved: find it again
        g_next_scan = now + 2000;
        g_base = p3_ping_pick_base(rdram, own, P3Prim::BadgeFrame, badges.size());
        slots = g_base ? p3_ping_read_slots(rdram, g_base) : std::vector<std::string>();
        slot_of = p3_ping_assign(P3Prim::BadgeFrame, badges.size(), slots, own);
        log_base();
    }
    for (size_t k = 0; k < slot_of.size(); k++) {
        const std::string& id = slots[slot_of[k]];
        Shown s;
        if (id != own) s = {true, ping_of(c, id)};
        const WorldTag t = p3_ping_tag(badges[k], s.ping.ms);
        g_looks.push_back({s.partner ? p3_ping_bars(s.ping.ms) : 0, t.r, t.g, t.b});
        g_shown.push_back(s);
    }
}

// Missions (4 slots) and VS (8): every other occupied slot, by player number, whatever the masks show.
void strip(const uint8_t* rdram, infra::Client& c, P3Prim screen, int64_t now) {
    const std::string own = c.online_id();
    const int count = screen == P3Prim::VsHud ? 8 : 4;
    std::vector<std::string> slots = g_base ? p3_ping_read_slots(rdram, g_base, count) : std::vector<std::string>();
    const bool lost = std::find(slots.begin(), slots.end(), own) == slots.end();
    if (lost || now >= g_next_scan) {
        // Pick again every 2 s, not only when our id is gone: a stale copy keeps our id while the game fills the
        // live table (a red-army player saw 3 of 7 players, 7 Nov). Cheap on a known base: no scan.
        if (lost && now < g_next_scan) return;
        g_next_scan = now + 2000;
        g_base = p3_ping_pick_base(rdram, own, P3Prim::None, 0, count);
        if (!g_base) return;
        slots = p3_ping_read_slots(rdram, g_base, count);
        log_base();
    }
    if (std::find(slots.begin(), slots.end(), own) == slots.end()) return;
    const std::vector<std::string> names = p3_ping_read_names(rdram, g_base, count);
    std::vector<P3Partner> partners;
    for (int i = 0; i < count; i++) {
        if (slots[i].empty() || slots[i] == own) continue;
        const infra::Client::PeerPing p = ping_of(c, slots[i]);
        partners.push_back({i + 1, names[i], p.ms, p.via_server});
    }
    const bool mission = screen == P3Prim::Mask;
    g_strip_tags = p3_ping_strip(partners, mission ? kP3MissionZone : kP3VsZone, mission);
    g_strip_kind = g_strip_tags.empty() ? P3Prim::None : screen;
    if (!g_logged_strip && !g_strip_tags.empty()) {
        g_logged_strip = true;
        std::fprintf(stderr, "[P3] ping strip: %s, %zu partner(s)\n", mission ? "mission" : "vs", partners.size());
    }
}

void on_frame(const uint8_t* rdram) {
    if (g_prims == 0) return;  // the same frame presented again: keep its tags and bars
    const std::vector<GePrimBox> badges = std::move(g_badges);
    const P3Prim screen = g_screen;
    g_badges.clear();
    g_screen = P3Prim::None;
    g_strip_drawn = false;
    g_prims = 0;
    g_strip = -1;
    g_looks.clear();
    g_shown.clear();
    g_strip_tags.clear();
    g_strip_kind = P3Prim::None;

    infra::Client& c = infra::client();
    if (!c.connected()) return;
    const int64_t now = now_ms();
    if (now >= g_next_ping) {
        g_next_ping = now + 1000;
        g_pings.clear();
    }
    if (screen != P3Prim::None) strip(rdram, c, screen, now);
    else if (!badges.empty()) hideout(rdram, c, badges, now);
}

}  // namespace

void p3_ping_register() {
    const char* e = std::getenv("PSPRECOMP_P3_SHOW_PING");
    if (e && e[0] == '0') return;
    psp_ge_set_observers(on_prim, on_frame);
    char* dir = SDL_GetBasePath();  // the exe's folder, with its trailing separator
    const bool icon = psp_world_tag_set_icon(std::string(dir ? dir : "") + "assets/ping_signal.png", 8);
    SDL_free(dir);
    std::fprintf(stderr, "[P3] ping of online partners: on (icon %s)\n", icon ? "on" : "missing, number only");
}
