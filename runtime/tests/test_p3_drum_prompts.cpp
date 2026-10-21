// Patapon 3's drum-hit words (p3_prompts.cpp): the four drum specs on the real dumps (DxD 1.1.4 es / en,
// build/prompts_ref/p3drums): each recognised as its own atlas, other 128x128 CLUT4 textures not; a composed
// word keeps every pixel outside its cell and ring, the cell is opaque (no hole in the black disc) and holds
// the new icon, at x1 and x4 and on the HD pack's x8 images.
#include "p3_prompts.h"
#include "psp_ge_constants.h"
#include "psp_prompt_atlas.h"
#include "psp_prompt_raster.h"
#include "psp_texrep_png.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::vector<PromptAtlasSpec> g_specs;
void psp_prompts_register_atlas(PromptAtlasSpec spec) { g_specs.push_back(std::move(spec)); }

static const std::map<std::string, int32_t> kKeys = {
    {"up", 1}, {"down", 2}, {"left", 3}, {"right", 4}, {"x", 5}, {"return", 6}, {"z", 7}, {"a", 8},
    {"s", 9}, {"q", 10}, {"w", 11}, {"space", 12}, {"v", 13}, {"tab", 14}};
static const std::map<std::string, int> kPads = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"start", 6}, {"leftstick", 7}, {"rightstick", 8},
    {"leftshoulder", 9}, {"rightshoulder", 10}, {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14}};
static std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
static ControlMap defaults() {
    std::vector<std::string> log;
    return ControlMap::build(
        "", [](const std::string& n) { auto it = kKeys.find(lower(n)); return it == kKeys.end() ? 0 : it->second; },
        [](const std::string& n) { auto it = kPads.find(lower(n)); return it == kPads.end() ? -1 : it->second; }, &log);
}

static bool load(const std::filesystem::path& p, RgbaImage* img) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)), {});
    return ge_texrep_decode_png(png.data(), png.size(), &img->w, &img->h, &img->px);
}

static int spec_index(const char* name) {
    for (size_t i = 0; i < g_specs.size(); i++)
        if (std::strcmp(g_specs[i].name, name) == 0) return static_cast<int>(i);
    return -1;
}

// Composes `img` (spec-sized x k) for a device / style and checks the cell + ring and everything outside.
static void check_compose(const PromptAtlasSpec& s, const RgbaImage& base, InputDevice d, PromptStyle st,
                          const char* what) {
    RgbaImage img = base;
    prompt_atlas_compose(s, d, st, defaults(), img);
    const int k = img.w / s.width, m = k / 2;
    const PromptCell& c = s.cells[0];
    bool opaque = true, ink = false, outside_same = true;
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
            const size_t i = (static_cast<size_t>(y) * img.w + x) * 4;
            const bool in = x >= c.x * k - m && x < (c.x + c.w) * k + m && y >= c.y * k - m && y < (c.y + c.h) * k + m;
            if (in) {
                opaque &= img.px[i + 3] == 0xFF;
                ink |= std::max({img.px[i], img.px[i + 1], img.px[i + 2]}) > 0x60;
            } else {
                outside_same &= std::equal(&img.px[i], &img.px[i] + 4, &base.px[i]);
            }
        }
    if (!opaque) std::printf("  %s: cell or ring not opaque\n", what);
    if (!ink) std::printf("  %s: no icon ink\n", what);
    if (!outside_same) std::printf("  %s: pixels outside the cell changed\n", what);
    CHECK(opaque && ink && outside_same, "drum word composed: opaque cell with an icon, nothing else touched");
}

