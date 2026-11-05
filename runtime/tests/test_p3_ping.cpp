// p3_ping: Patapon 3 hero badge / name label signatures, party slots from RAM, anchor -> slot, bars and tags.
#include "p3_ping.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::vector<uint8_t> ram(0x01500000);  // up to guest 0x09500000

static void put_id(uint32_t base, int slot, const char* id) {
    std::memset(&ram[(base + slot * 0x118) & 0x07FFFFFF], 0, 17);
    std::memcpy(&ram[(base + slot * 0x118) & 0x07FFFFFF], id, std::strlen(id));
}
static void put_name(uint32_t base, int slot, const char* name) {  // ASCII -> UTF-16
    const uint32_t a = (base - 0x1128 + slot * 0x23C) & 0x07FFFFFF;
    std::memset(&ram[a], 0, 40);
    for (size_t i = 0; name[i]; i++) ram[a + 2 * i] = static_cast<uint8_t>(name[i]);
}
static void clear() { std::fill(ram.begin(), ram.end(), 0); }

static GePrimInfo badge(int count) {
    GePrimInfo p;
    p.prim = 4; p.count = count; p.vtype = 0x193; p.texture = true;
    p.tex_addr = 0x104080; p.tex_fmt = 4; p.clut_addr = 0x1044C0;
    return p;
}
static GePrimInfo hud() {  // the mission HUD atlas (build/ping_recon/MASKS.md)
    GePrimInfo p;
    p.prim = 4; p.count = 4; p.vtype = 0x800183; p.texture = true;
    p.tex_addr = 0x11DA00; p.tex_size = 0x0808; p.tex_fmt = 5; p.clut_addr = 0x12DA40;
    return p;
}
static bool same(const std::vector<int>& a, std::vector<int> b) { return a == b; }

