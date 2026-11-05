// psp_present_aa.h: PSPRECOMP_AA parsing (off | fxaa | smaa).
#include "psp_present_aa.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

int main() {
    CHECK(psp_aa_parse("fxaa") == PspAa::Fxaa, "fxaa");
    CHECK(psp_aa_parse("FXAA") == PspAa::Fxaa, "any case");
    CHECK(psp_aa_parse("smaa") == PspAa::Smaa, "smaa");
    CHECK(psp_aa_parse("Smaa") == PspAa::Smaa, "Smaa");
    CHECK(psp_aa_parse("off") == PspAa::Off, "off");
    CHECK(psp_aa_parse("") == PspAa::Off && psp_aa_parse(nullptr) == PspAa::Off, "empty -> off");
    CHECK(psp_aa_parse("msaa") == PspAa::Off, "unknown -> off");
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_present_aa: all passed\n");
    return 0;
}
