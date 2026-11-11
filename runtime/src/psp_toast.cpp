#include "psp_toast.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace {

ToastText card(const ToastIn& a, const std::string& img_dir, bool en, bool (*exists)(const std::string&)) {
    const PspAchRarity& r = *a.rarity;
    ToastText t;
    t.title = en ? r.name_en + " achievement!" : "\xC2\xA1Logro " + r.name_es + "!";
    t.name = a.name;
    t.desc = a.desc;
    const std::string own = img_dir + a.id + ".jpg";
    t.image = exists(own) ? own : img_dir + "_default.jpg";
    t.rgb = r.rgb;
    t.ms = r.ms;
    return t;
}

// 0 -> 1 over [from, to] ms, exponential ease-out, exactly 1 at `to`.
float reveal(int64_t t, int64_t from, int64_t to) {
    if (t <= from) return 0.0f;
    if (t >= to) return 1.0f;
    const double x = static_cast<double>(t - from) / static_cast<double>(to - from);
    return static_cast<float>((1.0 - std::pow(2.0, -10.0 * x)) / (1.0 - std::pow(2.0, -10.0)));
}

}  // namespace

std::vector<ToastText> psp_ach_toasts(const std::vector<ToastIn>& in, const std::string& img_dir, const char* lang,
                                      bool (*exists)(const std::string&)) {
    const bool en = lang && std::strcmp(lang, "en") == 0;
    std::vector<ToastText> out;
    if (in.size() > 3) {
        const ToastIn& top = *std::max_element(in.begin(), in.end(), [](const ToastIn& a, const ToastIn& b) {
            return std::less<const PspAchRarity*>()(a.rarity, b.rarity);
        });
        ToastText t = card(top, img_dir, en, exists);
        const std::string n = std::to_string(in.size()), more = std::to_string(in.size() - 1);
        t.title = en ? n + " achievements!" : "\xC2\xA1" + n + " logros!";
        t.desc = en ? "and " + more + " more" : "y " + more + " m\xC3\xA1s";
        out.push_back(t);
        return out;
    }
    for (const ToastIn& a : in) out.push_back(card(a, img_dir, en, exists));
    return out;
}

ToastPose psp_toast_pose(int64_t t, int64_t ms) {
    ToastPose p{};
    // easeOutBack: 0 -> 1 by 600 ms, overshooting past 1 once (the card dips below its rest and settles).
    const double x = std::clamp(static_cast<double>(t) / 600.0, 0.0, 1.0), c1 = 1.70158, c3 = c1 + 1.0;
    const double back = 1.0 + c3 * std::pow(x - 1.0, 3.0) + c1 * std::pow(x - 1.0, 2.0);
    p.drop = x >= 1.0 ? 0.0f : static_cast<float>(-1.2 * (1.0 - back));
    p.medal = reveal(t, 150, 450);
    p.ribbon = reveal(t, 250, 650);
    p.rule = reveal(t, 400, 800);
    if (t >= ms) p.alpha = 0.0f;
    else if (t > ms - 300) {
        const float f = static_cast<float>(ms - t) / 300.0f;
        p.alpha = f * f;
    } else p.alpha = 1.0f;
    return p;
}

float psp_toast_alpha(int64_t t, int64_t ms) {
    if (t <= 0 || t >= ms) return 0.0f;
    if (t < 250) return static_cast<float>(t) / 250.0f;
    if (t > ms - 300) return static_cast<float>(ms - t) / 300.0f;
    return 1.0f;
}

std::vector<ToastIn> psp_ach_preview(const PspAchCatalogue& c, int step, const char* lang) {
    const bool en = lang && std::strcmp(lang, "en") == 0;
    auto sample = [&](const PspAchRarity& r) -> ToastIn {
        for (const PspAchEntry& e : c.entries)
            if (e.rarity == r.key) return {e.id, en ? e.name_en : e.name_es, en ? e.desc_en : e.desc_es, &r};
        return {r.key, en ? r.name_en : r.name_es, en ? "Preview" : "Vista previa", &r};
    };
    const size_t n = c.rarities.size(), k = static_cast<size_t>(std::max(step, 0)) % (n + 1);
    std::vector<ToastIn> out;
    if (k < n) out.push_back(sample(c.rarities[k]));
    else
        for (const PspAchRarity& r : c.rarities) out.push_back(sample(r));
    return out;
}
