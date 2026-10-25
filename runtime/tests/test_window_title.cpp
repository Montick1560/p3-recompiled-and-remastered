// Unit test for psp_window_title.h: the game window's title is PSPRECOMP_WINDOW_TITLE when the build names one
// (EXPECT_TITLE carries the same text into this test), else the generic "PSPrecomp Runtime".
#include "psp_window_title.h"

#include <cstdio>
#include <cstring>

int main() {
#ifdef EXPECT_TITLE
    const char* want = EXPECT_TITLE;
#else
    const char* want = "PSPrecomp Runtime";
#endif
    if (std::strcmp(psp_window_title(), want) != 0) {
        std::fprintf(stderr, "FAIL: title '%s', want '%s'\n", psp_window_title(), want);
        return 1;
    }
    std::printf("window title: %s\n", psp_window_title());
    return 0;
}
