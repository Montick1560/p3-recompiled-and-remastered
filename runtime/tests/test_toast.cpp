// Toast texts and timing (psp_toast.h).
#include "psp_toast.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void one_each_up_to_three() {
    const auto t = psp_toasts_for({{"📜", "El despertar"}, {"🥁", "Primer tambor"}}, "es");
    CHECK(t.size() == 2, "two toasts");
    CHECK(t[0].title == "Logro desbloqueado" && t[0].name == "📜 El despertar", "spanish");
    const auto e = psp_toasts_for({{"", "First Drum"}}, "en");
    CHECK(e.size() == 1 && e[0].title == "Achievement unlocked" && e[0].name == "First Drum", "english, no icon");
    CHECK(psp_toasts_for({{"a", "1"}, {"b", "2"}, {"c", "3"}}, nullptr).size() == 3, "three stay separate");
    CHECK(psp_toasts_for({}, "es").empty(), "nothing");
}

static void summary_over_three() {
    const auto t = psp_toasts_for({{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}}, "es");
    CHECK(t.size() == 1 && t[0].title == "Logros desbloqueados" && t[0].name == "🏆 4 logros", "spanish summary");
    const auto e = psp_toasts_for(std::vector<std::pair<std::string, std::string>>(20, {"x", "y"}), "en");
    CHECK(e.size() == 1 && e[0].title == "Achievements unlocked" && e[0].name == "🏆 20 achievements", "english");
}

static void fade() {
    CHECK(psp_toast_alpha(0) == 0.0f, "starts transparent");
    CHECK(psp_toast_alpha(125) > 0.45f && psp_toast_alpha(125) < 0.55f, "fading in");
    CHECK(psp_toast_alpha(250) == 1.0f && psp_toast_alpha(3700) == 1.0f, "solid");
    CHECK(psp_toast_alpha(3850) > 0.45f && psp_toast_alpha(3850) < 0.55f, "fading out");
    CHECK(psp_toast_alpha(kToastMs) == 0.0f && psp_toast_alpha(99999) == 0.0f, "gone");
}

int main() {
    one_each_up_to_three();
    summary_over_three();
    fade();
    if (failures == 0) std::printf("test_toast: all passed\n");
    return failures == 0 ? 0 : 1;
}
