// Back-to-menu combo and prompt (psp_menu_prompt.h): Select+Start held 1 s opens a Yes/No
// prompt that starts on "No".
#include "psp_menu_prompt.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void combo() {
    ComboHold c(1000);
    CHECK(!c.update(true, 0), "press starts the clock");
    CHECK(!c.update(true, 999), "999 ms: not yet");
    CHECK(c.update(true, 1000), "1000 ms: fires");
    CHECK(!c.update(true, 3000), "fires once while held");
    CHECK(!c.update(false, 3100), "release");
    CHECK(!c.update(true, 3200), "press again");
    CHECK(!c.update(false, 3800), "tap released at 600 ms: never fires");
    CHECK(!c.update(true, 4000) && c.update(true, 5000), "re-armed after release");
}

static void prompt() {
    MenuPrompt p;
    CHECK(!p.active(), "closed by default");
    CHECK(p.handle(PromptAction::Confirm) == PromptResult::None, "input while closed ignored");
    p.open();
    CHECK(p.active() && !p.yes_selected(), "opens on No");
    CHECK(p.handle(PromptAction::Confirm) == PromptResult::Cancelled && !p.active(),
          "confirm on No = cancel, closes");
    p.open();
    p.handle(PromptAction::Left);
    CHECK(p.yes_selected(), "left moves to Yes");
    p.handle(PromptAction::Right);
    CHECK(!p.yes_selected(), "right moves to No");
    p.handle(PromptAction::Up);
    CHECK(p.yes_selected(), "up toggles too");
    CHECK(p.handle(PromptAction::Confirm) == PromptResult::Confirmed && !p.active(), "confirm on Yes");
    p.open();
    p.handle(PromptAction::Left);
    CHECK(p.handle(PromptAction::Cancel) == PromptResult::Cancelled, "cancel always cancels");
}

static void texts() {
    CHECK(std::strcmp(menu_prompt_text("en").yes, "Yes") == 0, "English");
    CHECK(std::strcmp(menu_prompt_text("es").yes, "Sí") == 0, "Spanish");
    CHECK(std::strcmp(menu_prompt_text(nullptr).yes, "Sí") == 0, "default Spanish");
}

int main() {
    combo();
    prompt();
    texts();
    if (failures == 0) std::printf("test_menu_prompt: all passed\n");
    return failures == 0 ? 0 : 1;
}
