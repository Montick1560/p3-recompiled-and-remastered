#pragma once
// Patapon 3's mission command-list word tables (psp_bar_text.h): the command names of every language and
// release, as the game's 128x64 command-list atlas draws them, for the sharp text of the button prompts
// (p3_prompts.cpp).
#include "psp_prompt_atlas.h"

#include <vector>

// One table per (release, language) atlas whose label coverage differs.
const std::vector<BarLabels>& p3_command_labels();
// The glyph row's separator ':' (the same in every language; the '·' stays the game's).
BarRow p3_command_separators();
// The label areas the language signature covers (none of the atlas signature's rect {0, 53, 58, 11}).
std::vector<PromptRect> p3_command_label_sig_rects();
