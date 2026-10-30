// Unit tests for settings.ini v1 (docs/contracts/settings-v1.md): the INI parser and the
// resolution of keys into PSPRECOMP_* environment variables.
#include "psp_ini.h"
#include "psp_settings.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static const std::string kFix = std::string(PSP_CONTRACT_DIR) + "/fixtures/";
static const std::string kCatalog = slurp(PSP_GAME_OPTIONS_JSON);

// env name -> value of a resolution; "<unset>" when not set.
static std::string val(const SettingsResolution& r, const char* env) {
    std::string v = "<unset>";
    for (const auto& kv : r.set) if (kv.first == env) v = kv.second;
    return v;
}
static bool logged(const SettingsResolution& r, const char* needle) {
    for (const auto& l : r.log) if (l.find(needle) != std::string::npos) return true;
    return false;
}
static const EnvGetter kNoEnv = [](const char*) -> const char* { return nullptr; };

static void parser_rules() {
    IniDoc d = psp_ini_parse("; c\n# c\n[Display]\n Render_Scale = 4 \nx=a=b\nbad line\n[online]\nnick=\n");
    CHECK(d.get("display", "render_scale") && *d.get("display", "render_scale") == "4",
          "sections and keys are lowercased, values trimmed");
    CHECK(d.get("display", "x") && *d.get("display", "x") == "a=b", "split at the first '='");
    CHECK(d.get("online", "nick") && d.get("online", "nick")->empty(), "empty value kept");
    CHECK(d.warnings.size() == 1, "one warning for the line without '='");
    d = psp_ini_parse("[a]\nk = 1\nk = 2\n");
    CHECK(*d.get("a", "k") == "2", "repeated key keeps the last value");
    d = psp_ini_parse(slurp(kFix + "bom_crlf.ini"));
    CHECK(d.get("display", "render_scale") && *d.get("display", "render_scale") == "5",
          "BOM and CRLF are accepted");
    CHECK(*d.get("online", "nickname") == "\xC3\x91" "and\xC3\xBA", "UTF-8 value kept");
}

static void defaults_fixture() {
    const SettingsResolution r = psp_settings_resolve(slurp(kFix + "defaults.ini"), kCatalog,
                                                      "C:/Juegos/Patapon 3", kNoEnv);
    CHECK(val(r, "PSPRECOMP_UI_LANG") == "es", "language");
    CHECK(val(r, "PSPRECOMP_DISC0") == "C:/Juegos/Patapon 3/disc0", "disc0 resolved (spaces)");
    CHECK(val(r, "PSPRECOMP_SAVEDATA") == "C:/Juegos/Patapon 3/savedata", "savedata resolved");
    CHECK(val(r, "PSPRECOMP_FULLSCREEN") == "1", "fullscreen true -> 1");
    CHECK(val(r, "PSPRECOMP_WINDOW_MODE") == "fullscreen", "window mode");
    CHECK(val(r, "PSPRECOMP_WINDOW_SCALE") == "2", "window scale");
    CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "4", "render scale");
    CHECK(val(r, "PSPRECOMP_PRESENT_FILTER") == "linear", "present filter");
    CHECK(val(r, "PSPRECOMP_TEX_FILTER") == "auto", "texture filter");
    CHECK(val(r, "PSPRECOMP_TEXTURES") == "C:/Juegos/Patapon 3/textures/INFN00001", "pack on");
    CHECK(val(r, "PSPRECOMP_PATCHES") == "<unset>", "fps 30 -> no patch");
    CHECK(val(r, "PSPRECOMP_P3_ANIM_SMOOTH") == "1", "smooth on");
    CHECK(val(r, "PSPRECOMP_P3_FAST_LOAD") == "1", "fast load on");
    CHECK(val(r, "PSPRECOMP_ADHOC_SERVER") == "127.0.0.1", "server");
    CHECK(val(r, "PSPRECOMP_ADHOC_NICK") == "Patapon", "nick");
    CHECK(val(r, "PSPRECOMP_ADHOC_RELAY") == "<unset>", "relay off -> unset");
    CHECK(val(r, "PSPRECOMP_INFRA_DIRECT") == "<unset>", "direct on (default) -> unset");
    CHECK(val(r, "PSPRECOMP_DISCORD") == "<unset>", "discord on (default) -> unset");
    CHECK(val(r, "PSPRECOMP_ADHOC_PORT_OFFSET") == "10000", "port offset");
    CHECK(val(r, "PSPRECOMP_ADHOC_MAC_FILE") == "C:/Juegos/Patapon 3/adhoc_mac.txt", "mac file");
}

