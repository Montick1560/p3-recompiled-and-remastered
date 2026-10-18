#include "psp_menu_prompt.h"

#include <cstring>
#include <string>

bool ComboHold::update(bool held, int64_t now_ms) {
    if (!held) { since_ = -1; fired_ = false; return false; }
    if (since_ < 0) since_ = now_ms;
    if (!fired_ && now_ms - since_ >= hold_ms_) { fired_ = true; return true; }
    return false;
}

void MenuPrompt::open() { active_ = true; yes_ = false; }
bool MenuPrompt::active() const { return active_; }
bool MenuPrompt::yes_selected() const { return yes_; }

PromptResult MenuPrompt::handle(PromptAction a) {
    if (!active_) return PromptResult::None;
    switch (a) {
    case PromptAction::Left: yes_ = true; return PromptResult::None;
    case PromptAction::Right: yes_ = false; return PromptResult::None;
    case PromptAction::Up:
    case PromptAction::Down: yes_ = !yes_; return PromptResult::None;
    case PromptAction::Confirm:
        active_ = false;
        return yes_ ? PromptResult::Confirmed : PromptResult::Cancelled;
    case PromptAction::Cancel:
        active_ = false;
        return PromptResult::Cancelled;
    }
    return PromptResult::None;
}

PromptText menu_prompt_text(const char* lang) {
    if (lang && std::strcmp(lang, "en") == 0)
        return {"Back to the menu? Unsaved progress is lost.", "Yes", "No",
                "Arrows: choose    Enter / A: confirm    Esc / B: back"};
    return {"¿Volver al menú? Se perderá lo no guardado.", "Sí", "No",
            "Flechas: elegir    Enter / A: confirmar    Esc / B: volver"};
}

// "#rrggbb" -> opaque RGBA; anything else is rejected.
static bool parse_hex(const std::string& v, float out[4]) {
    if (v.size() != 7 || v[0] != '#') return false;
    unsigned rgb = 0;
    for (size_t i = 1; i < 7; i++) {
        const char c = v[i];
        const int d = c >= '0' && c <= '9' ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        rgb = rgb * 16 + static_cast<unsigned>(d);
    }
    out[0] = ((rgb >> 16) & 255) / 255.0f;
    out[1] = ((rgb >> 8) & 255) / 255.0f;
    out[2] = (rgb & 255) / 255.0f;
    out[3] = 1.0f;
    return true;
}

static void set(float dst[4], const float src[4], float alpha) {
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = alpha;
}

PromptColors menu_prompt_colors(const char* spec) {
    PromptColors c = {{0.07f, 0.06f, 0.10f, 0.95f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.75f, 0.75f, 0.80f, 1.0f},
                      {1.0f, 0.82f, 0.30f, 1.0f}, {1.0f, 0.82f, 0.30f, 0.25f}, {1.0f, 0.82f, 0.30f, 1.0f},
                      {1.0f, 1.0f, 1.0f, 1.0f}};
    if (!spec) return c;
    const std::string s(spec);
    bool themed = false;
    for (size_t at = 0; at <= s.size();) {
        size_t end = s.find(';', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        const size_t eq = item.find('=');
        float v[4];
        if (eq == std::string::npos || !parse_hex(item.substr(eq + 1), v)) continue;
        const std::string key = item.substr(0, eq);
        if (key == "panel") set(c.panel, v, 0.95f);
        else if (key == "panelText") { set(c.text, v, 1.0f); set(c.other_text, v, 1.0f); }
        else if (key == "muted") set(c.hint, v, 1.0f);
        else if (key == "accent") set(c.accent, v, 1.0f);
        else continue;
        themed = true;
    }
    if (themed) {  // the launcher's selected rows: black text on a solid accent
        set(c.choice_bg, c.accent, 1.0f);
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        set(c.choice_text, black, 1.0f);
    }
    return c;
}
