// psp_exit.h — exit-time handlers (GE summary, lookup-miss dump, PC trace) that run on both
// exit paths: std::exit (atexit) and the fast shutdown, which leaves with std::quick_exit so no
// static destructor runs under parked guest threads (at_quick_exit).
#pragma once

#include <cstdlib>

inline void psp_on_exit(void (*fn)()) {
    std::atexit(fn);
    std::at_quick_exit(fn);
}
