#pragma once
// Deterministic wall-clock override for automated visual runs.
// The guest reads the host clock once at boot (sceRtcGetCurrentClockLocalTime)
// and the Patapon hideout sky/lighting follows that value, so two runs of the
// same build at different host hours differ far past the rt_regress gate
// (measured: same-hour pairs <= 0.05 % struct; a 18:33 vs 20:41 pair up to
// 54.8 %). PSPRECOMP_RTC_FIXED pins the reported clock to a fixed base that
// keeps advancing in real time, making captures reproducible at any host
// time. Default off: the real host clock is reported, as on a real PSP.
// Read semantics reference: PPSSPP Core/HLE/sceRtc.cpp
// (sceRtcGetCurrentClockLocalTime / __RtcGetCurrentTick).
#include <chrono>
#include <cstdint>
#include <optional>

struct PspRtcTime {
    int year = 0;
    int month = 0;  // 1..12
    int day = 0;    // 1..31, validated against the month
    int hour = 0;   // 0..23
    int minute = 0;
    int second = 0;
};

// Strict `YYYY-MM-DDTHH:MM:SS` (a space instead of 'T' is also accepted).
// Field ranges are validated (leap years included); no timezone conversion:
// the fields are the local civil time the guest clock shows.
bool psp_rtc_parse(const char* text, PspRtcTime* out);

// Civil <-> unix microseconds, host-timezone independent (public-domain
// days-from-civil arithmetic, Howard Hinnant).
int64_t psp_rtc_to_unix_us(const PspRtcTime& t);
PspRtcTime psp_rtc_from_unix_us(int64_t us);

// PSPRECOMP_RTC_FIXED from the environment. nullopt when unset; an invalid
// value warns on stderr and is treated as unset. Reads the environment every
// call (cheap, once per process in practice) so tests can exercise it.
std::optional<PspRtcTime> psp_rtc_env_override();

// Fixed clock: `now()` is `base` plus the steady time since construction, so
// guest code that measures time still sees it pass, but every run of the same
// build observes the same wall time at the same point of its boot sequence.
class PspFixedClock {
public:
    explicit PspFixedClock(const PspRtcTime& base);
    PspRtcTime now() const;
    int64_t now_unix_us() const;

private:
    int64_t base_us_;
    std::chrono::steady_clock::time_point anchor_;
};
