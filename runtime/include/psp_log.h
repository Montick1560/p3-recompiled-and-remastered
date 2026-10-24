#pragma once
// Per-frame and per-call diagnostic lines (GE enqueue, frame-buffer swaps, async IO, ATRAC decodes)
// are off by default so a player's log keeps the lines a diagnosis needs; PSPRECOMP_LOG_VERBOSE=1
// restores every one of them.

#include <cstdlib>

inline bool psp_log_verbose_value(const char* v) { return v && v[0] == '1'; }

inline bool psp_log_verbose() {
    static const bool on = psp_log_verbose_value(std::getenv("PSPRECOMP_LOG_VERBOSE"));
    return on;
}