static void custom_fixture() {
    const SettingsResolution r = psp_settings_resolve(slurp(kFix + "custom.ini"), kCatalog,
                                                      "D:/P3", kNoEnv);
    CHECK(val(r, "PSPRECOMP_UI_LANG") == "en", "en");
    CHECK(val(r, "PSPRECOMP_DISC0") == "D:/P3/../data/disc0", "relative with ..");
    CHECK(val(r, "PSPRECOMP_SAVEDATA") == "D:/P3/Partidas Guardadas", "spaces in value");
    CHECK(val(r, "PSPRECOMP_FULLSCREEN") == "<unset>", "fullscreen no -> unset");
    CHECK(val(r, "PSPRECOMP_WINDOW_MODE") == "borderless", "window mode borderless");
    CHECK(val(r, "PSPRECOMP_TEXTURES") == "<unset>", "hd off -> unset");
    CHECK(val(r, "PSPRECOMP_PATCHES") == "60fps", "fps 60 -> patch");
    CHECK(val(r, "PSPRECOMP_P3_ANIM_SMOOTH") == "0", "smooth false -> 0");
    CHECK(val(r, "PSPRECOMP_P3_FAST_LOAD") == "1", "TRUE accepted");
    CHECK(val(r, "PSPRECOMP_ADHOC_RELAY") == "1", "yes -> 1");
    CHECK(val(r, "PSPRECOMP_ADHOC_PORT_OFFSET") == "0", "offset 0");
}

static void invalid_fixture() {
    const SettingsResolution r = psp_settings_resolve(slurp(kFix + "invalid.ini"), kCatalog,
                                                      "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_FULLSCREEN") == "1", "bad bool -> default (true)");
    CHECK(logged(r, "display.fullscreen"), "bad bool logged");
    CHECK(val(r, "PSPRECOMP_WINDOW_MODE") == "<unset>", "bad window mode -> unset");
    CHECK(logged(r, "display.window_mode"), "bad window mode logged");
    CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "3", "last duplicate (3) wins");
    CHECK(val(r, "PSPRECOMP_WINDOW_SCALE") == "2", "bad int -> default");
    CHECK(val(r, "PSPRECOMP_PRESENT_FILTER") == "linear", "bad enum -> default");
    CHECK(val(r, "PSPRECOMP_PATCHES") == "<unset>", "fps 45 -> default 30 -> unset");
    CHECK(logged(r, "game.fps"), "bad fps logged");
    CHECK(logged(r, "game.unknown_key"), "unknown key logged");
    CHECK(logged(r, "nosuchsection"), "unknown section logged");
    CHECK(val(r, "PSPRECOMP_ADHOC_NICK") == "Patapon", "empty nick -> default");
    CHECK(val(r, "PSPRECOMP_ADHOC_PORT_OFFSET") == "10000", "offset out of range -> default");
    CHECK(logged(r, "garbage line"), "ignored line logged");
}

static void env_precedence() {
    std::map<std::string, std::string> env = {{"PSPRECOMP_RENDER_SCALE", "1"},
                                              {"PSPRECOMP_PATCHES", "60fps"}};
    const EnvGetter get = [&](const char* n) -> const char* {
        auto it = env.find(n);
        return it == env.end() ? nullptr : it->second.c_str();
    };
    const SettingsResolution r = psp_settings_resolve(slurp(kFix + "defaults.ini"), kCatalog,
                                                      "C:/P", get);
    CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "<unset>", "env already set: not touched");
    CHECK(val(r, "PSPRECOMP_PATCHES") == "<unset>", "env PATCHES already set: not touched");
    CHECK(logged(r, "PSPRECOMP_RENDER_SCALE already set"), "precedence logged");
}

