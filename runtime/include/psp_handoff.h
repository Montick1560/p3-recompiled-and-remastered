// psp_handoff.h: leaving for the launcher menu without showing the desktop. A launcher that sets PSPRECOMP_HANDOFF prepares its
// window while the game shuts down, and the game's window stays on screen until that window is up (docs/contracts/settings-v1.md).
#pragma once

#include <chrono>

// Tells the launcher the game is leaving. False when no launcher takes part: the game behaves as if this did not exist.
bool psp_handoff_begin();

// Waits for the launcher, at most `limit`. True when a handoff was under way: the caller then lets go of its window.
bool psp_handoff_finish(std::chrono::milliseconds limit);
