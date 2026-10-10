xxHash - Extremely Fast Hash algorithm (Yann Collet), BSD 2-Clause licence
(see the header comment in xxhash.h). Copied from PPSSPP's
ext/xxhash.{h,c} (DECO/tools/ppsspp-src), the same version PPSSPP uses for
texture-replacement hashing, so our keys match PPSSPP texture packs.
Version: 0.8.4. Two PPSSPP build-integration lines were removed (the
`#include "ppsspp_config.h"` and an ARM-only __builtin_prefetch hint in
XXH32's loop); neither changes any hash value.
