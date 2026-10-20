#pragma once
// Button prompts (pure): which icon a PSP help-bar glyph shows for the player's input device, the
// prompt style and the effective control map (spec: docs/superpowers/specs/
// 2026-10-19-pc-button-prompts-part1-design.md). Icon names are basenames of the embedded Kenney
// icons (psp_prompt_raster.h) or "@arrows_*" keycaps drawn by the runtime. An empty result means
// "keep the game's own glyph": there is no icon that is faithfully the button doing the action.
#include "psp_controls.h"
#include "psp_input_device.h"

#include <cstdint>
#include <string>
#include <vector>

enum class PromptGlyph : uint8_t {
    Cross, Circle, Square, Triangle,
    LR,             // one cell with both shoulder buttons ("L/R")
    Start, Select,  // the wide pills
    DpadAll, DpadVertical, DpadUp,  // D-pad with all / up+down / up highlighted
    StickVertical,  // analog stick with up / down arrows
};

enum class PromptStyle : uint8_t { Gray, Color };

// PSPRECOMP_BUTTON_ICONS: "color" -> Color, anything else (unset, "gray") -> Gray.
PromptStyle prompt_style_from_env(const char* value);

// Icon names for `g` (two for LR: L then R; one otherwise), empty = keep the game's glyph.
std::vector<std::string> prompt_icons(PromptGlyph g, InputDevice d, PromptStyle s, const ControlMap& map);

// Kenney keyboard icon for an SDL key name, "" when there is none. `wide` (the START / SELECT pills)
// prefers word keys (SPACE, TAB, ENTER...); narrow cells use symbols.
std::string prompt_key_icon(const std::string& sdl_key_name, bool wide);
