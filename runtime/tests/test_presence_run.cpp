// Discord presence runner: the send rule and the off switches (no pipe, no thread).
#include "psp_presence_run.h"

#include <chrono>
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

static const np2::Party kParty{0x1000000773ULL, 2, 4};

static void with_party_adds_size_and_id() {
    const nlohmann::json a = psp_presence_with_party(
        {{"details", "Taterazay \xC2\xB7 Nivel 22"}, {"state", "En el escondite"}}, kParty, "es");
    CHECK(a["party"]["id"] == "room-68719478643", "party id is the room id");
    CHECK(a["party"]["size"] == nlohmann::json::array({2, 4}), "party size [size, max]");
    CHECK(a["state"] == "En el escondite", "an existing state is kept");
}

static void with_party_adds_generic_state() {
    const nlohmann::json d = {{"details", "En enfrentamiento"}};
    CHECK(psp_presence_with_party(d, kParty, "es")["state"] == "En l\xC3\xADnea", "generic state, es");
    CHECK(psp_presence_with_party(d, kParty, "en")["state"] == "Online", "generic state, en");
}

static void with_party_null_activity() {
    CHECK(psp_presence_with_party(nullptr, kParty, "es").is_null(), "nothing to show stays null");
}

static void stop_is_bounded() {
    const auto t0 = std::chrono::steady_clock::now();
    psp_presence_stop();  // never started: nothing to wait for
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1500, "stop returns within 1.5 s");
}

int main() {
    should_send();
    stays_off();
    with_party_adds_size_and_id();
    with_party_adds_generic_state();
    with_party_null_activity();
    stop_is_bounded();
    if (failures) return 1;
    std::puts("presence_run: OK");
    return 0;
}
