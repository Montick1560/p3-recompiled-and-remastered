#pragma once
// Opt-in patch sets: `[[patch_sets]]` in games/<id>/game.toml, emitted by the
// recompiler into the output's patch_sets.cpp as alternate function bodies.
// PSPRECOMP_PATCHES=<name>[,<name>...] registers a set's bodies over the
// dispatch entries at boot (main.cpp), so a set that is not named costs
// nothing and the default build runs the unpatched game.
#include <string>
#include <vector>

// Generated (patch_sets.cpp in the recompiler output).
int recomp_patch_set_count();
const char* recomp_patch_set_name(int i);
int recomp_patch_set_enable(int i);  // returns the number of entries registered

// Runtime (psp_patch_sets.cpp). Apply once, right after the dispatch table is
// built and before the HLE layer and game hooks wrap entries; game modules ask
// psp_patch_set_active() to pair a set with the host-side help it needs.
void psp_patch_sets_apply(const char* env);
bool psp_patch_set_active(const char* name);

struct PatchSelection {
    std::vector<int> enabled;           // indices into `names`, in request order
    std::vector<std::string> unknown;   // requested names the output does not have
};

// Splits `env` on commas, spaces and semicolons; matches names exactly;
// ignores repeats. A null or empty `env` selects nothing.
inline PatchSelection psp_patch_select(const char* env, const std::vector<std::string>& names) {
    PatchSelection sel;
    if (!env) return sel;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        int found = -1;
        for (size_t i = 0; i < names.size(); i++) {
            if (names[i] == cur) found = static_cast<int>(i);
        }
        if (found < 0) {
            sel.unknown.push_back(cur);
        } else {
            bool seen = false;
            for (int e : sel.enabled) seen = seen || e == found;
            if (!seen) sel.enabled.push_back(found);
        }
        cur.clear();
    };
    for (const char* p = env; *p; p++) {
        if (*p == ',' || *p == ' ' || *p == ';') flush();
        else cur.push_back(*p);
    }
    flush();
    return sel;
}
