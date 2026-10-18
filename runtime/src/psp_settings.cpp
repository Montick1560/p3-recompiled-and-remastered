#include "psp_settings.h"
#include "psp_ini.h"
#include "psp_controls.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

enum class Kind { Bool, Int, Enum, String, Path };

struct CoreKey {
    const char* section;
    const char* key;
    const char* env;
    Kind kind;
    const char* def;
    int min, max;          // Int
    const char* values;    // Enum: comma list
};

// docs/contracts/settings-v1.md, core key table. display.texture_pack is handled with
// display.hd_textures.
const CoreKey kCore[] = {
    {"general", "language", "PSPRECOMP_UI_LANG", Kind::Enum, "es", 0, 0, "es,en"},
    {"paths", "disc0", "PSPRECOMP_DISC0", Kind::Path, "disc0", 0, 0, nullptr},
    {"paths", "savedata", "PSPRECOMP_SAVEDATA", Kind::Path, "savedata", 0, 0, nullptr},
    {"display", "fullscreen", "PSPRECOMP_FULLSCREEN", Kind::Bool, "true", 0, 0, nullptr},
    {"display", "window_scale", "PSPRECOMP_WINDOW_SCALE", Kind::Int, "2", 1, 8, nullptr},
    {"display", "render_scale", "PSPRECOMP_RENDER_SCALE", Kind::Int, "4", 1, 8, nullptr},
    {"display", "present_filter", "PSPRECOMP_PRESENT_FILTER", Kind::Enum, "linear", 0, 0, "linear,nearest"},
    {"display", "texture_filter", "PSPRECOMP_TEX_FILTER", Kind::Enum, "auto", 0, 0, "auto,nearest,linear"},
    {"display", "hd_textures", "PSPRECOMP_TEXTURES", Kind::Bool, "true", 0, 0, nullptr},
    {"display", "texture_pack", nullptr, Kind::Path, "textures/INFN00001", 0, 0, nullptr},
    {"online", "server", "PSPRECOMP_ADHOC_SERVER", Kind::String, "127.0.0.1", 0, 0, nullptr},
    {"online", "nickname", "PSPRECOMP_ADHOC_NICK", Kind::String, "Patapon", 0, 0, nullptr},
    {"online", "relay", "PSPRECOMP_ADHOC_RELAY", Kind::Bool, "false", 0, 0, nullptr},
    {"online", "port_offset", "PSPRECOMP_ADHOC_PORT_OFFSET", Kind::Int, "10000", 0, 65535, nullptr},
    {"online", "mac_file", "PSPRECOMP_ADHOC_MAC_FILE", Kind::Path, "adhoc_mac.txt", 0, 0, nullptr},
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "true"/"false" or "" when not a boolean.
std::string parse_bool(const std::string& v) {
    const std::string l = lower(v);
    if (l == "true" || l == "1" || l == "yes" || l == "on") return "true";
    if (l == "false" || l == "0" || l == "no" || l == "off") return "false";
    return "";
}

bool parse_int(const std::string& v, int min, int max, std::string* out) {
    if (v.empty()) return false;
    size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
    if (i == v.size()) return false;
    for (size_t k = i; k < v.size(); k++) if (!std::isdigit(static_cast<unsigned char>(v[k]))) return false;
    if (v.size() > 7) return false;
    const long n = std::strtol(v.c_str(), nullptr, 10);
    if (n < min || n > max) return false;
    *out = std::to_string(n);
    return true;
}

bool in_list(const std::string& v, const std::string& list) {
    size_t a = 0;
    while (a <= list.size()) {
        size_t c = list.find(',', a);
        if (c == std::string::npos) c = list.size();
        if (list.compare(a, c - a, v) == 0) return true;
        a = c + 1;
    }
    return false;
}

bool is_absolute(const std::string& p) {
    return (!p.empty() && (p[0] == '/' || p[0] == '\\')) ||
           (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':');
}

std::string resolve_path(const std::string& base, const std::string& p) {
    if (is_absolute(p) || base.empty()) return p;
    return base + "/" + p;
}

// Validates `raw` for a key of kind/def/min/max/values; returns the normalized value or the
// default, logging a line when it falls back.
std::string validate(const std::string& where, const std::string* raw, Kind kind,
                     const std::string& def, int min, int max, const std::string& values,
                     std::vector<std::string>* log) {
    if (raw == nullptr) return def;
    std::string out;
    switch (kind) {
    case Kind::Bool:
        out = parse_bool(*raw);
        if (!out.empty()) return out;
        break;
    case Kind::Int:
        if (parse_int(*raw, min, max, &out)) return out;
        break;
    case Kind::Enum:
        if (in_list(*raw, values)) return *raw;
        break;
    case Kind::String:
    case Kind::Path:
        if (!raw->empty()) return *raw;
        break;
    }
    log->push_back("[SETTINGS] " + where + ": invalid value \"" + *raw + "\", using \"" + def + "\"");
    return def;
}

}  // namespace

