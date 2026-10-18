#include "psp_toast.h"

#include <cstring>

std::vector<ToastText> psp_toasts_for(const std::vector<std::pair<std::string, std::string>>& icon_name,
                                      const char* lang) {
    const bool en = lang && std::strcmp(lang, "en") == 0;
    std::vector<ToastText> out;
    if (icon_name.size() > 3) {
        const std::string n = std::to_string(icon_name.size());
        out.push_back({en ? "Achievements unlocked" : "Logros desbloqueados",
                       "\xF0\x9F\x8F\x86 " + n + (en ? " achievements" : " logros")});
        return out;
    }
    for (const auto& [icon, name] : icon_name)
        out.push_back({en ? "Achievement unlocked" : "Logro desbloqueado", icon.empty() ? name : icon + " " + name});
    return out;
}

float psp_toast_alpha(int64_t ms) {
    if (ms <= 0 || ms >= kToastMs) return 0.0f;
    if (ms < 250) return static_cast<float>(ms) / 250.0f;
    if (ms > kToastMs - 300) return static_cast<float>(kToastMs - ms) / 300.0f;
    return 1.0f;
}
