// psp_log.h: per-frame / per-call diagnostic lines are off unless PSPRECOMP_LOG_VERBOSE=1.
#include "psp_log.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

int main() {
    CHECK(!psp_log_verbose_value(nullptr));
    CHECK(!psp_log_verbose_value(""));
    CHECK(!psp_log_verbose_value("0"));
    CHECK(psp_log_verbose_value("1"));
    // The test runs without the variable: the cached switch is off.
    CHECK(!psp_log_verbose());
    if (failures) return 1;
    std::printf("log_verbose: all passed\n");
    return 0;
}
