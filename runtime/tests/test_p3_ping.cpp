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
    GePrimInfo fort;  // VS minimap fortress sprite (STRIP.md)
    fort.prim = 6; fort.count = 2; fort.vtype = 0x800102; fort.texture = true;
    fort.tex_addr = 0x1446C0; fort.tex_size = 0x0505; fort.tex_fmt = 4; fort.clut_addr = 0x144900;
    CHECK(p3_ping_classify(fort, {224, 30, 256, 50}) == P3Prim::VsHud, "classify_vs_hud: centre fort");
    CHECK(p3_ping_classify(fort, {107, 35, 133, 50}) == P3Prim::VsHud, "classify_vs_hud: tower");
    CHECK(p3_ping_classify(fort, {224, 130, 256, 150}) == P3Prim::None, "classify_vs_hud: elsewhere, not the minimap");
    fort.clut_addr = 0x144500;
    CHECK(p3_ping_classify(fort, {224, 30, 256, 50}) == P3Prim::None, "classify_vs_hud: another CLUT");

    // slots: 1.1.4 base, slot 1 left (name cleared, stale id kept)
    const uint32_t b114 = 0x0936E114, b115 = 0x0936DFD4, bfar = 0x09370000;
    clear();
    put_id(b114, 0, "P3A"); put_name(b114, 0, "Edson");
    put_id(b114, 1, "P3B");
    put_id(b114, 2, "P3C"); put_name(b114, 2, "Edson");
    const std::vector<std::string> s = p3_ping_read_slots(ram.data(), b114);
    CHECK(s.size() == 4 && s[0] == "P3A" && s[1].empty() && s[2] == "P3C" && s[3].empty(), "hole_in_slots: name cleared");
    CHECK(p3_ping_read_names(ram.data(), b114)[1].empty(), "strip_skips_left_player: no name either");
    // VS: the same table holds 8 players (STRIP.md), own may sit in slot 4-7
    put_id(b114, 5, "P3F"); put_name(b114, 5, "Gold");
    CHECK(p3_ping_read_slots(ram.data(), b114, 8)[5] == "P3F" && p3_ping_read_names(ram.data(), b114, 8)[5] == "Gold" &&
          p3_ping_read_slots(ram.data(), b114).size() == 4, "vs_eight_slots");
    CHECK(p3_ping_pick_base(ram.data(), "P3F", P3Prim::None, 0, 8) == b114, "pick_base: own in a VS slot");
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

    // a stale hideout copy at the known base (own + 1) loses against the VS table found by the scan (own + 7)
    clear();
    put_id(b114, 0, "P3E"); put_name(b114, 0, "Edson"); put_id(b114, 1, "P3F"); put_name(b114, 1, "Edson");
    for (int i = 0; i < 8; i++) {
        const char ids[8][4] = {"P3A", "P3B", "P3C", "P3D", "P3E", "P3F", "P3G", "P3H"};
        put_id(bfar, i, ids[i]); put_name(bfar, i, "Edson");
    }
    CHECK(p3_ping_pick_base(ram.data(), "P3E", P3Prim::None, 0, 8) == b114,
          "pick_base: a known base holding our id wins, no scan (live table of both releases)");
    // VS -> mission: a stale 8-player copy (own in slot 4) must not be picked for a mission (4 slots)
    clear();
    for (int i = 0; i < 8; i++) {
        const char ids[8][4] = {"P3A", "P3B", "P3C", "P3D", "P3E", "P3F", "P3G", "P3H"};
        put_id(bfar, i, ids[i]); put_name(bfar, i, "Edson");
    }
    const uint32_t mreal = 0x09380000;
    put_id(mreal, 0, "P3E"); put_name(mreal, 0, "Edson"); put_id(mreal, 1, "P3Z"); put_name(mreal, 1, "Edson");
    CHECK(p3_ping_pick_base(ram.data(), "P3E", P3Prim::None, 0, 4) == mreal, "pick_base: mission ignores VS slots 4-7");

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
    CHECK(p3_ping_pick_base(ram.data(), "P3B", P3Prim::None, 0) == real, "pick_base_without_anchors: the fuller table");
    CHECK(p3_ping_pick_base(ram.data(), "NOPE", P3Prim::None, 0) == 0, "pick_base_without_anchors: absent");
    put_name(real, 0, "Krish2long");
    const std::vector<std::string> nm = p3_ping_read_names(ram.data(), real);
    CHECK(nm.size() == 4 && nm[0] == "Krish2" && nm[1] == "Edson" && nm[2].empty() && nm[3].empty(), "read_names");
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
    CHECK(p3_ping_assign(P3Prim::BadgeFrame, 0, three, "P3A").empty(), "no anchors: nothing");

    // bars and tags
    CHECK(p3_ping_bars(40) == 3 && p3_ping_bars(120) == 2 && p3_ping_bars(200) == 1 && p3_ping_bars(-1) == 0,
          "bars_from_ping");
    const WorldTag t = p3_ping_tag({396, 174, 424, 190}, 60);
    CHECK(t.text == "60 ms" && !t.above && std::fabs(t.x - 410) < 0.01f && std::fabs(t.y - 192) < 0.01f &&
          t.icon == -1 && t.size_px == 0.0f, "tag under the badge, u15 look");
    CHECK(t.g > t.r && t.g > t.b, "green under 80");
    const WorldTag u = p3_ping_tag({0, 0, 1, 1}, -1);
    CHECK(u.text.empty() && u.icon == -1, "unknown_ping_keeps_badge: no tag");
    CHECK(p3_ping_tag({0, 0, 1, 1}, 200).r == 240, "red from 150");

    // strip: per partner a name tag ending at the cell's split and a ping tag starting there, sorted by number
    const P3Zone z{124, 304, 13, 0.6f};
    const float cell = 180.0f / 3;
    auto st = p3_ping_strip({{4, "Fuego", 197, false}, {2, "Blue", 10, false}, {3, "Krish2", 59, true}}, z, true);
    CHECK(st.size() == 6 && st[0].text == "2.Blue" && st[2].text == "3.Krish2" && st[4].text == "4.Fuego",
          "sorted_by_number");
    CHECK(st[1].text == "10" && st[3].text == "59" && st[5].text == "197", "digits only");
    CHECK(st[0].align == 1.0f && st[1].align == 0.0f && std::fabs(st[1].x - st[0].x - 2) < 0.01f, "name | ping split");
    CHECK(std::fabs(st[2].x - (124 + cell + cell * 0.6f)) < 0.01f && st[0].y == 13 && st[1].y == 13,
          "equal_cells");
    CHECK(st[1].icon == 3 && st[3].icon == 3 + 4 && st[5].icon == 1 && st[0].icon == -1, "icons on the ping tag");
    CHECK(st[1].g > st[1].r && st[5].r > st[5].g && st[0].r == 255 && st[0].g == 255, "ping coloured, name white");
    CHECK(st[0].size_px == 7.5f && st[1].size_px == 7.5f, "small text");
    auto vs = p3_ping_strip({{2, "Blue", 85, false}}, kP3VsZone, false);
    CHECK(vs.size() == 2 && vs[0].text == "2" && vs[1].text == "85", "vs_number_only");
    auto uk = p3_ping_strip({{5, "Max", -1, false}}, kP3VsZone, false);
    CHECK(uk[1].text.empty() && uk[1].icon == 0 && uk[1].r == 150, "strip_unknown_ping_grey");
    CHECK(p3_ping_strip({}, z, true).empty(), "no_partners_no_tags");
    CHECK(p3_ping_short_name(u"Krish2long") == "Krish2" && p3_ping_short_name(u"Ñandú") == "?and?" &&
          p3_ping_short_name(u"").empty() && p3_ping_short_name(u"Edson㊤") == "Edson", "short_name: game heart dropped");
    // cells_do_not_overlap: glyphs ~4.4 PSP px at 7.5 px (p3_ping.cpp), icon 7 + 1, 2 px after the split.
    const float mc = (kP3MissionZone.x1 - kP3MissionZone.x0) / 3, vc = (kP3VsZone.x1 - kP3VsZone.x0) / 7;
    CHECK(mc * kP3MissionZone.name_share >= 8 * 4.4f && mc * (1 - kP3MissionZone.name_share) >= 2 + 8 + 3 * 4.4f,
          "cells_do_not_overlap: mission, 3 partners, \"4.Krish2\" + \"197\"");
    CHECK(vc * kP3VsZone.name_share >= 4.4f && vc >= 4.4f + 2 + 8 + 3 * 4.4f, "cells_do_not_overlap: VS, 7 players");

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
