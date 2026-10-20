// The button-prompt texture patcher: psp_prompts_register_atlas() (psp_prompt_atlas.h) wires a game's
// prompt atlases (help bar, command list...) into the texture patch seam (psp_ge_texpatch.h) as one
// patcher. Kept apart from the pure composition in psp_prompt_atlas.cpp because it reads the
// runtime's control map and input device.
#include "psp_ge_texpatch.h"
#include "psp_prompt_atlas.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<PromptAtlasSpec> g_specs;  // registered before the game boots; read by the GE thread
PromptStyle g_style = PromptStyle::Gray;
bool g_icons = true, g_text = true;
bool g_sig_log = false;

bool candidate(const GeTexDecodeParams& p) {
    for (const PromptAtlasSpec& s : g_specs)
        if (p.width == s.width && p.height == s.height && p.format == s.format) return true;
    return false;
}

uint32_t recognise(const GeTexDecodeParams& p, const uint8_t* rgba) {
    const uint32_t tag = prompt_atlases_recognise(g_specs, p.width, p.height, p.format, rgba);
    if (g_sig_log) {
        for (const PromptAtlasSpec& s : g_specs) {
            if (p.width != s.width || p.height != s.height || p.format != s.format) continue;
            std::fprintf(stderr, "[PROMPTS] candidate 0x%08X (%s) sig=%016llX labels=%016llX %s\n", p.addr, s.name,
                         static_cast<unsigned long long>(prompt_atlas_signature(rgba, p.width, s.sig_rects)),
                         static_cast<unsigned long long>(prompt_atlas_signature(rgba, p.width, s.label_sig_rects)),
                         tag != 0 ? "accepted" : "rejected");
        }
    }
    static std::set<std::pair<uint32_t, uint32_t>> logged;  // GE thread only
    if (tag != 0 && logged.insert({p.addr, tag}).second) {
        const PromptAtlasSpec& s = g_specs[tag / kPromptAtlasTagStride];
        const uint32_t local = tag % kPromptAtlasTagStride;
        std::fprintf(stderr, "[PROMPTS] %s atlas recognised (%dx%d) at 0x%08X, labels %s\n", s.name, p.width, p.height,
                     p.addr, local >= 2 ? s.labels[local - 2].name : "unknown (the game's text stays)");
    }
    return tag;
}

uint32_t variant() { return prompt_atlas_variant(psp_input_device(), g_style, g_icons, g_text); }

void apply(uint32_t v, const GeTexDecodeParams&, RgbaImage& img) { prompt_atlases_apply(g_specs, v, psp_control_map(), img); }

bool env_off(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "off") == 0;
}

}  // namespace

void psp_prompts_register_atlas(PromptAtlasSpec spec) {
    g_icons = !env_off("PSPRECOMP_BUTTON_ICONS");
    g_text = !env_off("PSPRECOMP_BAR_TEXT") && bar_font_ok();
    if (!g_icons && !g_text) {
        std::fprintf(stderr, "[PROMPTS] button icons and bar text off: the game's %s atlas stays\n", spec.name);
        return;
    }
    const bool first = g_specs.empty();
    auto same = std::find_if(g_specs.begin(), g_specs.end(),
                             [&](const PromptAtlasSpec& s) { return std::strcmp(s.name, spec.name) == 0; });
    if (same != g_specs.end()) {
        *same = std::move(spec);
    } else if (g_specs.size() < static_cast<size_t>(kPromptAtlasMax)) {
        g_specs.push_back(std::move(spec));
    } else {
        std::fprintf(stderr, "[PROMPTS] more than %d prompt atlases: %s ignored\n", kPromptAtlasMax, spec.name);
        return;
    }
    g_style = prompt_style_from_env(std::getenv("PSPRECOMP_BUTTON_ICONS"));
    const char* sig = std::getenv("PSPRECOMP_PROMPTS_SIG_LOG");
    g_sig_log = sig != nullptr && sig[0] == '1';
    if (first) ge_texpatch_register({candidate, recognise, variant, apply});
    std::string names;
    size_t tables = 0;
    for (const PromptAtlasSpec& s : g_specs) {
        names += names.empty() ? s.name : std::string(", ") + s.name;
        tables += s.labels.size();
    }
    std::fprintf(stderr, "[PROMPTS] %s button icons: %s; bar text: %s (%zu label tables)\n", names.c_str(),
                 !g_icons ? "off (PSP icons)" : g_style == PromptStyle::Color ? "color" : "gray",
                 g_text ? "on" : "off (the game's labels)", tables);
}