SettingsResolution psp_settings_resolve(const std::string& ini_text, const std::string& catalog_json,
                                        const std::string& base_dir, const EnvGetter& getenv_fn) {
    SettingsResolution r;
    const IniDoc ini = psp_ini_parse(ini_text);
    for (const std::string& w : ini.warnings) r.log.push_back("[SETTINGS] " + w);
    if (const std::string* v = ini.get("meta", "version")) {
        if (*v != "1") r.log.push_back("[SETTINGS] settings.ini version " + *v +
                                       " is newer than 1: applying the keys this build knows");
    }
    std::set<std::string> known = {"meta.version"};
    auto emit = [&](const char* env, const std::string& value, bool unset) {
        if (getenv_fn(env) != nullptr) {
            r.log.push_back(std::string("[SETTINGS] ") + env + " already set in the environment, kept");
            return;
        }
        if (!unset) r.set.emplace_back(env, value);
    };

    std::string hd, pack;
    for (const CoreKey& k : kCore) {
        known.insert(std::string(k.section) + "." + k.key);
        const std::string where = std::string(k.section) + "." + k.key;
        const std::string v = validate(where, ini.get(k.section, k.key), k.kind, k.def, k.min, k.max,
                                       k.values ? k.values : "", &r.log);
        if (std::string(k.key) == "hd_textures") { hd = v; continue; }
        if (std::string(k.key) == "texture_pack") { pack = v; continue; }
        if (k.kind == Kind::Bool) emit(k.env, "1", v != "true");
        else if (k.kind == Kind::Path) emit(k.env, resolve_path(base_dir, v), false);
        else emit(k.env, v, false);
    }
    emit("PSPRECOMP_TEXTURES", resolve_path(base_dir, pack), hd != "true");
    // Achievements (docs/contracts/achievements-v1.md): the earned list lives next to settings.ini.
    emit("PSPRECOMP_ACHIEVEMENTS_FILE", resolve_path(base_dir, "achievements/unlocked.json"), false);

    if (!catalog_json.empty()) {
        nlohmann::json cat = nlohmann::json::parse(catalog_json, nullptr, false);
        if (cat.is_discarded() || !cat.contains("options") || !cat["options"].is_array()) {
            r.log.push_back("[SETTINGS] game_options.json is not valid JSON: game options skipped");
        } else {
            for (const auto& o : cat["options"]) {
                const std::string section = o.value("section", ""), key = o.value("key", "");
                const std::string type = o.value("type", "string"), def = o.value("default", "");
                known.insert(section + "." + key);
                const Kind kind = type == "bool" ? Kind::Bool : type == "int" ? Kind::Int
                                : type == "enum" ? Kind::Enum : type == "path" ? Kind::Path : Kind::String;
                std::string values;
                if (o.contains("values") && o["values"].is_array())
                    for (const auto& x : o["values"]) values += (values.empty() ? "" : ",") + x.get<std::string>();
                const std::string v = validate(section + "." + key, ini.get(section, key), kind, def,
                                               o.value("min", 0), o.value("max", 0), values, &r.log);
                const std::string env = o.value("env", "");
                if (env.empty()) continue;
                if (o.contains("map") && o["map"].is_object()) {
                    const auto& m = o["map"];
                    if (!m.contains(v) || m[v].is_null()) { emit(env.c_str(), "", true); continue; }
                    emit(env.c_str(), m[v].get<std::string>(), false);
                } else {
                    emit(env.c_str(), kind == Kind::Path ? resolve_path(base_dir, v) : v, false);
                }
            }
        }
    }

    // [controls] -> PSPRECOMP_CONTROLS, validated by the runtime's ControlMap (psp_controls.h): only
    // the actions the file names, the missing half of an action keeps its default.
    {
        std::string spec;
        for (const ControlAction& a : control_actions()) {
            const std::string kk = std::string(a.name) + "_keys", pk = std::string(a.name) + "_pad";
            known.insert("controls." + kk);
            known.insert("controls." + pk);
            const std::string* keys = ini.get("controls", kk);
            const std::string* pad = ini.get("controls", pk);
            if (keys == nullptr && pad == nullptr) continue;
            spec += (spec.empty() ? "" : ";") + std::string(a.name) + "=" + (keys ? *keys : a.keys) + "|" +
                    (pad ? *pad : a.pad);
        }
        if (!spec.empty()) emit("PSPRECOMP_CONTROLS", spec, false);
    }
    // [display] osk_skin -> PSPRECOMP_OSK_SKIN (<ini folder>/skins/osk/<name>.json).
    known.insert("display.osk_skin");
    if (const std::string* skin = ini.get("display", "osk_skin")) {
        const bool ok = !skin->empty() && std::all_of(skin->begin(), skin->end(), [](unsigned char c) {
            return std::islower(c) || std::isdigit(c) || c == '_' || c == '-';
        });
        if (ok) emit("PSPRECOMP_OSK_SKIN", resolve_path(base_dir, "skins/osk/" + *skin + ".json"), false);
        else r.log.push_back("[SETTINGS] display.osk_skin: invalid name \"" + *skin + "\", using the built-in skin");
    }

    for (const auto& s : ini.values) {
        if (s.first == "launcher") continue;  // launcher-only section (docs/contracts/settings-v1.md)
        for (const auto& kv : s.second) {
            const std::string id = s.first + "." + kv.first;
            if (!known.count(id)) r.log.push_back("[SETTINGS] unknown key " + id + " ignored");
        }
        if (s.second.empty() && s.first != "controls")
            r.log.push_back("[SETTINGS] empty section [" + s.first + "]");
    }
    return r;
}

