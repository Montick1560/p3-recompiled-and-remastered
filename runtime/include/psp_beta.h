#pragma once
// Beta builds of the online mode end on a date built in at configure time
// (CMake cache string PSPRECOMP_BETA_UNTIL, ISO UTC "YYYY-MM-DDTHH:MM:SSZ").
// Unset (the default) = no limit. After the date the exe shows a Spanish
// message and exits with kPspBetaEndedExit, which the launcher maps to its
// "the beta ended" screen. The server's window is the real lock; this only
// stops normal use of an old beta package.
#include <cstdint>

constexpr int kPspBetaEndedExit = 12;

// Strict "YYYY-MM-DDTHH:MM:SSZ" (UTC) -> unix seconds. false on anything else.
bool psp_beta_parse_utc(const char* iso, int64_t* unix_s);

// true when now is after until_iso; false when until_iso is empty or invalid.
bool psp_beta_expired(int64_t now_unix_s, const char* until_iso);

// The built-in end date ("" when not a beta build).
const char* psp_beta_until();

// 0, or shows the "beta ended" message box and returns kPspBetaEndedExit.
int psp_beta_check_at_start();
