#pragma once
// sceUtilityGetSystemParamInt values (pure, unit-tested). IDs follow pspsdk /
// PPSSPP sceUtility.h: 2 ADHOC_CHANNEL .. 10 LOCK_PARENTAL_LEVEL; 1 is the
// nickname, a string parameter.
#include <cstdint>

// Writes the value of integer system parameter `id` to *value and returns 0,
// or returns SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID (0x80110103) and
// leaves *value untouched.
int32_t psp_system_param_int(int32_t id, int32_t* value);