static void future_and_empty() {
    SettingsResolution r = psp_settings_resolve(slurp(kFix + "future.ini"), kCatalog, "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "6", "known key of a future version applied");
    CHECK(logged(r, "version 2"), "future version warned");
    r = psp_settings_resolve("", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "4", "empty ini -> defaults");
    CHECK(val(r, "PSPRECOMP_PATCHES") == "<unset>", "no catalog -> no game keys");
    r = psp_settings_resolve("[display]\nrender_scale=2\n", "{not json", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_RENDER_SCALE") == "2", "bad catalog does not stop core keys");
    CHECK(logged(r, "game_options.json"), "bad catalog logged");
}

static void args_rules() {
    char a0[] = "game.exe", a1[] = "--settings", a2[] = "C:/P 3/settings.ini", a3[] = "--log",
         a4[] = "C:/P 3/logs/game.log", a5[] = "--unknown";
    char* argv[] = {a0, a1, a2, a3, a4, a5};
    SettingsArgs s = psp_settings_parse_args(6, argv);
    CHECK(s.settings_path == "C:/P 3/settings.ini", "--settings path (spaces)");
    CHECK(s.log_path == "C:/P 3/logs/game.log", "--log path");
    char* only[] = {a0, a1};
    s = psp_settings_parse_args(2, only);
    CHECK(s.settings_path.empty(), "--settings without a value is ignored");
}

// [launcher] belongs to the launcher (last played release): the game ignores it silently.
static void launcher_section_is_silent() {
    const SettingsResolution r = psp_settings_resolve("[launcher]\nversion = dxd-1.1.5b\n", kCatalog,
                                                      "C:/P", kNoEnv);
    CHECK(!logged(r, "launcher"), "[launcher] not logged as unknown");
}

static void controls_and_skin() {
    SettingsResolution r = psp_settings_resolve(
        "[controls]\ncross_keys = K, Return\nsquare_pad = y\nbogus_keys = J\n[display]\nosk_skin = patapon\n",
        "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_CONTROLS") == "cross=K, Return|a;square=A|y", "only named actions, other half default");
    CHECK(val(r, "PSPRECOMP_OSK_SKIN") == "C:/P/skins/osk/patapon.json", "skin path resolved");
    CHECK(logged(r, "unknown key controls.bogus_keys"), "unknown controls key logged");
    r = psp_settings_resolve("[display]\nosk_skin = ../x\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_OSK_SKIN") == "<unset>" && logged(r, "display.osk_skin"), "bad skin name refused");
    r = psp_settings_resolve("[display]\nrender_scale = 4\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_CONTROLS") == "<unset>", "no [controls] -> no variable");
    const EnvGetter env = [](const char* n) -> const char* {
        return std::string(n) == "PSPRECOMP_CONTROLS" ? "cross=X|a" : nullptr;
    };
    r = psp_settings_resolve("[controls]\ncross_keys = K\n", "", "C:/P", env);
    CHECK(val(r, "PSPRECOMP_CONTROLS") == "<unset>" && logged(r, "PSPRECOMP_CONTROLS already set"), "env wins");
}

// Help-bar button icons (game_options.json button_icons -> PSPRECOMP_BUTTON_ICONS, psp_prompt_atlas.h).
static void button_icons_key() {
    const SettingsResolution d = psp_settings_resolve(slurp(kFix + "defaults.ini"), kCatalog, "C:/P", kNoEnv);
    CHECK(val(d, "PSPRECOMP_BUTTON_ICONS") == "gray", "gray by default");
    const SettingsResolution c = psp_settings_resolve("[game]\nbutton_icons = color\n", kCatalog, "C:/P", kNoEnv);
    CHECK(val(c, "PSPRECOMP_BUTTON_ICONS") == "color", "color");
    const SettingsResolution bad = psp_settings_resolve("[game]\nbutton_icons = blue\n", kCatalog, "C:/P", kNoEnv);
    CHECK(val(bad, "PSPRECOMP_BUTTON_ICONS") == "gray" || val(bad, "PSPRECOMP_BUTTON_ICONS") == "<unset>",
          "an invalid value keeps the default");
}

