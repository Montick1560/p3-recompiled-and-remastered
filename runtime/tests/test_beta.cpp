// psp_beta: the ISO UTC end date parse and the expiry rule of beta builds.
#include "psp_beta.h"

#include <cstdint>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void parse_utc() {
    int64_t t = 0;
    CHECK(psp_beta_parse_utc("2026-10-26T05:59:59Z", &t) && t == 1792994399, "end of beta 1");
    CHECK(psp_beta_parse_utc("1970-01-01T00:00:00Z", &t) && t == 0, "epoch");
    CHECK(psp_beta_parse_utc("2028-02-29T12:00:00Z", &t) && t == 1835438400, "leap day");
    CHECK(!psp_beta_parse_utc("2026-10-26T05:59:59", &t), "no Z: not UTC");
    CHECK(!psp_beta_parse_utc("2026-10-26 05:59:59Z", &t) || t == 1792994399, "space form, if accepted, same value");
    CHECK(!psp_beta_parse_utc("2026-13-01T00:00:00Z", &t), "month 13");
    CHECK(!psp_beta_parse_utc("ayer", &t), "text");
    CHECK(!psp_beta_parse_utc("", &t), "empty");
    CHECK(!psp_beta_parse_utc(nullptr, &t), "null");
}

static void expired_after_until() {
    const char* until = "2026-10-26T05:59:59Z";
    CHECK(psp_beta_expired(1792994399 + 1, until), "one second after the end");
    CHECK(!psp_beta_expired(1792994399, until), "the last second still plays");
    CHECK(!psp_beta_expired(1792994399 - 1, until), "before the end");
}

static void empty_never_expires() {
    CHECK(!psp_beta_expired(4102444800, ""), "empty until");
    CHECK(!psp_beta_expired(4102444800, nullptr), "null until");
    CHECK(!psp_beta_expired(4102444800, "not a date"), "invalid until never blocks");
}

static void beta_builds_have_no_debug_socket() {
    CHECK(psp_beta_debug_socket_allowed(""), "normal build: debug socket as before");
    CHECK(psp_beta_debug_socket_allowed(nullptr), "null until: normal build");
    CHECK(!psp_beta_debug_socket_allowed("2026-10-26T05:59:59Z"), "beta build: no debug socket");
}

static void beta_builds_drop_the_debug_menu() {
    const char* until = "2026-10-26T05:59:59Z";
    CHECK(psp_beta_filter_patches("60fps,debug_menu", until) == "60fps", "beta: debug_menu dropped");
    CHECK(psp_beta_filter_patches("debug_menu", until) == "", "beta: only debug_menu -> nothing");
    CHECK(psp_beta_filter_patches("debug_menu,60fps", until) == "60fps", "beta: first in the list");
    CHECK(psp_beta_filter_patches(nullptr, until) == "", "beta: unset");
    CHECK(psp_beta_filter_patches("60fps,debug_menu", "") == "60fps,debug_menu", "normal build: unchanged");
    CHECK(psp_beta_filter_patches("60fps", until) == "60fps", "beta: others stay");
}

int main() {
    parse_utc();
    beta_builds_have_no_debug_socket();
    beta_builds_drop_the_debug_menu();
    expired_after_until();
    empty_never_expires();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("beta: all tests passed\n");
    return 0;
}
