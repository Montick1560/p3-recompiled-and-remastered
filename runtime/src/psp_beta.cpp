#include "psp_beta.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <SDL.h>

#include "psp_rtc.h"

#ifndef PSPRECOMP_BETA_UNTIL
#define PSPRECOMP_BETA_UNTIL ""
#endif

bool psp_beta_parse_utc(const char* iso, int64_t* unix_s) {
    if (!iso || std::strlen(iso) != 20 || iso[19] != 'Z' || iso[10] != 'T') return false;
    PspRtcTime t;
    if (!psp_rtc_parse(std::string(iso, 19).c_str(), &t)) return false;
    *unix_s = psp_rtc_to_unix_us(t) / 1000000;
    return true;
}

bool psp_beta_expired(int64_t now_unix_s, const char* until_iso) {
    int64_t until = 0;
    return psp_beta_parse_utc(until_iso, &until) && now_unix_s > until;
}

const char* psp_beta_until() { return PSPRECOMP_BETA_UNTIL; }

bool psp_beta_debug_socket_allowed(const char* until_iso) { return !until_iso || !until_iso[0]; }

std::string psp_beta_filter_patches(const char* patches, const char* until_iso) {
    std::string in = patches ? patches : "";
    if (psp_beta_debug_socket_allowed(until_iso)) return in;  // not a beta build
    std::string out;
    size_t start = 0;
    while (start <= in.size()) {
        size_t end = in.find(',', start);
        if (end == std::string::npos) end = in.size();
        const std::string name = in.substr(start, end - start);
        if (!name.empty() && name != "debug_menu") out += (out.empty() ? "" : ",") + name;
        start = end + 1;
    }
    return out;
}

int psp_beta_check_at_start() {
    const char* until = psp_beta_until();
    if (!until[0]) return 0;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (!psp_beta_expired(now, until)) {
        std::fprintf(stderr, "[RT] beta build: plays until %s\n", until);
        return 0;
    }
    std::fprintf(stderr, "[RT] beta build ended at %s: not starting (exit %d)\n", until, kPspBetaEndedExit);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Patapon 3",
                             "La beta de Patapon 3 termin\xC3\xB3 el domingo 25 de octubre a las 23:59 (hora CDMX). "
                             "Espera la siguiente ronda.",
                             nullptr);
    return kPspBetaEndedExit;
}
