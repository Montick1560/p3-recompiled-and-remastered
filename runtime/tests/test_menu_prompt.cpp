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

static bool same(const float a[4], float r, float g, float b, float al) {
    const float want[4] = {r, g, b, al};
    for (int i = 0; i < 4; i++) if (a[i] < want[i] - 0.002f || a[i] > want[i] + 0.002f) return false;
    return true;
}

static void colors() {
    const PromptColors d = menu_prompt_colors(nullptr);
    CHECK(same(d.panel, 0.07f, 0.06f, 0.10f, 0.95f) && same(d.text, 1, 1, 1, 1), "built-in look without the launcher");
    CHECK(same(d.choice_bg, 1.0f, 0.82f, 0.30f, 0.25f) && same(d.choice_text, 1.0f, 0.82f, 0.30f, 1), "built-in choice");
    CHECK(same(menu_prompt_colors("").panel, 0.07f, 0.06f, 0.10f, 0.95f), "empty = built-in");
    // The launcher's Noche theme: its dialog look (panel, panelText, muted; the choice on accent, black text).
    const PromptColors n = menu_prompt_colors(
        "bg=#070912;text=#f2f0ff;accent=#8fd3ff;panel=#161a2e;panelText=#e8e8f5;muted=#8a8fb0");
    CHECK(same(n.panel, 0x16 / 255.0f, 0x1a / 255.0f, 0x2e / 255.0f, 0.95f), "panel");
    CHECK(same(n.text, 0xe8 / 255.0f, 0xe8 / 255.0f, 0xf5 / 255.0f, 1), "question in panelText");
    CHECK(same(n.other_text, 0xe8 / 255.0f, 0xe8 / 255.0f, 0xf5 / 255.0f, 1), "the other choice in panelText");
    CHECK(same(n.hint, 0x8a / 255.0f, 0x8f / 255.0f, 0xb0 / 255.0f, 1), "hint in muted");
    CHECK(same(n.accent, 0x8f / 255.0f, 0xd3 / 255.0f, 1, 1) && same(n.choice_bg, 0x8f / 255.0f, 0xd3 / 255.0f, 1, 1),
          "accent bar and the chosen button");
    CHECK(same(n.choice_text, 0, 0, 0, 1), "black on the accent, like the launcher's selected rows");
    const PromptColors bad = menu_prompt_colors("panel=red;accent=#12345;muted=#GG0000;=;;panelText");
    CHECK(same(bad.panel, 0.07f, 0.06f, 0.10f, 0.95f) && same(bad.accent, 1.0f, 0.82f, 0.30f, 1), "bad entries ignored");
    CHECK(same(bad.choice_text, 1.0f, 0.82f, 0.30f, 1), "nothing valid = built-in look");
    CHECK(same(menu_prompt_colors("ACCENT=#ffffff").accent, 1.0f, 0.82f, 0.30f, 1), "keys are case-sensitive");
}

int main() {
    combo();
    prompt();
    texts();
    colors();
    if (failures == 0) std::printf("test_menu_prompt: all passed\n");
    return failures == 0 ? 0 : 1;
}
