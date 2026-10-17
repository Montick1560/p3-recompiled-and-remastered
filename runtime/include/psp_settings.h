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
