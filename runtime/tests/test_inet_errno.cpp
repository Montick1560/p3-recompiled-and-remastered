// inet_errno: PSP sceNetInet errno -> sceNetInetGetPspError value, after
// PPSSPP convertInetErrno2PSPError (Core/HLE/NetInetConstants.cpp).
#include "net/inet_errno.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

int main() {
    CHECK(inet_errno_to_psp(inet_errno::kEAGAIN) == 0x8001000BU, "EAGAIN -> RESOURCE_UNAVAILABLE");
    CHECK(inet_errno::kEAGAIN == 11, "EAGAIN is 11 on the PSP");
    CHECK(inet_errno_to_psp(inet_errno::kENOTCONN) == 0x80010080U, "ENOTCONN -> NOT_CONNECTED");
    CHECK(inet_errno_to_psp(inet_errno::kECONNRESET) == 0x80010068U, "ECONNRESET -> CONNECTION_RESET");
    CHECK(inet_errno_to_psp(inet_errno::kENETUNREACH) == 0x80010013U, "ENETUNREACH -> DEVICE_NOT_FOUND");
    CHECK(inet_errno_to_psp(inet_errno::kEINVAL) == 0x80010016U, "EINVAL -> INVALID_ARGUMENT");
    CHECK(inet_errno_to_psp(inet_errno::kEPROTONOSUPPORT) == 0x8001B000U, "EPROTONOSUPPORT -> FUNCTION_NOT_SUPPORTED");
    CHECK(inet_errno_to_psp(inet_errno::kEINPROGRESS) == 0x80010077U, "EINPROGRESS -> IN_PROGRESS (game: pending)");
    CHECK(inet_errno_to_psp(inet_errno::kEALREADY) == 0x80010078U, "EALREADY -> ALREADY (game: pending)");
    CHECK(inet_errno_to_psp(inet_errno::kEISCONN) == 0x8001007FU, "EISCONN -> IS_ALREADY_CONNECTED (game: connected)");
    CHECK(inet_errno_to_psp(inet_errno::kECONNREFUSED) == 0x80010011U, "ECONNREFUSED as PPSSPP");
    CHECK(inet_errno_to_psp(inet_errno::kEPIPE) == 0x80010002U, "EPIPE as PPSSPP");
    CHECK(inet_errno_to_psp(inet_errno::kEBADF) == 9, "EBADF is not mapped by PPSSPP: passes through");
    CHECK(inet_errno_to_psp(0) == 0, "no error");
    CHECK(inet_errno_to_psp(77) == 77, "unknown passes through");
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("inet_errno: all tests passed\n");
    return 0;
}
