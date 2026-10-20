#pragma once
// Patapon 3's help-bar word tables (psp_bar_text.h): the labels of every language and release, as the
// game's atlas draws them, for the sharp bar text of the button prompts (p3_prompts.cpp).
#include "psp_prompt_atlas.h"

#include <vector>

// One table per (release, language) atlas whose label coverage differs.
const std::vector<BarLabels>& p3_bar_labels();
// The glyph row's separators ': / &' (the same in every language).
BarRow p3_bar_separators();
// The label areas the language signature covers (none of the atlas signature's rects).
std::vector<PromptRect> p3_label_sig_rects();
