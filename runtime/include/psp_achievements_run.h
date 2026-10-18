#pragma once
// The achievements system in a running game (spec 2026-10-18 part 5b §2, §5). The game module registers
// its sources (one per release) in register_hooks; main() starts the system after that and stops it at
// shutdown. Off unless PSPRECOMP_ACHIEVEMENTS_FILE is set; PSPRECOMP_ACHIEVEMENTS=0 is the kill switch.
#include "psp_achievements.h"
#include "psp_toast.h"

#include <cstddef>
#include <string>

struct PspAchRunOptions {
    std::string exe_dir;                       // holds achievements.json and release.json
    void (*post_toast)(const ToastText&);      // psp_toast_post in the game; a recorder in tests
};

void psp_achievements_register(const PspAchievementSource* sources, size_t n);
// Reads the catalogue, the release id and unlocked.json, compiles the pending checks, starts the
// worker and installs the vblank hook. Returns false (one [ACH] line) when achievements stay off.
bool psp_achievements_start(const PspAchRunOptions& opt);
// Uninstalls the hook, lets the worker write what is queued, joins it. Safe to call when not started.
void psp_achievements_stop();
