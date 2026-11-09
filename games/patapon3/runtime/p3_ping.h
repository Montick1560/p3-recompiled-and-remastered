#pragma once
// The ping of each online partner (infrastructure mode). Hideout: the game's per-hero signal badge is found in the GE
// stream, matched to its party slot and gets the partner's ping; its three bars become a real meter. Missions and
// VS: a fixed strip (p3_ping_strip), drawn when the mask outline / VS HUD says which screen is on.
// Recon: build/ping_recon/FINDINGS.md, SLOTS.md, MASKS.md, STRIP.md.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "psp_ge_observer.h"
#include "psp_osk_overlay.h"

enum class P3Prim { None, BadgeFrame, BadgeStrip, Mask, VsHud };

// Which part of the hero badge a PRIM is (texture, CLUT, vertex type, count), or the 26x11 health-bar outline under
// a party mask (HUD atlas, top-left row; `box` in PSP pixels): drawn only in missions; or a fortress of the VS
// minimap (top band, y 30..50): drawn only in VS.
P3Prim p3_ping_classify(const GePrimInfo& p, const GePrimBox& box);

// Party slots read from guest RAM: online id per slot ("" = slot free). id_base = slot 0 of the online-id table
// (stride 0x118); the hero-name table (stride 0x23C) sits 0x1128 below and is the one cleared when a player leaves.
// 4 slots in the hideout and missions, 8 in VS (both armies, player number = slot + 1; STRIP.md).
std::vector<std::string> p3_ping_read_slots(const uint8_t* rdram, uint32_t id_base, int slots = 4);
// The hero name per slot (p3_ping_short_name), "" for a free slot.
std::vector<std::string> p3_ping_read_names(const uint8_t* rdram, uint32_t id_base, int slots = 4);
// id_base whose occupied slots hold our own online id and match the `anchors` badges on screen (p3_ping_assign);
// anchors 0 (missions, VS): our id in the first `slots` slots. The known release bases first (the live table: a hit
// there returns without a scan), else, of every place our id sits in 0x09300000-0x09400000 (the heap moves the table
// between releases and even between two instances), the one with the most players. 0 when none fits.
uint32_t p3_ping_pick_base(const uint8_t* rdram, const std::string& own, P3Prim kind, size_t anchors, int slots = 4);

// Slot index of each hideout badge in draw order: slots 1 0 2 3 (drawn right to left over heroes standing 3 2 0 1);
// empty when the badges cannot be matched to the occupied slots, or for any other kind.
std::vector<int> p3_ping_assign(P3Prim kind, size_t anchors, const std::vector<std::string>& slots,
                                const std::string& own);

int p3_ping_bars(int ms);  // 3 / 2 / 1 bars for good / fair / poor, 0 = unknown (the game's own badge)
// The tag under one partner's hideout badge: "N ms" in its colour, nothing when unknown.
WorldTag p3_ping_tag(const GePrimBox& box, int ms);

// Missions and VS: the partners' pings in a fixed strip (spec 2026-11-07-ping-strip-design.md), not tied to the
// masks, which vanish with their hero and flip order with the mission's direction.
struct P3Partner {
    int number;        // the game's player number, the "N." of the hero label (party slot + 1)
    std::string name;  // p3_ping_short_name of the hero name
    int ms;            // -1 = unknown
    bool via_server;
};
struct P3Zone {
    float x0, x1, y;   // PSP pixels: the free span and the text top
    float name_share;  // of a cell: the name before the split, icon + digits after it
};
extern const P3Zone kP3MissionZone;  // top row, between the party masks and the loot icons
extern const P3Zone kP3VsZone;       // under the minimap, between the FEVER worm and the score box
// Per partner (sorted by number) a white name tag ("N.Name", or "N" without names) ending at its cell's split and a
// ping tag (signal icon + digits, coloured; grey icon alone when unknown) starting 2 px after it. Equal cells across
// the zone, so an entry never moves when a number changes.
std::vector<WorldTag> p3_ping_strip(std::vector<P3Partner> partners, const P3Zone& zone, bool names);
// At most max_chars of a NUL-terminated UTF-16 name; the game font's heart (U+32A4) is dropped, anything else outside
// printable ASCII becomes '?' (the tag font).
std::string p3_ping_short_name(const char16_t* utf16, size_t max_chars = 6);

// Registers the GE observers unless PSPRECOMP_P3_SHOW_PING is "0"; tags appear only while connected online.
void p3_ping_register();
