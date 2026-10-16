// psp_exit.h: exit-time diagnostics (GE summary, lookup-miss dump) must also run on the fast
// shutdown path, which leaves with std::quick_exit (no static destructors under parked threads).
#include "psp_exit.h"

#include <cstdio>
#include <cstring>

static void handler() { std::fprintf(stderr, "on_exit handler ran\n"); }

int main(int argc, char** argv) {
    psp_on_exit(handler);
    if (argc > 1 && std::strcmp(argv[1], "--quick") == 0) std::quick_exit(0);
    return 0;  // normal exit: atexit
}
