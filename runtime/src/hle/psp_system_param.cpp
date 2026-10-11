#include "hle/psp_system_param.h"

namespace {
constexpr int32_t kErrInvalidSystemParamId = static_cast<int32_t>(0x80110103u);
}  // namespace

int32_t psp_system_param_int(int32_t id, int32_t* value) {
    int32_t v = 0;
    switch (id) {
        case 2: v = 0; break;   // ADHOC_CHANNEL: automatic
        case 3: v = 0; break;   // WLAN_POWERSAVE: off
        case 4: v = 0; break;   // DATE_FORMAT: YYYYMMDD
        case 5: v = 0; break;   // TIME_FORMAT: 24 h
        case 6: v = 0; break;   // TIMEZONE: minutes from UTC
        case 7: v = 0; break;   // DAYLIGHTSAVINGS: standard time
        case 8: v = 1; break;   // LANGUAGE: English
        case 9: v = 1; break;   // BUTTON_PREFERENCE: CROSS confirms
        case 10: v = 0; break;  // LOCK_PARENTAL_LEVEL
        default: return kErrInvalidSystemParamId;
    }
    *value = v;
    return 0;
}
