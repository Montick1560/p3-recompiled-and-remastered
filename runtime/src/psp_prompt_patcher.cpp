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
bool g_sig_log = false;

bool candidate(const GeTexDecodeParams& p) {
    return p.width == g_spec.width && p.height == g_spec.height && p.format == g_spec.format;
}

bool recognise(const GeTexDecodeParams& p, const uint8_t* rgba) {
    const uint64_t sig = prompt_atlas_signature(rgba, p.width, g_spec.sig_rects);
    const bool ok = std::find(g_spec.sig_hashes.begin(), g_spec.sig_hashes.end(), sig) != g_spec.sig_hashes.end();
    if (g_sig_log) {
        std::fprintf(stderr, "[PROMPTS] candidate 0x%08X sig=%016llX %s\n", p.addr,
                     static_cast<unsigned long long>(sig), ok ? "accepted" : "rejected");
    }
    static std::set<uint32_t> logged;  // GE thread only
    if (ok && logged.insert(p.addr).second) {
        std::fprintf(stderr, "[PROMPTS] help-bar atlas recognised (%dx%d) at 0x%08X\n", p.width, p.height, p.addr);
    }
    return ok;
}

uint32_t variant() { return prompt_atlas_variant(psp_input_device(), g_style); }

void apply(uint32_t v, const GeTexDecodeParams&, RgbaImage& img) {
    InputDevice d;
    PromptStyle s;
    if (prompt_atlas_variant_decode(v, &d, &s)) prompt_atlas_compose(g_spec, d, s, psp_control_map(), img);
}

}  // namespace

void psp_prompts_register_atlas(PromptAtlasSpec spec) {
    const char* icons = std::getenv("PSPRECOMP_BUTTON_ICONS");
    if (icons != nullptr && std::strcmp(icons, "off") == 0) {
        std::fprintf(stderr, "[PROMPTS] button icons off: the game's PSP icons\n");
        return;
    }
    const bool first = g_spec.cells.empty();
    g_spec = std::move(spec);
    g_style = prompt_style_from_env(icons);
    const char* sig = std::getenv("PSPRECOMP_PROMPTS_SIG_LOG");
    g_sig_log = sig != nullptr && sig[0] == '1';
    if (first) ge_texpatch_register({candidate, recognise, variant, apply});
    std::fprintf(stderr, "[PROMPTS] help-bar button icons: %s, follow the last input device\n",
                 g_style == PromptStyle::Color ? "color" : "gray");
}
