// Discord presence runner: the send rule and the off switches (no pipe, no thread).
#include "psp_presence_run.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static nlohmann::json fake(const uint8_t*, const char*, int64_t) { return {{"details", "x"}}; }

static void should_send() {
    const nlohmann::json a = {{"details", "a"}}, b = {{"details", "b"}};
    CHECK(!psp_presence_should_send(a, a, 60000), "same activity");
    CHECK(!psp_presence_should_send(a, b, 14999), "changed at 14 999 ms");
    CHECK(psp_presence_should_send(a, b, 15000), "changed at 15 000 ms");
    CHECK(psp_presence_should_send(nlohmann::json(), a, 15000), "first activity");
}

static void stays_off() {
    psp_presence_register({nullptr, fake});
    CHECK(!psp_presence_start(nullptr, "es"), "no client id");
    psp_presence_register({"", fake});
    CHECK(!psp_presence_start(nullptr, "es"), "empty client id");
    psp_presence_register({"123", fake});
#ifdef _WIN32
    _putenv("PSPRECOMP_DISCORD=0");
#else
    setenv("PSPRECOMP_DISCORD", "0", 1);
#endif
    CHECK(!psp_presence_start(nullptr, "es"), "kill switch");
    psp_presence_stop();  // safe when never started
}

int main() {
    should_send();
    stays_off();
    if (failures) return 1;
    std::puts("presence_run: OK");
    return 0;
}
