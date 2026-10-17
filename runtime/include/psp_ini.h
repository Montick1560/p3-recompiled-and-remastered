#pragma once
// settings.ini v1 parser (docs/contracts/settings-v1.md, "Parsing"). Pure: text in, values out.
#include <map>
#include <string>
#include <vector>

struct IniDoc {
    std::map<std::string, std::map<std::string, std::string>> values;  // lowercased names
    std::vector<std::string> warnings;                                  // per ignored line
    const std::string* get(const std::string& section, const std::string& key) const;
};

IniDoc psp_ini_parse(const std::string& text);
