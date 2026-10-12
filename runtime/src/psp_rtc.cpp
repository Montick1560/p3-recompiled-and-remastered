#include "psp_rtc.h"

#include <cstdio>
#include <cstdlib>

namespace {

bool leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int days_in_month(int y, int m) {
    static const int table[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && leap_year(y)) return 29;
    return table[m - 1];
}

bool read_digits(const char*& p, int count, int* out) {
    int value = 0;
    for (int i = 0; i < count; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        value = value * 10 + (p[i] - '0');
    }
    p += count;
    *out = value;
    return true;
}

// Howard Hinnant's days_from_civil / civil_from_days (public domain), epoch
// 1970-01-01. Valid for the whole proleptic Gregorian calendar.
int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp + (mp < 10 ? 3 : -9);
    *y = year + (month <= 2);
    *m = month;
    *d = day;
}

}  // namespace

bool psp_rtc_parse(const char* text, PspRtcTime* out) {
    if (text == nullptr || out == nullptr) return false;
    const char* p = text;
    PspRtcTime t;
    if (!read_digits(p, 4, &t.year)) return false;
    if (*p++ != '-') return false;
    if (!read_digits(p, 2, &t.month)) return false;
    if (*p++ != '-') return false;
    if (!read_digits(p, 2, &t.day)) return false;
    if (*p != 'T' && *p != ' ') return false;
    p++;
    if (!read_digits(p, 2, &t.hour)) return false;
    if (*p++ != ':') return false;
    if (!read_digits(p, 2, &t.minute)) return false;
    if (*p++ != ':') return false;
    if (!read_digits(p, 2, &t.second)) return false;
    if (*p != '\0') return false;
    // Keep the value inside the unix-microsecond range the fixed clock uses.
    if (t.year < 1970 || t.year > 9999) return false;
    if (t.month < 1 || t.month > 12) return false;
    if (t.day < 1 || t.day > days_in_month(t.year, t.month)) return false;
    if (t.hour > 23 || t.minute > 59 || t.second > 59) return false;
    *out = t;
    return true;
}

int64_t psp_rtc_to_unix_us(const PspRtcTime& t) {
    const int64_t days =
        days_from_civil(t.year, static_cast<unsigned>(t.month),
                        static_cast<unsigned>(t.day));
    const int64_t secs =
        days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    return secs * 1000000;
}

PspRtcTime psp_rtc_from_unix_us(int64_t us) {
    int64_t secs = us / 1000000;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civil_from_days(days, &y, &m, &d);
    PspRtcTime t;
    t.year = y;
    t.month = static_cast<int>(m);
    t.day = static_cast<int>(d);
    t.hour = static_cast<int>(rem / 3600);
    t.minute = static_cast<int>((rem % 3600) / 60);
    t.second = static_cast<int>(rem % 60);
    return t;
}

std::optional<PspRtcTime> psp_rtc_env_override() {
    const char* text = std::getenv("PSPRECOMP_RTC_FIXED");
    if (text == nullptr || *text == '\0') return std::nullopt;
    PspRtcTime t;
    if (!psp_rtc_parse(text, &t)) {
        std::fprintf(stderr,
                     "[RTC] PSPRECOMP_RTC_FIXED=\"%s\" is not "
                     "YYYY-MM-DDTHH:MM:SS; ignoring\n",
                     text);
        return std::nullopt;
    }
    return t;
}

PspFixedClock::PspFixedClock(const PspRtcTime& base)
    : base_us_(psp_rtc_to_unix_us(base)),
      anchor_(std::chrono::steady_clock::now()) {}

PspRtcTime PspFixedClock::now() const {
    return psp_rtc_from_unix_us(now_unix_us());
}

int64_t PspFixedClock::now_unix_us() const {
    const int64_t elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - anchor_)
                                .count();
    return base_us_ + elapsed;
}
