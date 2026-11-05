#include "psp_present_aa.h"

#include <cctype>
#include <string>

PspAa psp_aa_parse(const char* env) {
    if (env == nullptr) return PspAa::Off;
    std::string v(env);
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "fxaa") return PspAa::Fxaa;
    if (v == "smaa") return PspAa::Smaa;
    return PspAa::Off;
}
