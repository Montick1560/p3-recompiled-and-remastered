#pragma once
// In-game toasts (spec 2026-10-18 part 5b §4.4, 2026-11-08 rarity rework §3): the cards for one batch of
// achievements earned in the same check, and their motion. Pure.
#include "psp_achievements.h"

#include <cstdint>
#include <string>
#include <vector>

constexpr int64_t kToastMs = 4000;

// rgb == 0: a plain two-line toast (perf marks). rgb != 0: an achievement card in that tier colour, `title` =
// the ribbon text, `image` = the image file to show in the medallion.
struct ToastText {
    std::string title, name, desc, image;
    uint32_t rgb = 0;
    int64_t ms = kToastMs;
};

// One earned achievement, texts already in the UI language. `rarity` points into the catalogue's tier
// vector, so a later tier has a higher address (tier order = rank).
struct ToastIn { std::string id, name, desc; const PspAchRarity* rarity; };

// One card per achievement ("¡Logro Épico!"), or, when the batch has more than 3, one summary card
// ("¡5 logros!", the highest tier's name, colour, time and image, "y 4 más"). lang "en" = English, else
// Spanish. Image = img_dir + id + ".jpg" when exists() says so, else img_dir + "_default.jpg".
std::vector<ToastText> psp_ach_toasts(const std::vector<ToastIn>& in, const std::string& img_dir, const char* lang,
                                      bool (*exists)(const std::string&));

// Card motion at `elapsed_ms` of a toast lasting `ms`, in real time (smooth at any frame rate).
// drop: card offset in card heights, -1.2 -> 0 by 600 ms with one overshoot; medal/ribbon/rule: 0 -> 1
// reveals (150-450, 250-650, 400-800 ms); alpha: 1, eased out over the last 300 ms, 0 from `ms` on.
struct ToastPose { float drop, medal, ribbon, rule, alpha; };
ToastPose psp_toast_pose(int64_t elapsed_ms, int64_t ms);

// Plain toast opacity: fades in over 250 ms, out over the last 300 ms, 0 from `ms` on.
float psp_toast_alpha(int64_t elapsed_ms, int64_t ms);

// The F7 preview of the cards (PSPRECOMP_ACH_PREVIEW=1): step % (tiers + 1) < tiers = the first entry of that
// tier (a sample card named after the tier when it has none); the last step = one per tier at once.
std::vector<ToastIn> psp_ach_preview(const PspAchCatalogue& c, int step, const char* lang);
