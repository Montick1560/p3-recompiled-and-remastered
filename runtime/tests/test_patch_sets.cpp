// psp_patch_select: PSPRECOMP_PATCHES parsing for the opt-in patch sets;
// VblankPacer: the pad-update pacing the 60fps set pairs with.
#include "psp_patch_sets.h"
#include "psp_vblank_pacer.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    const std::vector<std::string> names = {"fps60", "debug_menu"};

    PatchSelection none = psp_patch_select(nullptr, names);
    CHECK(none.enabled.empty() && none.unknown.empty(), "null env selects nothing");
    none = psp_patch_select("", names);
    CHECK(none.enabled.empty() && none.unknown.empty(), "empty env selects nothing");

    PatchSelection one = psp_patch_select("fps60", names);
    CHECK(one.enabled.size() == 1 && one.enabled[0] == 0, "single name");
    CHECK(one.unknown.empty(), "single name: nothing unknown");

    PatchSelection two = psp_patch_select("debug_menu, fps60", names);
    CHECK(two.enabled.size() == 2 && two.enabled[0] == 1 && two.enabled[1] == 0,
          "comma + space separated, request order kept");

    PatchSelection rep = psp_patch_select("fps60;fps60,,fps60", names);
    CHECK(rep.enabled.size() == 1, "repeats and empty items ignored");

    PatchSelection bad = psp_patch_select("fps60,FPS60,nope", names);
    CHECK(bad.enabled.size() == 1 && bad.enabled[0] == 0, "exact match only");
    CHECK(bad.unknown.size() == 2 && bad.unknown[0] == "FPS60" && bad.unknown[1] == "nope",
          "unknown names reported in order");

    PatchSelection empty = psp_patch_select("fps60", {});
    CHECK(empty.enabled.empty() && empty.unknown.size() == 1, "output without sets");

    // VblankPacer: one run per 2 vblanks at 60 fps, every frame at 30 or slower.
    VblankPacer p60;
    const int64_t f60[] = {10, 11, 12, 13, 14, 15};
    const bool want60[] = {true, false, true, false, true, false};
    for (int i = 0; i < 6; i++) CHECK(p60.due(f60[i], 2) == want60[i], "60 fps: every other frame");
    VblankPacer p30;
    for (int64_t v = 100; v < 110; v += 2) CHECK(p30.due(v, 2), "30 fps: every frame");
    VblankPacer pj;  // jitter: frames 1 and 2 vblanks apart
    CHECK(pj.due(0, 2) && !pj.due(1, 2) && pj.due(3, 2) && !pj.due(4, 2) && pj.due(5, 2),
          "jitter: a run waits for 2 vblanks since the last run");
    VblankPacer pf;
    CHECK(pf.due(0, 2), "first call always runs, even at vblank 0");

    if (failures == 0) std::printf("patch_sets: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
