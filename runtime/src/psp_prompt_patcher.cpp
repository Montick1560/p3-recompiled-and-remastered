// The button-prompt texture patcher: psp_prompts_register_atlas() (psp_prompt_atlas.h) wires a game's
// help-bar atlas into the texture patch seam (psp_ge_texpatch.h). Kept apart from the pure
// composition in psp_prompt_atlas.cpp because it reads the runtime's control map and input device.
#include "psp_ge_texpatch.h"
#include "psp_prompt_atlas.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace {

PromptAtlasSpec g_spec;
PromptStyle g_style = PromptStyle::Gray;
bool g_icons = true, g_text = true;
bool g_sig_log = false;

bool candidate(const GeTexDecodeParams& p) {
    return p.width == g_spec.width && p.height == g_spec.height && p.format == g_spec.format;
}

uint32_t recognise(const GeTexDecodeParams& p, const uint8_t* rgba) {
    const uint32_t tag = prompt_atlas_recognise(g_spec, rgba, p.width);
    if (g_sig_log) {
        std::fprintf(stderr, "[PROMPTS] candidate 0x%08X sig=%016llX labels=%016llX %s\n", p.addr,
                     static_cast<unsigned long long>(prompt_atlas_signature(rgba, p.width, g_spec.sig_rects)),
                     static_cast<unsigned long long>(prompt_atlas_signature(rgba, p.width, g_spec.label_sig_rects)),
                     tag != 0 ? "accepted" : "rejected");
    }
    static std::set<std::pair<uint32_t, uint32_t>> logged;  // GE thread only
    if (tag != 0 && logged.insert({p.addr, tag}).second) {
        std::fprintf(stderr, "[PROMPTS] help-bar atlas recognised (%dx%d) at 0x%08X, labels %s\n", p.width, p.height,
                     p.addr, tag >= 2 ? g_spec.labels[tag - 2].name : "unknown (the game's text stays)");
    }
    return tag;
}

uint32_t variant() { return prompt_atlas_variant(psp_input_device(), g_style, g_icons, g_text); }

void apply(uint32_t v, const GeTexDecodeParams&, RgbaImage& img) { prompt_atlas_apply(g_spec, v, psp_control_map(), img); }

bool env_off(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "off") == 0;
}

}  // namespace

void psp_prompts_register_atlas(PromptAtlasSpec spec) {
    g_icons = !env_off("PSPRECOMP_BUTTON_ICONS");
    g_text = !env_off("PSPRECOMP_BAR_TEXT") && bar_font_ok();
    if (!g_icons && !g_text) {
        std::fprintf(stderr, "[PROMPTS] button icons and bar text off: the game's help bars\n");
        return;
    }
    const bool first = g_spec.cells.empty();
    g_spec = std::move(spec);
    g_style = prompt_style_from_env(std::getenv("PSPRECOMP_BUTTON_ICONS"));
    const char* sig = std::getenv("PSPRECOMP_PROMPTS_SIG_LOG");
    g_sig_log = sig != nullptr && sig[0] == '1';
    if (first) ge_texpatch_register({candidate, recognise, variant, apply});
    std::fprintf(stderr, "[PROMPTS] help-bar button icons: %s; bar text: %s (%zu label tables)\n",
                 !g_icons ? "off (PSP icons)" : g_style == PromptStyle::Color ? "color" : "gray",
                 g_text ? "on" : "off (the game's labels)", g_spec.labels.size());
}