static void achievements_keys() {
    const SettingsResolution d = psp_settings_resolve(slurp(kFix + "defaults.ini"), kCatalog,
                                                      "C:/Juegos/Patapon 3", kNoEnv);
    CHECK(val(d, "PSPRECOMP_ACHIEVEMENTS_FILE") == "C:/Juegos/Patapon 3/achievements/unlocked.json",
          "achievements file next to settings.ini");
    CHECK(val(d, "PSPRECOMP_ACHIEVEMENT_TOASTS") == "1", "toasts on by default");
    const SettingsResolution off = psp_settings_resolve("[game]\nachievement_toasts = false\n", kCatalog,
                                                        "C:/P", kNoEnv);
    CHECK(val(off, "PSPRECOMP_ACHIEVEMENT_TOASTS") == "0", "toasts off");
    const EnvGetter env = [](const char* n) -> const char* {
        return std::string(n) == "PSPRECOMP_ACHIEVEMENTS_FILE" ? "D:/x.json" : nullptr;
    };
    CHECK(val(psp_settings_resolve("", kCatalog, "C:/P", env), "PSPRECOMP_ACHIEVEMENTS_FILE") == "<unset>",
          "an environment value wins (nothing emitted)");
}

// display.window_mode -> PSPRECOMP_WINDOW_MODE; absent or invalid sets nothing, so the legacy
// display.fullscreen decides (psp_window_mode.h).
static void window_mode_key() {
    SettingsResolution r = psp_settings_resolve("[display]\nwindow_mode = borderless\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_WINDOW_MODE") == "borderless", "window_mode borderless");
    r = psp_settings_resolve("[display]\nfullscreen = false\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_WINDOW_MODE") == "<unset>", "absent window_mode sets nothing");
    CHECK(!logged(r, "display.window_mode"), "absent window_mode is not logged");
    r = psp_settings_resolve("[display]\nwindow_mode = foo\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_WINDOW_MODE") == "<unset>", "invalid window_mode sets nothing");
    CHECK(logged(r, "display.window_mode"), "invalid window_mode logged");
    CHECK(!logged(r, "unknown key"), "window_mode is a known key");
}

static void direct_key() {
    SettingsResolution r = psp_settings_resolve("[online]\ndirect = false\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_INFRA_DIRECT") == "0", "direct false -> 0");
    r = psp_settings_resolve("[online]\ndirect = true\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_INFRA_DIRECT") == "<unset>", "direct true -> unset");
    r = psp_settings_resolve("[online]\ndirect = maybe\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_INFRA_DIRECT") == "<unset>", "invalid -> default (on)");
    CHECK(logged(r, "online.direct"), "invalid direct logged");
    CHECK(!logged(r, "unknown key"), "direct is a known key");
}

static void stick_keys() {
    SettingsResolution r = psp_settings_resolve(
        "[controls]\nleft_stick = dpad\nright_stick = face\nstick_deadzone = 30\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_PAD_LEFT_STICK") == "dpad", "left_stick -> env");
    CHECK(val(r, "PSPRECOMP_PAD_RIGHT_STICK") == "face", "right_stick -> env");
    CHECK(val(r, "PSPRECOMP_PAD_DEADZONE") == "30", "stick_deadzone -> env");
    CHECK(!logged(r, "unknown key"), "stick keys are known");
    r = psp_settings_resolve("[controls]\nstick_deadzone = 7\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_PAD_DEADZONE") == "25", "out of range -> default 25");
    CHECK(logged(r, "controls.stick_deadzone"), "bad dead zone logged");
    r = psp_settings_resolve("", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_PAD_LEFT_STICK") == "off" && val(r, "PSPRECOMP_PAD_RIGHT_STICK") == "analog",
          "old ini without the keys: left off, right analog");
}

static void discord_key() {
    SettingsResolution r = psp_settings_resolve("[general]\ndiscord = false\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_DISCORD") == "0", "discord false -> 0");
    r = psp_settings_resolve("[general]\ndiscord = true\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_DISCORD") == "<unset>", "discord true -> unset");
    r = psp_settings_resolve("[general]\ndiscord = maybe\n", "", "C:/P", kNoEnv);
    CHECK(val(r, "PSPRECOMP_DISCORD") == "<unset>", "invalid -> default (on)");
    CHECK(logged(r, "general.discord"), "invalid discord logged");
    CHECK(!logged(r, "unknown key"), "discord is a known key");
}

int main() {
    window_mode_key();
    direct_key();
    discord_key();
    parser_rules();
    defaults_fixture();
    custom_fixture();
    invalid_fixture();
    env_precedence();
    future_and_empty();
    args_rules();
    launcher_section_is_silent();
    controls_and_skin();
    stick_keys();
    achievements_keys();
    button_icons_key();
    if (failures == 0) std::printf("test_settings: all passed\n");
    return failures == 0 ? 0 : 1;
}
