#include "psp_menu_prompt.h"

#include <cstring>

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
