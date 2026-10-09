// Modified for psprecomp: PPSSPP's Common/Log.h logging is replaced by a
// stderr sink that stays silent unless PSPRECOMP_ATRAC_LOG is set in the
// environment (corrupt frames would otherwise spam once per decoded frame).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "compat.h"

void av_log(int level, const char *fmt, ...) {
	static const bool enabled = std::getenv("PSPRECOMP_ATRAC_LOG") != nullptr;
	if (!enabled || level > AV_LOG_WARNING) {
		return;
	}
	char buffer[512];
	va_list vl;
	va_start(vl, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, vl);
	va_end(vl);
	fprintf(stderr, "Atrac3/3+: %s", buffer);
}