SettingsArgs psp_settings_parse_args(int argc, char** argv) {
    SettingsArgs a;
    for (int i = 1; i + 1 < argc; i++) {
        const std::string k = argv[i];
        if (k == "--settings") a.settings_path = argv[++i];
        else if (k == "--log") a.log_path = argv[++i];
    }
    return a;
}

namespace {
std::string read_file(const std::string& path, bool* ok) {
    FILE* f = std::fopen(path.c_str(), "rb");
    *ok = f != nullptr;
    if (!f) return "";
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
std::string dir_of(const std::string& path) {
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? "." : path.substr(0, p);
}
std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}
void set_env(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}
}  // namespace

void psp_settings_apply(const SettingsArgs& args, const char* exe_dir) {
    if (!args.log_path.empty()) {
        if (std::freopen(args.log_path.c_str(), "w", stderr) == nullptr)
            std::fprintf(stdout, "[SETTINGS] cannot open log %s\n", args.log_path.c_str());
    }
    const std::string exe = slashes(exe_dir ? exe_dir : "");
    std::string ini_path = slashes(args.settings_path);
    bool ok = false;
    std::string text;
    if (!ini_path.empty()) {
        text = read_file(ini_path, &ok);
        if (!ok) {
            std::fprintf(stderr, "[SETTINGS] %s not found: using the runtime defaults\n", ini_path.c_str());
            return;
        }
    } else {
        ini_path = exe + "/settings.ini";
        text = read_file(ini_path, &ok);
        if (!ok) return;  // no settings.ini: everything as before
    }
    bool cat_ok = false;
    const std::string catalog = read_file(exe + "/game_options.json", &cat_ok);
    std::fprintf(stderr, "[SETTINGS] reading %s (game options: %s)\n", ini_path.c_str(),
                 cat_ok ? "game_options.json" : "none");
    const SettingsResolution r = psp_settings_resolve(text, catalog, dir_of(ini_path),
                                                      [](const char* n) { return std::getenv(n); });
    for (const std::string& l : r.log) std::fprintf(stderr, "%s\n", l.c_str());
    for (const auto& kv : r.set) {
        set_env(kv.first, kv.second);
        std::fprintf(stderr, "[SETTINGS] %s=%s\n", kv.first.c_str(), kv.second.c_str());
    }
}
