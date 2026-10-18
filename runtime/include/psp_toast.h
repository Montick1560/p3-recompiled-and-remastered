#pragma once
// In-game achievement toasts (spec 2026-10-18 part 5b §4.4): the texts for one batch of achievements
// earned in the same check, and the fade timing. Pure.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ToastText { std::string title, name; };

constexpr int64_t kToastMs = 4000;

// One toast per achievement ("Logro desbloqueado", "<icon> <name>"), or, when the batch has more than
// 3, a single summary ("Logros desbloqueados", "🏆 N logros"). lang "en" = English, else Spanish.
std::vector<ToastText> psp_toasts_for(const std::vector<std::pair<std::string, std::string>>& icon_name,
                                      const char* lang);

// Opacity at `elapsed_ms` since the toast appeared: fades in over 250 ms, out over the last 300 ms,
// 0 from kToastMs on.
float psp_toast_alpha(int64_t elapsed_ms);
