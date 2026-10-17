#pragma once
// Back to the launcher menu (docs/contracts/settings-v1.md, exit code 10): the Select+Start
// hold detector and the Yes/No prompt state. Pure: no SDL, no GL, no clock of its own.
#include <cstdint>

// true once when held >= hold_ms; re-arms on release.
class ComboHold {
public:
    explicit ComboHold(int64_t hold_ms) : hold_ms_(hold_ms) {}
    bool update(bool held, int64_t now_ms);
private:
    int64_t hold_ms_, since_ = -1;
    bool fired_ = false;
};

enum class PromptAction { Left, Right, Up, Down, Confirm, Cancel };
enum class PromptResult { None, Confirmed, Cancelled };

// "Back to the menu?" Yes / No. Opens on "No" so a stray confirm never quits.
class MenuPrompt {
public:
    void open();
    bool active() const;
    bool yes_selected() const;
    PromptResult handle(PromptAction a);
private:
    bool active_ = false, yes_ = false;
};

struct PromptText { const char* question; const char* yes; const char* no; const char* hint; };
PromptText menu_prompt_text(const char* lang);  // "en" -> English, anything else Spanish
