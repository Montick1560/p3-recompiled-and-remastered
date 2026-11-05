// psp_icon_strip_load: the PNG strip a world tag's icon comes from (Patapon 3 ping signal icon).
#include "psp_icon_strip.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    IconStrip s;
    CHECK(!psp_icon_strip_load("no/such/ping_signal.png", 8, &s) && s.frames == 0, "icon_missing_file_is_refused");
    CHECK(!psp_icon_strip_load(PSP_PING_ICON_PNG, 3, &s), "icon_bad_width_is_refused (448 px is not 3 frames)");
    CHECK(!psp_icon_strip_load(PSP_PING_ICON_PNG, 0, &s), "zero frames refused");
    CHECK(!psp_icon_strip_load(__FILE__, 8, &s), "not a PNG refused");
    CHECK(psp_icon_strip_load(PSP_PING_ICON_PNG, 8, &s), "icon_shipped_png_loads");
    CHECK(s.w == 448 && s.h == 48 && s.frames == 8 && s.rgba.size() == 448u * 48u * 4u, "8 frames of 56x48");
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("world_tag_icon: all tests passed\n");
    return 0;
}
