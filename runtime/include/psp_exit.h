// psp_exit.h — exit-time handlers (GE summary, lookup-miss dump, PC trace) that run on both
// exit paths: std::exit (atexit) and the fast shutdown, which leaves with std::quick_exit so no
// static destructor runs under parked guest threads (at_quick_exit).
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>

inline void psp_on_exit(void (*fn)()) {
    std::atexit(fn);
    std::at_quick_exit(fn);
}

// Process exit code main() ends with: 0 = quit, 10 = back to the launcher menu
// (docs/contracts/settings-v1.md).
inline std::atomic<int>& psp_requested_exit_code() {
    static std::atomic<int> code{0};
    return code;
}

extern std::atomic<bool> g_should_exit;  // psp_event_loop.cpp

// End the session and go back to the launcher menu (exit code 10): F1 -> Yes, or the game's
// own "save and quit" (sceKernelExitGame).
inline void psp_request_back_to_launcher(const char* why) {
    std::fprintf(stderr, "[RT] back to the launcher menu (exit code 10): %s\n", why);
    psp_requested_exit_code().store(10);
    g_should_exit.store(true);
}
