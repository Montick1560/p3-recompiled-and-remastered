#pragma once
// The ping of each online partner (infrastructure mode): the game's per-hero signal badge (hideout) and the party
// masks' health bars (missions) are found in the GE stream, matched to party slots, and get the partner's ping in
// its colour; the badge's three bars become a real meter. Recon: build/ping_recon/FINDINGS.md, SLOTS.md, MASKS.md.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "psp_ge_observer.h"
#include "psp_osk_overlay.h"

enum class P3Prim { None, BadgeFrame, BadgeStrip, Mask };

// Which part of the hero badge a PRIM is (texture, CLUT, vertex type, count), or the 26x11 health-bar outline under
// a party mask (HUD atlas, top-left row; `box` in PSP pixels).
P3Prim p3_ping_classify(const GePrimInfo& p, const GePrimBox& box);

// Party slots read from guest RAM: online id per slot ("" = slot free). id_base = slot 0 of the online-id table
// (stride 0x118); the hero-name table (stride 0x23C) sits 0x1128 below and is the one cleared when a player leaves.
std::vector<std::string> p3_ping_read_slots(const uint8_t* rdram, uint32_t id_base);
// id_base whose occupied slots hold our own online id and match the `anchors` badges / labels on screen
// (p3_ping_assign): the known release bases first, else every place our id sits in 0x09300000-0x09400000 (the
// heap moves the table between releases and even between two instances). 0 when none fits.
uint32_t p3_ping_pick_base(const uint8_t* rdram, const std::string& own, P3Prim kind, size_t anchors);

// Slot index of each anchor in draw order; empty when the anchors cannot be matched to the occupied slots.
// Hideout badges: slots 1 0 2 3 (drawn right to left over heroes standing 3 2 0 1). Mission masks: slots high to
// low. own_anchor >= 0: the anchor the game marks as ours (gold mask); a rule that puts us elsewhere gives empty.
std::vector<int> p3_ping_assign(P3Prim kind, size_t anchors, const std::vector<std::string>& slots,
                                const std::string& own, int own_anchor = -1);

int p3_ping_bars(int ms);  // 3 / 2 / 1 bars for good / fair / poor, 0 = unknown (the game's own badge)
// The tag of one partner's anchor, centred under it. Badge: "N ms", nothing when unknown. Mask: small digits after
// the signal icon (frame = bars, +4 through the server); unknown: the grey icon alone.
WorldTag p3_ping_tag(P3Prim kind, const GePrimBox& box, int ms, bool via_server);

// Registers the GE observers unless PSPRECOMP_P3_SHOW_PING is "0"; tags appear only while connected online.
void p3_ping_register();
