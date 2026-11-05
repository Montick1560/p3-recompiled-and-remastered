// The runtime half of the partner pings (p3_ping.h): GE observers on the GL thread, slots from guest RAM, pings
// from the infrastructure client; each partner's tag is drawn into the game image right after its badge / mask.
#include "p3_ping.h"

#include <SDL.h>

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

struct Shown {             // one anchor of the last presented frame
    bool partner = false;  // false: our own hero (a ping is between two players: no tag, badge untouched)
    infra::Client::PeerPing ping;
};

// GL thread only.
std::vector<GePrimBox> g_badges, g_masks;   // anchors of the frame being drawn, in draw order
int g_own_mask = -1;                        // the anchor the game draws gold (ours) in that frame, -1 none
int g_prims = 0;                            // PRIMs seen since the last present
int g_strip = -1;                           // strip index after the last badge frame, -1 outside a badge
std::vector<Look> g_looks;                  // per badge in draw order, from the last presented frame
P3Prim g_kind = P3Prim::None;               // anchors of the last presented frame: badges or masks
std::vector<Shown> g_shown;                 // what each of them shows, in draw order
uint32_t g_base = 0;                        // online-id table, 0 until found
int64_t g_next_scan = 0, g_next_ping = 0;
std::map<std::string, infra::Client::PeerPing> g_pings;  // by online id, refreshed once a second
bool g_logged_base = false;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The tag of anchor `index` of this frame, with what its slot showed in the last presented frame. Drawn right
// away: game windows drawn later (hideout menus, the mission's pause dialog) cover it like the rest of the HUD.
void tag(P3Prim kind, size_t index, const GePrimBox& box) {
    if (kind != g_kind || index >= g_shown.size() || !g_shown[index].partner) return;
    const WorldTag t = p3_ping_tag(kind, box, g_shown[index].ping.ms, g_shown[index].ping.via_server);
    if (!t.text.empty() || t.icon >= 0) psp_ge_draw_tag(t, false);
}

bool on_prim(const GePrimInfo& p, const GePrimBox& box, uint8_t* rgba, size_t n, size_t stride) {
    g_prims++;
    switch (p3_ping_classify(p, box)) {
    case P3Prim::BadgeFrame:
        tag(P3Prim::BadgeFrame, g_badges.size(), box);
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
        if (n > 0 && rgba[0] == 255 && rgba[1] == 192 && rgba[2] == 0)  // MASKS.md: ours gold, others grey
            g_own_mask = static_cast<int>(g_masks.size());
        tag(P3Prim::Mask, g_masks.size(), box);
        g_masks.push_back(box);
        g_strip = -1;
        return true;
    case P3Prim::None:
        g_strip = -1;
        return true;
    }
    return true;
}

void on_frame(const uint8_t* rdram) {
    if (g_prims == 0) return;  // the same frame presented again: keep its tags and bars
    const std::vector<GePrimBox> badges = std::move(g_badges), masks = std::move(g_masks);
    const int own_mask = g_own_mask;
    g_badges.clear();
    g_masks.clear();
    g_own_mask = -1;
    g_prims = 0;
    g_strip = -1;

    std::vector<Look> looks;
    std::vector<Shown> shown;
    const P3Prim kind = !masks.empty() ? P3Prim::Mask : !badges.empty() ? P3Prim::BadgeFrame : P3Prim::None;
    infra::Client& c = infra::client();
    if (kind != P3Prim::None && c.connected()) {
        const std::vector<GePrimBox>& anchors = kind == P3Prim::Mask ? masks : badges;
        const int own_anchor = kind == P3Prim::Mask ? own_mask : -1;
        const std::string own = c.online_id();
        const int64_t now = now_ms();
        std::vector<std::string> slots = g_base ? p3_ping_read_slots(rdram, g_base) : std::vector<std::string>();
        std::vector<int> slot_of = p3_ping_assign(kind, anchors.size(), slots, own, own_anchor);
        if (slot_of.empty() && now >= g_next_scan) {  // first time, or the table moved: find it again
            g_next_scan = now + 2000;
            g_base = p3_ping_pick_base(rdram, own, kind, anchors.size());
            slots = g_base ? p3_ping_read_slots(rdram, g_base) : std::vector<std::string>();
            slot_of = p3_ping_assign(kind, anchors.size(), slots, own, own_anchor);
            if (g_base && !g_logged_base) {
                g_logged_base = true;
                std::fprintf(stderr, "[P3] ping: party table at 0x%08X\n", g_base);
            }
        }
        if (now >= g_next_ping) {
            g_next_ping = now + 1000;
            g_pings.clear();
        }
        for (size_t k = 0; k < slot_of.size(); k++) {
            const std::string& id = slots[slot_of[k]];
            Shown s;
            if (id != own) {
                auto it = g_pings.find(id);
                if (it == g_pings.end()) it = g_pings.emplace(id, c.peer_ping(id)).first;
                s = {true, it->second};
            }
            const WorldTag t = p3_ping_tag(kind, anchors[k], s.ping.ms, s.ping.via_server);
            if (kind == P3Prim::BadgeFrame) looks.push_back({s.partner ? p3_ping_bars(s.ping.ms) : 0, t.r, t.g, t.b});
            shown.push_back(s);
        }
    }
    g_looks = std::move(looks);
    g_shown = std::move(shown);
    g_kind = g_shown.empty() ? P3Prim::None : kind;
}

}  // namespace

void p3_ping_register() {
    const char* e = std::getenv("PSPRECOMP_P3_SHOW_PING");
    if (e && e[0] == '0') return;
    psp_ge_set_observers(on_prim, on_frame);
    char* dir = SDL_GetBasePath();  // the exe's folder, with its trailing separator
    const bool icon = psp_world_tag_set_icon(std::string(dir ? dir : "") + "assets/ping_signal.png", 8);
    SDL_free(dir);
    std::fprintf(stderr, "[P3] ping under online heroes: on (icon %s)\n", icon ? "on" : "missing, number only");
}
