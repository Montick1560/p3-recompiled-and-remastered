#pragma once
// settings.ini v1 (docs/contracts/settings-v1.md): turns the ini keys (core keys + the game's
// game_options.json catalog) into the PSPRECOMP_* environment variables the runtime already
// reads. An environment variable that is already set always wins. Pure: no getenv / setenv here.
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct SettingsResolution {
    std::vector<std::pair<std::string, std::string>> set;  // env name -> value, in order
    std::vector<std::string> log;                           // "[SETTINGS] ..." lines
};

using EnvGetter = std::function<const char*(const char*)>;

SettingsResolution psp_settings_resolve(const std::string& ini_text,
                                        const std::string& catalog_json,  // "" = none
                                        const std::string& base_dir,      // ini folder, '/'
                                        const EnvGetter& getenv_fn);

// Command line: --settings <path>, --log <path> (both optional, "" = not given).
struct SettingsArgs { std::string settings_path, log_path; };
SettingsArgs psp_settings_parse_args(int argc, char** argv);  // pure

// Redirects stderr to args.log_path when given, finds settings.ini (args path, else next to
// the executable), resolves it with game_options.json next to the executable and sets the
// environment. No ini and no --settings: does nothing.
void psp_settings_apply(const SettingsArgs& args, const char* exe_dir);
