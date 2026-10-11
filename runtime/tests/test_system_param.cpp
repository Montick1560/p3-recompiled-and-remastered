// psp_system_param: sceUtilityGetSystemParamInt IDs and values (PPSSPP sceUtility.cpp).
#include "hle/psp_system_param.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } } while (0)

static int32_t value_of(int32_t id) {
    int32_t v = -12345;
    return psp_system_param_int(id, &v) == 0 ? v : -12345;
}

int main() {
    CHECK(value_of(2) == 0, "ADHOC_CHANNEL (2) = automatic");
    CHECK(value_of(3) == 0, "WLAN_POWERSAVE (3) = off");
    CHECK(value_of(4) == 0, "DATE_FORMAT (4) = YYYYMMDD");
    CHECK(value_of(5) == 0, "TIME_FORMAT (5) = 24 h");
    CHECK(value_of(6) == 0, "TIMEZONE (6) = UTC offset 0");
    CHECK(value_of(7) == 0, "DAYLIGHTSAVINGS (7) = standard time");
    CHECK(value_of(8) == 1, "LANGUAGE (8) = English");
    CHECK(value_of(9) == 1, "BUTTON_PREFERENCE (9) = CROSS confirms");
    CHECK(value_of(10) == 0, "LOCK_PARENTAL_LEVEL (10) = 0");
    int32_t v = 77;
    CHECK(psp_system_param_int(1, &v) == static_cast<int32_t>(0x80110103u) && v == 77,
          "NICKNAME (1) is a string param: invalid id, nothing written");
    CHECK(psp_system_param_int(11, &v) == static_cast<int32_t>(0x80110103u), "unknown id");
    if (failures == 0) std::printf("system_param tests passed\n");
    return failures == 0 ? 0 : 1;
}