int main() {
    // classify
    const GePrimBox any{0, 0, 10, 10};
    CHECK(p3_ping_classify(badge(7), any) == P3Prim::BadgeFrame, "classify_badge_frame");
    CHECK(p3_ping_classify(badge(4), any) == P3Prim::BadgeStrip, "classify_bar");
    GePrimInfo untex = badge(7);
    untex.texture = false;
    CHECK(p3_ping_classify(untex, any) == P3Prim::None, "untextured: none");
    // classify_mask: the health bar's 26x11 outline under each party mask (x 9+28k, y 27..38)
    CHECK(p3_ping_classify(hud(), {9, 27, 35, 38}) == P3Prim::Mask, "classify_mask: first outline");
    CHECK(p3_ping_classify(hud(), {93, 27, 119, 38}) == P3Prim::Mask, "classify_mask: fourth outline");
    CHECK(p3_ping_classify(hud(), {10, 5, 34, 29}) == P3Prim::None, "mask background: not the anchor");
    CHECK(p3_ping_classify(hud(), {10, 28, 34, 37}) == P3Prim::None, "health frame: not the anchor");
    CHECK(p3_ping_classify(hud(), {12, 30, 33, 35}) == P3Prim::None, "health fill: not the anchor");
    CHECK(p3_ping_classify(hud(), {200, 120, 226, 131}) == P3Prim::None, "same size elsewhere: not the anchor");
    GePrimInfo other = hud();
    other.clut_addr = 0x123440;
    CHECK(p3_ping_classify(other, {9, 27, 35, 38}) == P3Prim::None, "no_match_no_tags: another CLUT");

    // slots: 1.1.4 base, slot 1 left (name cleared, stale id kept)
    const uint32_t b114 = 0x0936E114, b115 = 0x0936DFD4, bfar = 0x09370000;
    clear();
    put_id(b114, 0, "P3A"); put_name(b114, 0, "Edson");
    put_id(b114, 1, "P3B");
    put_id(b114, 2, "P3C"); put_name(b114, 2, "Edson");
    const std::vector<std::string> s = p3_ping_read_slots(ram.data(), b114);
    CHECK(s.size() == 4 && s[0] == "P3A" && s[1].empty() && s[2] == "P3C" && s[3].empty(), "hole_in_slots: name cleared");
    CHECK(p3_ping_pick_base(ram.data(), "P3C", P3Prim::BadgeFrame, 2) == b114, "find 1.1.4 base");
    CHECK(p3_ping_pick_base(ram.data(), "P3B", P3Prim::BadgeFrame, 2) == 0, "a left player's stale id is no base");
    clear();
    put_id(b115, 1, "P3B"); put_name(b115, 1, "Edson"); put_id(b115, 0, "P3A"); put_name(b115, 0, "Edson");
    CHECK(p3_ping_pick_base(ram.data(), "P3B", P3Prim::BadgeFrame, 2) == b115, "find 1.1.5b base");
    clear();
    put_id(bfar, 2, "XY_9"); put_name(bfar, 2, "Hero");
    CHECK(p3_ping_pick_base(ram.data(), "XY_9", P3Prim::BadgeFrame, 1) == bfar, "search finds an unknown release's base");
    CHECK(p3_ping_pick_base(ram.data(), "NOPE", P3Prim::BadgeFrame, 1) == 0 &&
          p3_ping_pick_base(ram.data(), "", P3Prim::BadgeFrame, 1) == 0, "absent: 0");

    // pick_base: a second copy of our id with garbage before it (1.1.5b visitor, 2 Nov) must lose against the
    // table whose occupied slots match the anchors on screen
    clear();
    const uint32_t real = 0x0936DF94;
    put_id(real, 0, "P3A"); put_name(real, 0, "Edson");
    put_id(real, 1, "P3B"); put_name(real, 1, "Edson");
    put_id(0x0936E0AC, 0, "P3B");  // the other copy; the bytes 0x1128 below it are not empty
    ram[(0x0936E0AC - 0x1128) & 0x07FFFFFF] = 0x01;
    ram[(0x0936E0AC - 0x1128 + 1) & 0x07FFFFFF] = 0x20;
    CHECK(p3_ping_pick_base(ram.data(), "P3B", P3Prim::BadgeFrame, 2) == real, "pick the table that matches 2 badges");
    CHECK(p3_ping_pick_base(ram.data(), "P3B", P3Prim::BadgeFrame, 3) == 0, "no table matches 3 badges");
    CHECK(p3_ping_pick_base(ram.data(), "P3B", P3Prim::Mask, 2) == real, "masks too");
    put_id(real, 1, "P3 B"); put_name(real, 1, "Edson");
    CHECK(p3_ping_read_slots(ram.data(), real)[1].empty(), "not an online id (space): free");

    // assign
    // hideout: badges are drawn right to left over heroes standing (left to right) 3 2 0 1 -> slots 1 0 2 3
    const std::vector<std::string> two = {"P3A", "P3B", "", ""}, three = {"P3A", "P3B", "P3C", ""},
                                   four = {"P3A", "P3B", "P3C", "P3D"}, hole = {"P3A", "", "P3C", ""};
    CHECK(same(p3_ping_assign(P3Prim::BadgeFrame, 2, two, "P3A"), {1, 0}), "hideout 2p: the visitor's badge first");
    CHECK(same(p3_ping_assign(P3Prim::BadgeFrame, 3, three, "P3C"), {1, 0, 2}), "hideout 3p");
    CHECK(same(p3_ping_assign(P3Prim::BadgeFrame, 4, four, "P3D"), {1, 0, 2, 3}), "hideout 4p");
    CHECK(same(p3_ping_assign(P3Prim::BadgeFrame, 2, hole, "P3C"), {0, 2}), "hideout: a hole keeps the order");
    CHECK(p3_ping_assign(P3Prim::BadgeFrame, 3, hole, "P3C").empty(), "anchor count mismatch: nothing");
    // assign_mask_order: masks left to right = slots high to low, on every screen
    CHECK(same(p3_ping_assign(P3Prim::Mask, 2, two, "P3B"), {1, 0}), "masks 2p");
    CHECK(same(p3_ping_assign(P3Prim::Mask, 3, three, "P3A"), {2, 1, 0}), "masks 3p");
    CHECK(same(p3_ping_assign(P3Prim::Mask, 4, four, "P3C"), {3, 2, 1, 0}), "masks 4p");
    CHECK(same(p3_ping_assign(P3Prim::Mask, 3, three, "P3B", 1), {2, 1, 0}), "own gold mask where the rule puts it");
    CHECK(p3_ping_assign(P3Prim::Mask, 3, three, "P3B", 0).empty(), "own gold mask elsewhere: nothing");
    CHECK(p3_ping_assign(P3Prim::Mask, 2, three, "P3B").empty(), "mask_count_mismatch_draws_nothing");
    CHECK(p3_ping_assign(P3Prim::Mask, 3, three, "P3Z").empty(), "own not in the party: nothing");
    CHECK(p3_ping_assign(P3Prim::BadgeFrame, 0, three, "P3A").empty(), "no anchors: nothing");

    // bars and tags
    CHECK(p3_ping_bars(40) == 3 && p3_ping_bars(120) == 2 && p3_ping_bars(200) == 1 && p3_ping_bars(-1) == 0,
          "bars_from_ping");
    const WorldTag t = p3_ping_tag(P3Prim::BadgeFrame, {396, 174, 424, 190}, 60, false);
    CHECK(t.text == "60 ms" && !t.above && std::fabs(t.x - 410) < 0.01f && std::fabs(t.y - 192) < 0.01f &&
          t.icon == -1 && t.size_px == 0.0f, "tag under the badge, u15 look");
    CHECK(t.g > t.r && t.g > t.b, "green under 80");
    const WorldTag u = p3_ping_tag(P3Prim::BadgeFrame, {0, 0, 1, 1}, -1, true);
    CHECK(u.text.empty() && u.icon == -1, "unknown_ping_keeps_badge: no tag");
    // mask_tag_layout: digits only, small, icon = bars (+4 through the server), centred under the outline
    const WorldTag m = p3_ping_tag(P3Prim::Mask, {9, 27, 35, 38}, 60, false);
    CHECK(m.text == "60" && m.size_px == 7.5f && m.icon == 3 && std::fabs(m.x - 22) < 0.01f &&
          std::fabs(m.y - 39) < 0.01f && !m.above, "mask tag: 60 direct");
    const WorldTag r = p3_ping_tag(P3Prim::Mask, {37, 27, 63, 38}, 170, true);
    CHECK(r.text == "170" && r.icon == 5 && r.r > r.g && r.r > r.b, "red from 150, server set");
    const WorldTag y = p3_ping_tag(P3Prim::Mask, {37, 27, 63, 38}, 100, true);
    CHECK(y.icon == 6 && y.r > y.b && y.g > y.b, "yellow from 80, server set");
    // unknown_ping_grey_icon: the icon alone, all bars unlit, grey
    const WorldTag g = p3_ping_tag(P3Prim::Mask, {9, 27, 35, 38}, -1, false);
    CHECK(g.text.empty() && g.icon == 0 && g.r == 150 && g.g == 150 && g.b == 150, "unknown: grey icon, no number");
    CHECK(p3_ping_tag(P3Prim::Mask, {9, 27, 35, 38}, -1, true).icon == 4, "unknown through the server");

    // overlay: tags of heroes standing together stack instead of overlapping
    std::vector<TagRect> placed;
    const TagRect a = psp_tag_unstack({100, 50, 40, 10}, placed, true);
    const TagRect b = psp_tag_unstack({120, 50, 40, 10}, placed, true);
    const TagRect c = psp_tag_unstack({300, 50, 40, 10}, placed, true);
    CHECK(a.y == 50 && b.y == 40 && c.y == 50, "the second tag moves a line up, a far one stays");
    const TagRect d = psp_tag_unstack({110, 45, 40, 10}, placed, false);
    CHECK(d.y >= 60 && placed.size() == 4, "a below-tag moves down past both");
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("p3_ping: all tests passed\n");
    return 0;
}
