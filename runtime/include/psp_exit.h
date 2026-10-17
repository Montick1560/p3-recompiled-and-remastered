// psp_exit.h — exit-time handlers (GE summary, lookup-miss dump, PC trace) that run on both
// exit paths: std::exit (atexit) and the fast shutdown, which leaves with std::quick_exit so no
// static destructor runs under parked guest threads (at_quick_exit).
#pragma once

#include <atomic>
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
