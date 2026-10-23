// psp_beta: the ISO UTC end date parse and the expiry rule of beta builds.
#include "psp_beta.h"

#include <cstdint>
#include <cstdio>

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

int main() {
    parse_utc();
    expired_after_until();
    empty_never_expires();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("beta: all tests passed\n");
    return 0;
}
