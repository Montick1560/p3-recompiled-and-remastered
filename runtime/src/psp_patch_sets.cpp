// PSPRECOMP_PATCHES: registers the opt-in patch sets of the recompiler
// output over the dispatch table (see psp_patch_sets.h).
#include "psp_patch_sets.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_active;  // written once at boot, read-only afterwards
}

void psp_patch_sets_apply(const char* env) {
    std::vector<std::string> names;
    for (int i = 0; i < recomp_patch_set_count(); i++) names.emplace_back(recomp_patch_set_name(i));
    const PatchSelection sel = psp_patch_select(env, names);
    for (int i : sel.enabled) {
        const int n = recomp_patch_set_enable(i);
        g_active.push_back(names[i]);
        std::fprintf(stderr, "[RT] Patch set '%s' enabled (%d entries)\n", names[i].c_str(), n);
    }
    std::string have;
    for (const std::string& n : names) have += " " + n;
    for (const std::string& u : sel.unknown) {
        std::fprintf(stderr, "[RT] WARNING: PSPRECOMP_PATCHES names unknown set '%s' (have:%s)\n",
                     u.c_str(), have.empty() ? " none" : have.c_str());
    }
}

bool psp_patch_set_active(const char* name) {
    for (const std::string& a : g_active) {
        if (a == name) return true;
    }
    return false;
}