// A pack image's old symbol (near-white texels) must lie inside the cleared cell + ring.
static void check_pack_symbol_covered(const PromptAtlasSpec& s, const RgbaImage& img, const char* what) {
    const int k = img.w / s.width, m = k / 2;
    const PromptCell& c = s.cells[0];
    bool covered = true;
    for (int y = (c.y - 4) * k; y < (c.y + c.h + 4) * k; y++)
        for (int x = (c.x - 4) * k; x < (c.x + c.w + 4) * k; x++) {
            const uint8_t* p = &img.px[(static_cast<size_t>(y) * img.w + x) * 4];
            const bool white = p[3] > 128 && std::min({p[0], p[1], p[2]}) > 128 &&
                               std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}) < 40;
            const bool in = x >= c.x * k - m && x < (c.x + c.w) * k + m && y >= c.y * k - m && y < (c.y + c.h) * k + m;
            covered &= !white || in;
        }
    if (!covered) std::printf("  %s: the pack's symbol leaves the cell\n", what);
    CHECK(covered, "the HD pack's symbol lies inside the cell + ring");
}

int main() {
    p3_prompts_register();
    const char* expected[] = {"help-bar", "command-list", "drum-pata", "drum-pon", "drum-chaka", "drum-don"};
    CHECK(g_specs.size() == 6, "six prompt atlases");
    for (size_t i = 0; i < 6 && i < g_specs.size(); i++) CHECK(std::strcmp(g_specs[i].name, expected[i]) == 0, "spec order");
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    // PSP_P3_DRUMS_DIR (env) overrides the recon dumps' folder (build/prompts_ref/p3drums).
    const char* env_dir = std::getenv("PSP_P3_DRUMS_DIR");
    const std::filesystem::path dir = env_dir != nullptr ? env_dir : PSP_P3_DRUMS_DIR;
    if (!std::filesystem::is_directory(dir / "en")) {
        std::printf("test_p3_drum_prompts: recon dumps not on this PC (%s), dump checks skipped\n", dir.string().c_str());
        return 0;
    }
    const std::map<std::string, std::string> pack = {{"pata", "00000000926ff6d0d54d2415.png"},
                                                     {"pon", "00000000790fa5b59568b857.png"},
                                                     {"don", "00000000871b192d7e73949f.png"},
                                                     {"chaka", "000000009427cb08fa48ba64.png"}};
    int dumps = 0, others = 0;
    for (const std::filesystem::path& sub : {dir, dir / "en"})
        for (const auto& e : std::filesystem::directory_iterator(sub)) {
            std::string n = e.path().filename().string();
            if (e.path().extension() != ".png" || n.find("_04") == std::string::npos) continue;
            if (n.rfind("en_", 0) == 0) n = n.substr(3);
            const std::string drum = n.substr(0, n.find('_'));
            if (drum != "other" && spec_index(("drum-" + drum).c_str()) < 0) continue;  // not a dump
            RgbaImage img;
            CHECK(load(e.path(), &img) && img.w == 128 && img.h == 128, "a 128x128 dump");
            const uint32_t tag = prompt_atlases_recognise(g_specs, 128, 128, GE_TFMT_CLUT4, img.px.data());
            if (drum == "other") {
                others++;
                CHECK(tag == 0, "a non-drum 128x128 CLUT4 texture is not recognised");
                continue;
            }
            const int idx = spec_index(("drum-" + drum).c_str());
            dumps++;
            const std::string what = e.path().filename().string();
            CHECK(tag == static_cast<uint32_t>(idx) * kPromptAtlasTagStride + 1, "a drum dump -> its drum spec");
            const PromptAtlasSpec& s = g_specs[idx];
            for (int k : {1, 4}) {
                const RgbaImage base = k == 1 ? img : rgba_resample(img, 128 * k, 128 * k);
                check_compose(s, base, InputDevice::Keyboard, PromptStyle::Gray, (what + " keyboard").c_str());
                check_compose(s, base, InputDevice::Xbox, PromptStyle::Color, (what + " xbox").c_str());
            }
            RgbaImage hd;
            if (load(dir / "pack_img" / pack.at(drum), &hd)) {
                check_pack_symbol_covered(s, hd, (drum + " pack").c_str());
                check_compose(s, hd, InputDevice::Xbox, PromptStyle::Color, (drum + " pack").c_str());
            }
        }
    CHECK(dumps == 8, "four drums, es and en");
    CHECK(others >= 1, "a non-drum dump to reject");
    std::printf("%d dump(s), %d other(s), %d failure(s)\n", dumps, others, failures);
    return failures == 0 ? 0 : 1;
}
