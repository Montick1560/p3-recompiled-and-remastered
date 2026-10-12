// Deterministic wall-clock override for automated visual runs (T7):
// parse/format helpers and the fixed clock behind PSPRECOMP_RTC_FIXED
// (runtime/src/psp_rtc.cpp). Header-only pure logic plus a getenv read.
#include "psp_rtc.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static void parse_accepts_strict_iso() {
    PspRtcTime t{};
    CHECK(psp_rtc_parse("2026-10-11T18:32:05", &t), "parse: ISO T separator");
    CHECK(t.year == 2026 && t.month == 10 && t.day == 11
          && t.hour == 18 && t.minute == 32 && t.second == 5, "parse: fields");
    CHECK(psp_rtc_parse("2026-10-11 18:32:05", &t), "parse: space separator");
    CHECK(t.year == 2026 && t.hour == 18, "parse: space separator fields");
}

static void parse_rejects_bad_input() {
    PspRtcTime t{};
    CHECK(!psp_rtc_parse(nullptr, &t), "parse: null");
    CHECK(!psp_rtc_parse("", &t), "parse: empty");
    CHECK(!psp_rtc_parse("2026-10-11", &t), "parse: missing time");
    CHECK(!psp_rtc_parse("2026-13-01T00:00:00", &t), "parse: month 13");
    CHECK(!psp_rtc_parse("2026-00-01T00:00:00", &t), "parse: month 0");
    CHECK(!psp_rtc_parse("2026-10-32T00:00:00", &t), "parse: day 32");
    CHECK(!psp_rtc_parse("2026-10-11T24:00:00", &t), "parse: hour 24");
    CHECK(!psp_rtc_parse("2026-10-11T18:60:05", &t), "parse: minute 60");
    CHECK(!psp_rtc_parse("2026-10-11T18:32:60", &t), "parse: second 60");
    CHECK(!psp_rtc_parse("2026-10-11T18:32:05Z", &t), "parse: trailing junk");
    CHECK(!psp_rtc_parse("26-10-11T18:32:05", &t), "parse: short year");
    CHECK(!psp_rtc_parse("2026-02-29T00:00:00", &t), "parse: non-leap Feb 29");
    CHECK(psp_rtc_parse("2024-02-29T00:00:00", &t), "parse: leap Feb 29");
    CHECK(!psp_rtc_parse("2026-04-31T00:00:00", &t), "parse: April 31");
}

static void unix_epoch_anchors() {
    PspRtcTime t{};
    CHECK(psp_rtc_parse("1970-01-01T00:00:00", &t), "epoch parse");
    CHECK(psp_rtc_to_unix_us(t) == 0, "epoch is 0 us");
    // 2001-01-01T00:00:00Z is the PSP epoch: 978307200 unix seconds.
    CHECK(psp_rtc_parse("2001-01-01T00:00:00", &t), "PSP epoch parse");
    CHECK(psp_rtc_to_unix_us(t) == 978307200LL * 1000000, "PSP epoch us");
}

static void civil_round_trip() {
    PspRtcTime t{};
    CHECK(psp_rtc_parse("2026-10-11T18:32:05", &t), "round trip parse");
    const int64_t us = psp_rtc_to_unix_us(t);
    const PspRtcTime back = psp_rtc_from_unix_us(us);
    CHECK(back.year == t.year && back.month == t.month && back.day == t.day
          && back.hour == t.hour && back.minute == t.minute
          && back.second == t.second, "round trip fields");
    // One second earlier and later stay exact too (calendar arithmetic).
    const PspRtcTime before = psp_rtc_from_unix_us(us - 1000000);
    const PspRtcTime after = psp_rtc_from_unix_us(us + 1000000);
    CHECK(before.second == t.second - 1, "second before");
    CHECK(after.second == t.second + 1, "second after");
    // A leap day and a year rollover.
    PspRtcTime leap{};
    CHECK(psp_rtc_parse("2024-02-29T23:59:59", &leap), "leap parse");
    const PspRtcTime next = psp_rtc_from_unix_us(psp_rtc_to_unix_us(leap) + 1000000);
    CHECK(next.year == 2024 && next.month == 3 && next.day == 1
          && next.hour == 0 && next.minute == 0 && next.second == 0,
          "leap day rollover");
}

static void fixed_clock_starts_at_base_and_advances() {
    PspRtcTime base{};
    CHECK(psp_rtc_parse("2026-10-11T18:32:05", &base), "clock base parse");
    PspFixedClock clock(base);
    const int64_t us0 = clock.now_unix_us();
    const int64_t base_us = psp_rtc_to_unix_us(base);
    CHECK(us0 >= base_us && us0 <= base_us + 10000,
          "clock starts at base (us-scale setup time)");
    const PspRtcTime now0 = clock.now();
    CHECK(now0.year == base.year && now0.hour == base.hour
          && now0.minute == base.minute && now0.second == base.second,
          "now() civil fields at base");
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    const int64_t us1 = clock.now_unix_us();
    CHECK(us1 >= us0 + 10000, "clock advances with real time");
    CHECK(us1 <= us0 + 200000, "clock stays close (15 ms sleep)");
}

static void env_override_reads_and_validates() {
#ifdef _WIN32
    _putenv_s("PSPRECOMP_RTC_FIXED", "2026-10-11T18:32:05");
#else
    setenv("PSPRECOMP_RTC_FIXED", "2026-10-11T18:32:05", 1);
#endif
    auto ok = psp_rtc_env_override();
    CHECK(ok.has_value(), "env: valid value parsed");
    if (ok) CHECK(ok->year == 2026 && ok->minute == 32, "env: fields");
#ifdef _WIN32
    _putenv_s("PSPRECOMP_RTC_FIXED", "not-a-date");
#else
    setenv("PSPRECOMP_RTC_FIXED", "not-a-date", 1);
#endif
    CHECK(!psp_rtc_env_override().has_value(), "env: invalid value rejected");
#ifdef _WIN32
    _putenv_s("PSPRECOMP_RTC_FIXED", "");
#else
    unsetenv("PSPRECOMP_RTC_FIXED");
#endif
    CHECK(!psp_rtc_env_override().has_value(), "env: unset means no override");
}

int main() {
    parse_accepts_strict_iso();
    parse_rejects_bad_input();
    unix_epoch_anchors();
    civil_round_trip();
    fixed_clock_starts_at_base_and_advances();
    env_override_reads_and_validates();
    if (failures == 0) std::printf("test_rtc_fixed: all passed\n");
    return failures == 0 ? 0 : 1;
}
