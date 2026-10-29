#include "net/secure_random.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#endif

namespace net {

bool secure_random(uint8_t* out, size_t len) {
#ifdef _WIN32
    while (len > 0) {  // ULONG-sized calls
        const ULONG n = len > 0x10000000 ? 0x10000000 : static_cast<ULONG>(len);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return false;
        out += n;
        len -= n;
    }
#else
    while (len > 0) {  // getentropy takes at most 256 bytes
        const size_t n = len > 256 ? 256 : len;
        if (getentropy(out, n) != 0) return false;
        out += n;
        len -= n;
    }
#endif
    return true;
}

}  // namespace net
